#include "ma/Ducker.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kControlBlock = 16; // samples between filter updates
constexpr double kKneeDb = 6.0;   // full depth this far over the threshold
constexpr double kPi = 3.14159265358979323846;

Biquad bandPass(double sampleRate, double hz, double q)
{
    const double w = 2.0 * kPi * hz / sampleRate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad f;
    f.b0 = alpha / a0;
    f.b1 = 0.0;
    f.b2 = -alpha / a0;
    f.a1 = -2.0 * std::cos(w) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

double coefficient(double ms, double sampleRate)
{
    return std::exp(-1.0 / (std::max(ms, 0.05) * 0.001 * sampleRate));
}

void copyCoefficients(Biquad& target, const Biquad& source)
{
    target.b0 = source.b0;
    target.b1 = source.b1;
    target.b2 = source.b2;
    target.a1 = source.a1;
    target.a2 = source.a2;
}
} // namespace

void Ducker::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    updateDetector();
    reset();
}

void Ducker::reset()
{
    for (auto& f : detector)
        f.reset();
    for (auto& f : dip)
        f = Biquad {};
    envelope = 0.0;
    lastReductionDb = 0.0;
    peakReductionDb = 0.0;
    sidechainLevelDb = -200.0;
    appliedGain = 1.0;
}

void Ducker::setSettings(const DuckSettings& newSettings)
{
    const bool detectorChanged = std::abs(newSettings.hz - settings.hz) > 1e-3f || std::abs(newSettings.q - settings.q) > 1e-4f
                                 || std::abs(newSettings.wholeSignal - settings.wholeSignal) > 0.5f;
    const bool timesChanged = std::abs(newSettings.attackMs - settings.attackMs) > 1e-4f
                              || std::abs(newSettings.releaseMs - settings.releaseMs) > 1e-4f;
    settings = newSettings;
    if (detectorChanged)
        updateDetector();
    else if (timesChanged)
    {
        attackCoeff = coefficient(settings.attackMs, sampleRate);
        releaseCoeff = coefficient(settings.releaseMs, sampleRate);
    }
}

void Ducker::updateDetector()
{
    const double hz = std::clamp(static_cast<double>(settings.hz), 20.0, 0.45 * sampleRate);
    for (auto& f : detector)
        copyCoefficients(f, bandPass(sampleRate, hz, std::max(0.3, static_cast<double>(settings.q))));
    attackCoeff = coefficient(settings.attackMs, sampleRate);
    releaseCoeff = coefficient(settings.releaseMs, sampleRate);
}

void Ducker::process(float* const* main, int numMain, const float* const* sidechain, int numSidechain, int numSamples)
{
    const bool active = settings.on > 0.5f && sidechain != nullptr && numSidechain > 0;
    peakReductionDb = 0.0;
    if (!active)
    {
        lastReductionDb = 0.0;
        envelope = 0.0;
        sidechainLevelDb = -200.0;
        appliedGain = 1.0;
        return;
    }

    const bool whole = settings.wholeSignal > 0.5f;
    const int channels = std::min(numMain, kMaxChannels);
    double peakEnvelope = 0.0;

    for (int start = 0; start < numSamples; start += kControlBlock)
    {
        const int n = std::min(kControlBlock, numSamples - start);

        // Detector: the sidechain in the same band (whole mode listens to all of it).
        for (int i = start; i < start + n; ++i)
        {
            double x = 0.0;
            for (int c = 0; c < numSidechain; ++c)
                x += sidechain[c][i];
            x /= numSidechain;
            if (!whole)
                x = detector[1].process(detector[0].process(x));
            const double level = std::abs(x);
            const double coeff = level > envelope ? attackCoeff : releaseCoeff;
            envelope = coeff * envelope + (1.0 - coeff) * level;
            peakEnvelope = std::max(peakEnvelope, envelope);
        }

        const double envelopeDb = 20.0 * std::log10(std::max(envelope * std::sqrt(2.0), 1e-10));
        const double amount = std::clamp((envelopeDb - settings.thresholdDb) / kKneeDb, 0.0, 1.0);
        lastReductionDb = amount * settings.depthDb;
        peakReductionDb = std::max(peakReductionDb, lastReductionDb);

        if (whole)
        {
            const double target = std::pow(10.0, -lastReductionDb / 20.0);
            const double step = (target - appliedGain) / n;
            for (int i = start; i < start + n; ++i)
            {
                appliedGain += step;
                for (int c = 0; c < channels; ++c)
                    main[c][i] = static_cast<float>(main[c][i] * appliedGain);
            }
        }
        else
        {
            const auto bell = Biquad::peaking(sampleRate, std::clamp(static_cast<double>(settings.hz), 20.0, 0.45 * sampleRate),
                                              -lastReductionDb, std::max(0.3, static_cast<double>(settings.q)));
            for (int c = 0; c < channels; ++c)
            {
                auto& f = dip[static_cast<size_t>(c)];
                copyCoefficients(f, bell);
                for (int i = start; i < start + n; ++i)
                    main[c][i] = static_cast<float>(f.process(main[c][i]));
            }
        }
    }
    sidechainLevelDb = 20.0 * std::log10(std::max(peakEnvelope * std::sqrt(2.0), 1e-10));
}

} // namespace ma

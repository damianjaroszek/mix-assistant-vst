#include "ma/Harmonics.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDrive = 2.0;
constexpr double kBias = 0.3; // asymmetry: even harmonics as well as odd
constexpr int kErbBands = 40;

Biquad butterworth(double sampleRate, double hz, bool highPass)
{
    const double k = std::tan(kPi * std::min(hz, 0.45 * sampleRate) / sampleRate);
    const double q = 1.0 / std::sqrt(2.0);
    const double norm = 1.0 / (1.0 + k / q + k * k);
    Biquad f;
    if (highPass)
    {
        f.b0 = norm;
        f.b1 = -2.0 * norm;
        f.b2 = norm;
    }
    else
    {
        f.b0 = k * k * norm;
        f.b1 = 2.0 * f.b0;
        f.b2 = f.b0;
    }
    f.a1 = 2.0 * (k * k - 1.0) * norm;
    f.a2 = (1.0 - k / q + k * k) * norm;
    return f;
}

void setCoefficients(Biquad& target, const Biquad& source)
{
    target.b0 = source.b0;
    target.b1 = source.b1;
    target.b2 = source.b2;
    target.a1 = source.a1;
    target.a2 = source.a2;
}

// Power shares of the harmonics 2..6 the shaper makes from a sine (measured once; the 3rd dominates).
constexpr int kHarmonics = 5;
constexpr double kHarmonicShare[kHarmonics] = { 0.133, 0.741, 0.042, 0.066, 0.018 };
} // namespace

double HarmonicEnhancer::shape(double x)
{
    return std::tanh(kDrive * x + kBias) - std::tanh(kBias);
}

void HarmonicEnhancer::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    envelopeCoeff = std::exp(-1.0 / (0.05 * sampleRate));

    // Calibrate on a sine an octave under the split: everything scales with hz, so once is enough.
    settings.hz = 100.0f;
    updateFilters();
    reset();
    State probe = state[0];
    double sourceSum = 0.0, harmonicSum = 0.0;
    const int n = static_cast<int>(0.5 * sampleRate);
    for (int i = 0; i < n; ++i)
    {
        const double x = 0.5 * std::sin(2.0 * kPi * 50.0 * i / sampleRate);
        const double source = probe.low[1].process(probe.low[0].process(x));
        probe.meanSquare = envelopeCoeff * probe.meanSquare + (1.0 - envelopeCoeff) * source * source;
        const double rms = std::sqrt(probe.meanSquare);
        const double shaped = rms > 1e-9 ? shape(source / rms) * rms : 0.0;
        const double harmonics = probe.high[1].process(probe.high[0].process(shaped));
        if (i > n / 2)
        {
            sourceSum += source * source;
            harmonicSum += harmonics * harmonics;
        }
    }
    calibration = sourceSum > 0.0 && harmonicSum > 0.0 ? std::sqrt(harmonicSum / sourceSum) : 1.0;
    reset();
}

void HarmonicEnhancer::reset()
{
    for (auto& s : state)
    {
        for (int i = 0; i < 2; ++i)
        {
            s.low[i].reset();
            s.high[i].reset();
        }
        s.meanSquare = 0.0;
    }
}

void HarmonicEnhancer::setSettings(const HarmonicSettings& newSettings)
{
    const bool moved = std::abs(newSettings.hz - settings.hz) > 1e-3f;
    settings = newSettings;
    if (moved)
        updateFilters();
}

void HarmonicEnhancer::updateFilters()
{
    const double hz = std::clamp(static_cast<double>(settings.hz), 20.0, 0.4 * sampleRate);
    const auto low = butterworth(sampleRate, hz, false);
    const auto high = butterworth(sampleRate, hz, true);
    for (auto& s : state)
        for (int i = 0; i < 2; ++i)
        {
            setCoefficients(s.low[i], low);
            setCoefficients(s.high[i], high);
        }
}

void HarmonicEnhancer::process(float* const* channels, int numChannels, int numSamples)
{
    if (settings.on < 0.5f)
        return;
    const double gain = std::pow(10.0, settings.amountDb / 20.0) / calibration;
    for (int c = 0; c < std::min(numChannels, kMaxChannels); ++c)
    {
        auto& s = state[static_cast<size_t>(c)];
        float* data = channels[c];
        for (int i = 0; i < numSamples; ++i)
        {
            const double x = data[i];
            const double source = s.low[1].process(s.low[0].process(x));
            s.meanSquare = envelopeCoeff * s.meanSquare + (1.0 - envelopeCoeff) * source * source;
            const double rms = std::sqrt(s.meanSquare);
            const double shaped = rms > 1e-9 ? shape(source / rms) * rms : 0.0;
            const double harmonics = s.high[1].process(s.high[0].process(shaped));
            data[i] = static_cast<float>(x + gain * harmonics);
        }
    }
}

SpectrumAnalyzer::Bands predictHarmonics(const SpectrumAnalyzer::Bands& bandsDb, const HarmonicSettings& settings)
{
    auto out = bandsDb;
    if (settings.on < 0.5f)
        return out;
    const double hz = settings.hz;
    const auto lowPass = butterworth(48000.0, hz, false);
    const auto highPass = butterworth(48000.0, hz, true);
    double reference = 0.0; // what passes for the calibration sine, an octave under the split
    for (int h = 0; h < kHarmonics; ++h)
        reference += kHarmonicShare[h] * std::pow(10.0, 2.0 * highPass.magnitudeDb(0.5 * hz * (h + 2), 48000.0) / 10.0);
    const double amount = std::pow(10.0, settings.amountDb / 10.0) / std::max(reference, 1e-6);

    std::array<double, SpectrumAnalyzer::kMaxBands> added {};
    for (int b = 0; b < kErbBands; ++b)
    {
        const double f = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        const double source = std::pow(10.0, (bandsDb[static_cast<size_t>(b)] + 2.0 * lowPass.magnitudeDb(f, 48000.0)) / 10.0);
        if (source < 1e-15)
            continue;
        // Shares of the harmonics that make it through the high-pass, rescaled like the calibration
        // (which counts what passes for a source an octave under the split).
        for (int h = 0; h < kHarmonics; ++h)
        {
            const double fh = f * (h + 2);
            if (fh > 20000.0)
                break;
            const double pass = std::pow(10.0, 2.0 * highPass.magnitudeDb(fh, 48000.0) / 10.0);
            const double power = source * amount * kHarmonicShare[h] * pass;
            int target = 0;
            while (target + 1 < kErbBands && SpectrumAnalyzer::bandEdgeHz(BandScale::erb, target + 1) <= fh)
                ++target;
            added[static_cast<size_t>(target)] += power;
        }
    }
    for (int b = 0; b < kErbBands; ++b)
    {
        const double base = std::pow(10.0, bandsDb[static_cast<size_t>(b)] / 10.0);
        out[static_cast<size_t>(b)] = 10.0 * std::log10(std::max(base + added[static_cast<size_t>(b)], 1e-20));
    }
    return out;
}

} // namespace ma

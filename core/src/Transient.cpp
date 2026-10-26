#include "ma/Transient.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeadZoneDb = 3.0;  // ripple of a steady sound between the two envelopes
constexpr double kFullLiftDb = 10.0; // fast envelope this far over the slow one = full lift

double coefficient(double ms, double sampleRate)
{
    return std::exp(-1.0 / (ms * 0.001 * sampleRate));
}

Biquad highPassFilter(double sampleRate, double hz)
{
    const double k = std::tan(kPi * std::min(hz, 0.45 * sampleRate) / sampleRate);
    const double q = 1.0 / std::sqrt(2.0);
    const double norm = 1.0 / (1.0 + k / q + k * k);
    Biquad f;
    f.b0 = norm;
    f.b1 = -2.0 * norm;
    f.b2 = norm;
    f.a1 = 2.0 * (k * k - 1.0) * norm;
    f.a2 = (1.0 - k / q + k * k) * norm;
    return f;
}
} // namespace

void TransientEnhancer::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    fastAttack = coefficient(0.3, sampleRate);
    fastRelease = coefficient(15.0, sampleRate);
    slowAttack = coefficient(20.0, sampleRate);
    slowRelease = coefficient(150.0, sampleRate);
    updateFilter();
    reset();
}

void TransientEnhancer::reset()
{
    for (auto& f : highPass)
        f.reset();
    fast = slow = 0.0;
    peakBoostDb = 0.0;
}

void TransientEnhancer::setSettings(const TransientSettings& newSettings)
{
    const bool moved = std::abs(newSettings.hz - settings.hz) > 1e-3f;
    settings = newSettings;
    if (moved)
        updateFilter();
}

void TransientEnhancer::updateFilter()
{
    const auto f = highPassFilter(sampleRate, std::clamp(static_cast<double>(settings.hz), 100.0, 0.4 * sampleRate));
    for (auto& h : highPass)
    {
        h.b0 = f.b0;
        h.b1 = f.b1;
        h.b2 = f.b2;
        h.a1 = f.a1;
        h.a2 = f.a2;
    }
}

void TransientEnhancer::process(float* const* channels, int numChannels, int numSamples)
{
    peakBoostDb = 0.0;
    if (settings.on < 0.5f || numChannels <= 0)
        return;
    const int count = std::min(numChannels, kMaxChannels);
    const double maxLift = std::pow(10.0, settings.amountDb / 20.0) - 1.0;
    for (int i = 0; i < numSamples; ++i)
    {
        double level = 0.0;
        for (int c = 0; c < count; ++c)
            level = std::max(level, static_cast<double>(std::abs(channels[c][i])));
        fast = (level > fast ? fastAttack : fastRelease) * (fast - level) + level;
        slow = (level > slow ? slowAttack : slowRelease) * (slow - level) + level;
        const double overDb = 20.0 * std::log10(std::max(fast, 1e-9) / std::max(slow, 1e-9));
        const double amount = std::clamp((overDb - kDeadZoneDb) / (kFullLiftDb - kDeadZoneDb), 0.0, 1.0);
        const double lift = amount * maxLift;
        if (lift > 0.0)
            peakBoostDb = std::max(peakBoostDb, 20.0 * std::log10(1.0 + lift));
        for (int c = 0; c < count; ++c)
        {
            const double x = channels[c][i];
            const double high = highPass[static_cast<size_t>(c)].process(x);
            channels[c][i] = static_cast<float>(x + lift * high);
        }
    }
}

SpectrumAnalyzer::Bands predictTransients(const SpectrumAnalyzer::Bands& bandsDb, const TransientSettings& settings, double onsetShare)
{
    auto out = bandsDb;
    if (settings.on < 0.5f)
        return out;
    const auto filter = highPassFilter(48000.0, settings.hz);
    const double gain = std::pow(10.0, settings.amountDb / 20.0) - 1.0;
    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
    {
        const double response = std::pow(10.0, filter.magnitudeDb(SpectrumAnalyzer::bandCentreHz(BandScale::erb, b), 48000.0) / 20.0);
        const double lifted = (1.0 + gain * response) * (1.0 + gain * response);
        out[static_cast<size_t>(b)] += 10.0 * std::log10(1.0 + onsetShare * (lifted - 1.0));
    }
    return out;
}

} // namespace ma

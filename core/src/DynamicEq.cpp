#include "ma/DynamicEq.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kControlBlock = 16;
constexpr double kDetectorMs = 5.0;     // mean square of band and whole part, before attack and release
constexpr double kRiderLevelMs = 100.0; // the rider reads a 100 ms level, as the Learn percentiles
constexpr double kRiderGateDb = 24.0;   // quieter than target minus this is a pause: the gain holds
constexpr double kSilentPower = 1e-7;   // -70 dBFS: nothing to detect
constexpr double kRatio = 3.0;

double coefficient(double ms, double samples, double sampleRate)
{
    return std::exp(-samples / (std::max(ms, 0.05) * 0.001 * sampleRate));
}

double toDb(double power)
{
    return power > 1e-20 ? 10.0 * std::log10(power) : -200.0;
}
} // namespace

double dynamicProbeHz(int probe)
{
    constexpr double hz[kDynamicBands] = { 250.0, 3200.0, 6800.0 };
    return hz[std::clamp(probe, 0, kDynamicBands - 1)];
}

double dynamicProbeQ(int probe)
{
    constexpr double q[kDynamicBands] = { 1.4, 1.4, 1.2 };
    return q[std::clamp(probe, 0, kDynamicBands - 1)];
}

DynamicBand dynamicProbeBand(int probe)
{
    DynamicBand b;
    b.hz = static_cast<float>(dynamicProbeHz(probe));
    b.q = static_cast<float>(dynamicProbeQ(probe));
    const auto which = static_cast<DynamicProbe>(std::clamp(probe, 0, kDynamicBands - 1));
    b.attackMs = which == DynamicProbe::sibilance ? 1.0f : which == DynamicProbe::harsh ? 5.0f : 10.0f;
    b.releaseMs = which == DynamicProbe::sibilance ? 60.0f : which == DynamicProbe::harsh ? 120.0f : 150.0f;
    return b;
}

double dynamicBandCutDb(const DynamicBand& band, double excessDb)
{
    if (!dynamicBandOn(band) || excessDb <= 0.0)
        return 0.0;
    return std::min(static_cast<double>(band.rangeDb), excessDb * (1.0 - 1.0 / kRatio));
}

void DynamicEq::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    powerCoeff = coefficient(kDetectorMs, 1.0, sampleRate);
    riderPowerCoeff = coefficient(kRiderLevelMs, 1.0, sampleRate);
    reset();
    detectorsStale = true;
    setSettings(settings);
}

void DynamicEq::reset()
{
    for (auto& d : detector)
        d.reset();
    for (auto& channel : cut)
        for (auto& f : channel)
            f = Biquad {};
    bandPower.fill(0.0);
    cutDb.fill(0.0);
    peakCut.fill(0.0);
    wholePower = riderPower = 0.0;
    riderGain = 0.0;
    sinceUpdate = 0;
}

void DynamicEq::setSettings(const DynamicEqSettings& newSettings)
{
    const bool moved = [&] {
        for (int b = 0; b < kDynamicBands; ++b)
        {
            const auto& a = settings.bands[static_cast<size_t>(b)];
            const auto& n = newSettings.bands[static_cast<size_t>(b)];
            if (a.hz != n.hz || a.q != n.q)
                return true;
        }
        return false;
    }();
    settings = newSettings;
    const double nyquistSafe = 0.45 * sampleRate;
    for (int b = 0; b < kDynamicBands; ++b)
    {
        const auto& band = settings.bands[static_cast<size_t>(b)];
        const double hz = std::clamp(static_cast<double>(band.hz), 20.0, nyquistSafe);
        const double q = std::clamp(static_cast<double>(band.q), 0.3, 12.0);
        if (moved || detectorsStale)
            detector[static_cast<size_t>(b)].copyCoefficients(Biquad::bandPass(sampleRate, hz, q));
        attack[static_cast<size_t>(b)] = coefficient(band.attackMs, 1.0, sampleRate);
        release[static_cast<size_t>(b)] = coefficient(band.releaseMs, 1.0, sampleRate);
        if (!dynamicBandOn(band))
            cutDb[static_cast<size_t>(b)] = 0.0;
    }
    detectorsStale = false;
    riderCoeff = coefficient(settings.rider.speedMs, 1.0, sampleRate);
    if (!riderOn(settings.rider))
        riderGain = 0.0;
    updateFilters();
}

void DynamicEq::updateFilters()
{
    const double nyquistSafe = 0.45 * sampleRate;
    for (int b = 0; b < kDynamicBands; ++b)
    {
        const auto& band = settings.bands[static_cast<size_t>(b)];
        const double hz = std::clamp(static_cast<double>(band.hz), 20.0, nyquistSafe);
        const double q = std::clamp(static_cast<double>(band.q), 0.3, 12.0);
        const auto shape = Biquad::peaking(sampleRate, hz, -cutDb[static_cast<size_t>(b)], q);
        for (auto& channel : cut)
            channel[static_cast<size_t>(b)].copyCoefficients(shape);
    }
}

void DynamicEq::process(float* const* channels, int numChannels, int numSamples)
{
    peakCut.fill(0.0);
    if (!dynamicEqOn(settings) || numChannels <= 0)
        return;
    numChannels = std::min(numChannels, kMaxChannels);
    const double scale = 1.0 / numChannels;
    const bool riding = riderOn(settings.rider);
    const double target = settings.rider.targetDb, range = settings.rider.rangeDb;
    double gainNow = std::pow(10.0, riderGain / 20.0);

    for (int i = 0; i < numSamples; ++i)
    {
        double x = 0.0;
        for (int c = 0; c < numChannels; ++c)
            x += channels[c][i];
        x *= scale;

        if (riding)
        {
            riderPower = riderPowerCoeff * riderPower + (1.0 - riderPowerCoeff) * x * x;
            const double level = toDb(riderPower);
            if (level > target - kRiderGateDb) // a pause holds the gain, it does not pull the noise up
            {
                const double wanted = std::clamp(target - level, -range, range);
                riderGain = riderCoeff * riderGain + (1.0 - riderCoeff) * wanted;
            }
            x *= gainNow;
            for (int c = 0; c < numChannels; ++c)
                channels[c][i] = static_cast<float>(channels[c][i] * gainNow);
        }

        wholePower = powerCoeff * wholePower + (1.0 - powerCoeff) * x * x;
        for (int b = 0; b < kDynamicBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const auto& band = settings.bands[at];
            if (!dynamicBandOn(band))
                continue;
            const double y = detector[at].process(x);
            bandPower[at] = powerCoeff * bandPower[at] + (1.0 - powerCoeff) * y * y;
            double wanted = 0.0;
            if (wholePower > kSilentPower)
                wanted = dynamicBandCutDb(band, toDb(bandPower[at]) - toDb(wholePower) - band.thresholdDb);
            const double k = wanted > cutDb[at] ? attack[at] : release[at];
            cutDb[at] = k * cutDb[at] + (1.0 - k) * wanted;
            peakCut[at] = std::max(peakCut[at], cutDb[at]);
        }

        if (++sinceUpdate >= kControlBlock)
        {
            sinceUpdate = 0;
            updateFilters();
            gainNow = std::pow(10.0, riderGain / 20.0);
        }

        for (int b = 0; b < kDynamicBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            if (!dynamicBandOn(settings.bands[at]))
                continue;
            for (int c = 0; c < numChannels; ++c)
                channels[c][i] = static_cast<float>(cut[static_cast<size_t>(c)][at].process(channels[c][i]));
        }
    }
    for (auto& p : bandPower)
        if (p < 1e-30)
            p = 0.0;
    if (wholePower < 1e-30)
        wholePower = 0.0;
    if (riderPower < 1e-30)
        riderPower = 0.0;
}

} // namespace ma

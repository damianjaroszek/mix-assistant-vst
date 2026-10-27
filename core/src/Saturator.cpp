#include "ma/Saturator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kKnee = 0.5;      // -6 dBFS reaches the shaper's knee at 0 dB drive
constexpr double kReference = 0.25; // sine peak (-12 dBFS) the wet level is matched at

} // namespace

double Saturator::shape(SaturationMode mode, double x)
{
    switch (mode)
    {
    case SaturationMode::tape:
        return std::tanh(x);
    case SaturationMode::tube:
        // Same slope on both sides at zero, but the negative half gives in sooner: even harmonics.
        return x >= 0.0 ? 1.0 - std::exp(-x) : -(1.0 - std::exp(1.6 * x)) / 1.6;
    case SaturationMode::softClip:
        return std::abs(x) >= 1.5 ? std::copysign(1.0, x) : x - 4.0 * x * x * x / 27.0;
    case SaturationMode::hardClip:
        return std::clamp(x, -1.0, 1.0);
    case SaturationMode::fold:
        return std::sin(0.5 * kPi * x);
    }
    return x;
}

void Saturator::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    dcCoeff = std::exp(-2.0 * kPi * 10.0 / sampleRate);
    prepared = true;
    updateGains();
    reset();
}

void Saturator::reset()
{
    for (auto& s : state)
    {
        s.oversampler.reset();
        s.dcIn = s.dcOut = 0.0;
    }
}

void Saturator::setSettings(const SaturationSettings& newSettings)
{
    const bool changed = newSettings.mode != settings.mode || newSettings.driveDb != settings.driveDb;
    const bool switchedOn = newSettings.on >= 0.5f && settings.on < 0.5f;
    settings = newSettings;
    if (changed)
        updateGains();
    if (switchedOn)
        reset(); // no stale filter state from the last time it ran
}

void Saturator::updateGains()
{
    driveGain = std::pow(10.0, std::clamp(static_cast<double>(settings.driveDb), -12.0, 36.0) / 20.0) / kKnee;

    // Match the wet level to the dry one on a sine at the reference level: one period is enough.
    const auto mode = static_cast<SaturationMode>(std::clamp(static_cast<int>(settings.mode + 0.5f), 0, kSaturationModes - 1));
    constexpr int points = 64;
    double dry = 0.0, wet = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double x = kReference * std::sin(2.0 * kPi * (i + 0.5) / points);
        const double y = kKnee * shape(mode, x * driveGain);
        dry += x * x;
        wet += y * y;
    }
    makeup = wet > 1e-12 ? std::clamp(std::sqrt(dry / wet), 0.0625, 16.0) : 1.0;
}

double Saturator::compensationDb() const
{
    return 20.0 * std::log10(makeup);
}

double Saturator::oversampledSample(ChannelState& s, double x) const
{
    const auto mode = static_cast<SaturationMode>(std::clamp(static_cast<int>(settings.mode + 0.5f), 0, kSaturationModes - 1));
    const double mix = std::clamp(static_cast<double>(settings.mix), 0.0, 1.0);
    const auto blend = [&](double v) { return (1.0 - mix) * v + mix * makeup * kKnee * shape(mode, v * driveGain); };

    double o[Oversampler4x::kFactor];
    s.oversampler.up(x, o);
    for (auto& v : o)
        v = blend(v);
    return s.oversampler.down(o);
}

void Saturator::process(float* const* channels, int numChannels, int numSamples)
{
    if (settings.on < 0.5f || !prepared)
        return;
    for (int c = 0; c < std::min(numChannels, kMaxChannels); ++c)
    {
        auto& s = state[static_cast<size_t>(c)];
        float* data = channels[c];
        for (int i = 0; i < numSamples; ++i)
        {
            const double y = oversampledSample(s, data[i]);
            // Asymmetric shapes leave DC; a 10 Hz high-pass takes it out.
            const double out = y - s.dcIn + dcCoeff * s.dcOut;
            s.dcIn = y;
            s.dcOut = out;
            data[i] = static_cast<float>(out);
        }
    }
}

SaturationPrediction predictSaturation(const SaturationSettings& settings, double hz, double levelDb, double sampleRate)
{
    SaturationPrediction out;
    out.harmonicDb.fill(-200.0);
    if (settings.on < 0.5f || hz <= 0.0 || settings.mix <= 0.0f)
        return out;

    Saturator saturator;
    saturator.prepare(sampleRate);
    saturator.setSettings(settings);
    const int total = static_cast<int>(0.25 * sampleRate);
    const int settle = total / 2; // the DC blocker and the oversampling filters settle first
    std::vector<float> signal(static_cast<size_t>(total));
    const double amplitude = std::sqrt(2.0) * std::pow(10.0, levelDb / 20.0);
    for (int i = 0; i < total; ++i)
        signal[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / sampleRate));
    constexpr int kBlock = 512;
    for (int start = 0; start < total; start += kBlock)
    {
        float* channels[] = { signal.data() + start };
        saturator.process(channels, 1, std::min(kBlock, total - start));
    }

    // Goertzel on the settled half, Hann windowed.
    const int length = total - settle;
    auto powerAt = [&](double f) {
        const double w = 2.0 * kPi * f / sampleRate;
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < length; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (length - 1));
            const double s0 = window * signal[static_cast<size_t>(settle + i)] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - coeff * s1 * s2;
    };
    const double fundamental = std::max(powerAt(hz), 1e-30);
    double overtones = 0.0;
    for (int k = 2; k <= 8; ++k)
    {
        if (k * hz >= 0.45 * sampleRate)
            break;
        const double p = powerAt(k * hz);
        out.harmonicDb[static_cast<size_t>(k - 2)] = 10.0 * std::log10(std::max(p / fundamental, 1e-20));
        overtones += p;
    }
    out.overtonesDb = 10.0 * std::log10(std::max(overtones / fundamental, 1e-20));
    return out;
}

} // namespace ma

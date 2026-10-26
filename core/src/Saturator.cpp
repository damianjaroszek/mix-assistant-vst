#include "ma/Saturator.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kKnee = 0.5;      // -6 dBFS reaches the shaper's knee at 0 dB drive
constexpr double kReference = 0.25; // sine peak (-12 dBFS) the wet level is matched at

// Half-band designs (HIIR's designer): base <-> 2x, transition 0.04 of the 2x rate,
// about 100 dB of rejection; 2x <-> 4x only has to clear the gap above the audio band.
constexpr std::array<double, 8> kStage1 = { 0.04063346092419326, 0.1505051290226746, 0.30075705599187408, 0.46077450496145061,
                                            0.6095243148961883,  0.73850384111885725, 0.84922381039206607, 0.9497427837050002 };
constexpr std::array<double, 4> kStage2 = { 0.079866426236357507, 0.28382934487410993, 0.54532365107113223, 0.83441189148073791 };
} // namespace

template <size_t Coefs>
double Saturator::HalfBand<Coefs>::allpasses(double in, int first)
{
    for (auto k = static_cast<size_t>(first); k < Coefs; k += 2)
    {
        const double out = c[k] * (in - y1[k]) + x1[k];
        x1[k] = in;
        y1[k] = out;
        in = out;
    }
    return in;
}

template <size_t Coefs>
void Saturator::HalfBand<Coefs>::up(double in, double& even, double& odd)
{
    even = allpasses(in, 0);
    odd = allpasses(in, 1);
}

template <size_t Coefs>
double Saturator::HalfBand<Coefs>::down(double even, double odd)
{
    const double out = 0.5 * (allpasses(even, 0) + allpasses(pendingOdd, 1));
    pendingOdd = odd;
    return out;
}

template <size_t Coefs>
void Saturator::HalfBand<Coefs>::reset()
{
    x1.fill(0.0);
    y1.fill(0.0);
    pendingOdd = 0.0;
}

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
    for (auto& s : state)
    {
        s.up1.c = kStage1;
        s.down1.c = kStage1;
        s.up2.c = kStage2;
        s.down2.c = kStage2;
    }
    prepared = true;
    updateGains();
    reset();
}

void Saturator::reset()
{
    for (auto& s : state)
    {
        s.up1.reset();
        s.down1.reset();
        s.up2.reset();
        s.down2.reset();
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

    double a = 0.0, b = 0.0;
    s.up1.up(x, a, b);
    double o[4];
    s.up2.up(a, o[0], o[1]);
    s.up2.up(b, o[2], o[3]);
    for (auto& v : o)
        v = blend(v);
    const double t0 = s.down2.down(o[0], o[1]);
    const double t1 = s.down2.down(o[2], o[3]);
    return s.down1.down(t0, t1);
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

} // namespace ma

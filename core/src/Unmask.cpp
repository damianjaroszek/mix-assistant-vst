#include "ma/Unmask.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kControlBlock = 16;
constexpr double kPi = 3.14159265358979323846;
constexpr double kKeyMeanMs = 3.0; // mean square first, so attack and release do not ride the waveform
constexpr double kKeyAttackMs = 5.0;
constexpr double kKeyReleaseMs = 25.0; // fast enough for a kick: 18 dB down in about 100 ms
constexpr double kOctaveQ = 1.4142135623730951;

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

double coefficient(double ms, double samples, double sampleRate)
{
    return std::exp(-samples / (std::max(ms, 0.05) * 0.001 * sampleRate));
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

std::int32_t keyBandsFor(double lowHz, double highHz)
{
    std::int32_t mask = 0;
    for (int b = 0; b < kKeyBands; ++b)
    {
        const double hz = keyBandCentreHz(b);
        if (hz >= lowHz && hz <= highHz)
            mask |= 1 << b;
    }
    if (mask != 0 || lowHz <= 0.0 || highHz <= 0.0)
        return mask;
    const double middle = std::sqrt(lowHz * highHz);
    int nearest = 0;
    for (int b = 1; b < kKeyBands; ++b)
        if (std::abs(std::log2(keyBandCentreHz(b) / middle)) < std::abs(std::log2(keyBandCentreHz(nearest) / middle)))
            nearest = b;
    return 1 << nearest;
}

double keyLevelDb(const float* bandDb, std::int32_t bands)
{
    double sum = 0.0;
    for (int b = 0; b < kKeyBands; ++b)
        if ((bands & (1 << b)) != 0 && bandDb[b] > -150.0f)
            sum += std::pow(10.0, bandDb[b] / 10.0);
    return sum > 1e-20 ? 10.0 * std::log10(sum) : static_cast<double>(kKeySilentDb);
}

void KeyFollower::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    for (int b = 0; b < kKeyBands; ++b)
    {
        const double hz = std::min(keyBandCentreHz(b), 0.45 * sampleRate);
        filters[static_cast<size_t>(b)] = bandPass(sampleRate, hz, kOctaveQ);
    }
    mean = coefficient(kKeyMeanMs, 1.0, sampleRate);
    attack = coefficient(kKeyAttackMs, 1.0, sampleRate);
    release = coefficient(kKeyReleaseMs, 1.0, sampleRate);
    reset();
}

void KeyFollower::reset()
{
    for (auto& f : filters)
        f.reset();
    power.fill(0.0);
    meanSquare.fill(0.0);
    levelsDb.fill(kKeySilentDb);
}

void KeyFollower::process(const float* const* channels, int numChannels, int numSamples)
{
    if (numChannels <= 0)
        return;
    const double scale = 1.0 / numChannels;
    for (int i = 0; i < numSamples; ++i)
    {
        double x = 0.0;
        for (int c = 0; c < numChannels; ++c)
            x += channels[c][i];
        x *= scale;
        for (int b = 0; b < kKeyBands; ++b)
        {
            const double y = filters[static_cast<size_t>(b)].process(x);
            auto& m = meanSquare[static_cast<size_t>(b)];
            m = mean * m + (1.0 - mean) * y * y;
            auto& p = power[static_cast<size_t>(b)];
            const double k = m > p ? attack : release;
            p = k * p + (1.0 - k) * m;
        }
    }
    for (int b = 0; b < kKeyBands; ++b)
    {
        auto& p = power[static_cast<size_t>(b)];
        if (p < 1e-30)
            p = 0.0; // no denormals while the channel is silent
        if (meanSquare[static_cast<size_t>(b)] < 1e-30)
            meanSquare[static_cast<size_t>(b)] = 0.0;
        levelsDb[static_cast<size_t>(b)] = p > 1e-20 ? static_cast<float>(10.0 * std::log10(p)) : kKeySilentDb;
    }
}

void Unmasker::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    reset();
}

void Unmasker::reset()
{
    for (auto& channels : dip)
        for (auto& f : channels)
            f = Biquad {};
    referenceDb.fill(kKeySilentDb);
    currentDb.fill(0.0);
    peakReductionDb.fill(0.0);
}

void Unmasker::setSettings(int index, const UnmaskSettings& newSettings)
{
    if (index < 0 || index >= kMaxUnmask)
        return;
    auto& s = settings[static_cast<size_t>(index)];
    if (static_cast<std::int32_t>(newSettings.keyBands) != static_cast<std::int32_t>(s.keyBands))
        referenceDb[static_cast<size_t>(index)] = kKeySilentDb; // listens to other bands: learn their level again
    s = newSettings;
}

double Unmasker::amountFor(double keyDb, double referenceDb)
{
    if (keyDb <= -150.0 || referenceDb <= -150.0)
        return 0.0;
    return std::clamp((keyDb - (referenceDb - kStartUnderDb)) / (kStartUnderDb - kFullUnderDb), 0.0, 1.0);
}

void Unmasker::process(float* const* main, int numMain, int numSamples, const double* keyDb)
{
    const int channels = std::min(numMain, kMaxChannels);
    const double blockSeconds = numSamples / sampleRate;
    for (int d = 0; d < kMaxUnmask; ++d)
    {
        const auto& s = settings[static_cast<size_t>(d)];
        auto& reference = referenceDb[static_cast<size_t>(d)];
        auto& current = currentDb[static_cast<size_t>(d)];
        auto& filters = dip[static_cast<size_t>(d)];
        peakReductionDb[static_cast<size_t>(d)] = 0.0;
        const double key = keyDb != nullptr ? keyDb[d] : kKeySilentDb;

        // The key's recent loud level: jumps up with it, falls slowly.
        if (key > reference)
            reference = key;
        else if (reference > -150.0)
            reference -= kReferenceFallDbPerSecond * blockSeconds;

        const double target = unmaskOn(s) ? amountFor(key, reference) * s.depthDb : 0.0;
        if (target <= 0.01 && current <= 0.01)
        {
            current = 0.0;
            for (auto& f : filters)
                f.reset(); // a 0 dB bell with an empty state is the identity, so it can start again cleanly
            continue;
        }

        const double hz = std::clamp(static_cast<double>(s.hz), 20.0, 0.45 * sampleRate);
        const double q = std::max(0.3, static_cast<double>(s.q));
        for (int start = 0; start < numSamples; start += kControlBlock)
        {
            const int n = std::min(kControlBlock, numSamples - start);
            const double k = coefficient(target > current ? kAttackMs : kReleaseMs, n, sampleRate);
            current = k * current + (1.0 - k) * target;
            peakReductionDb[static_cast<size_t>(d)] = std::max(peakReductionDb[static_cast<size_t>(d)], current);
            const auto bell = Biquad::peaking(sampleRate, hz, -current, q);
            for (int c = 0; c < channels; ++c)
            {
                auto& f = filters[static_cast<size_t>(c)];
                copyCoefficients(f, bell);
                for (int i = start; i < start + n; ++i)
                    main[c][i] = static_cast<float>(f.process(main[c][i]));
            }
        }
    }
}

} // namespace ma

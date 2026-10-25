#include "ma/Translation.h"

#include "ma/ChannelAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kErbBands = 40;
constexpr double kDesignRate = 48000.0;

Biquad butterworth(double sampleRate, double hz, bool highPass, double q = 1.0 / std::sqrt(2.0))
{
    const double k = std::tan(kPi * std::min(hz, 0.45 * sampleRate) / sampleRate);
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

// RBJ low shelf, slope 1.
Biquad lowShelf(double sampleRate, double hz, double gainDb)
{
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * hz / sampleRate;
    const double cosw = std::cos(w);
    const double alpha = std::sin(w) / 2.0 * std::sqrt(2.0);
    const double sqrtA = 2.0 * std::sqrt(a) * alpha;
    const double a0 = (a + 1.0) + (a - 1.0) * cosw + sqrtA;
    Biquad f;
    f.b0 = a * ((a + 1.0) - (a - 1.0) * cosw + sqrtA) / a0;
    f.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw) / a0;
    f.b2 = a * ((a + 1.0) - (a - 1.0) * cosw - sqrtA) / a0;
    f.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosw) / a0;
    f.a2 = ((a + 1.0) + (a - 1.0) * cosw - sqrtA) / a0;
    return f;
}

// Up to four sections per system; unused ones pass through.
std::array<Biquad, SystemSimulator::kSections> design(PlaybackSystem system, double sr)
{
    std::array<Biquad, SystemSimulator::kSections> f {};
    switch (system)
    {
    case PlaybackSystem::fullRange:
        break;
    case PlaybackSystem::phone: // tiny speaker: steep roll-off under ~350 Hz, a little honk at 2.5 kHz
        f[0] = butterworth(sr, 350.0, true);
        f[1] = butterworth(sr, 350.0, true);
        f[2] = Biquad::peaking(sr, 2500.0, 3.0, 1.0);
        f[3] = butterworth(sr, 10000.0, false);
        break;
    case PlaybackSystem::laptop:
        f[0] = butterworth(sr, 180.0, true);
        f[1] = butterworth(sr, 180.0, true);
        f[2] = Biquad::peaking(sr, 1500.0, 2.0, 0.8);
        f[3] = butterworth(sr, 15000.0, false);
        break;
    case PlaybackSystem::earbuds:
        f[0] = butterworth(sr, 70.0, true);
        f[1] = Biquad::peaking(sr, 3000.0, 3.0, 1.2);
        break;
    case PlaybackSystem::car: // cabin gain in the lows, road noise masks the very top
        f[0] = lowShelf(sr, 90.0, 6.0);
        f[1] = butterworth(sr, 25.0, true);
        f[2] = butterworth(sr, 13000.0, false);
        break;
    case PlaybackSystem::club: // subs push under 60 Hz, roll off under 30 Hz
        f[0] = lowShelf(sr, 60.0, 5.0);
        f[1] = butterworth(sr, 30.0, true);
        f[2] = butterworth(sr, 30.0, true);
        f[3] = butterworth(sr, 14000.0, false);
        break;
    }
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

struct ResponseTable
{
    std::array<std::array<double, SpectrumAnalyzer::kMaxBands>, kPlaybackSystems> db {};
    ResponseTable()
    {
        for (int s = 0; s < kPlaybackSystems; ++s)
            for (int b = 0; b < kErbBands; ++b)
                db[static_cast<size_t>(s)][static_cast<size_t>(b)]
                    = systemResponseDb(static_cast<PlaybackSystem>(s), SpectrumAnalyzer::bandCentreHz(BandScale::erb, b));
    }
};

const ResponseTable& responses()
{
    static const ResponseTable table;
    return table;
}
} // namespace

double systemResponseDb(PlaybackSystem system, double hz)
{
    double db = 0.0;
    for (const auto& f : design(system, kDesignRate))
        db += f.magnitudeDb(hz, kDesignRate);
    return db;
}

void SystemSimulator::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    const auto system = current;
    current = PlaybackSystem::fullRange;
    for (auto& channel : filters)
        for (auto& f : channel)
            f = Biquad {};
    setSystem(system);
}

void SystemSimulator::setSystem(PlaybackSystem system)
{
    if (system == current)
        return;
    current = system;
    const auto sections = design(system, sampleRate);
    for (auto& channel : filters)
        for (int i = 0; i < kSections; ++i)
        {
            setCoefficients(channel[static_cast<size_t>(i)], sections[static_cast<size_t>(i)]);
            channel[static_cast<size_t>(i)].reset();
        }
}

void SystemSimulator::process(float* const* channels, int numChannels, int numSamples)
{
    if (current == PlaybackSystem::fullRange)
        return;
    // A club plays mono.
    if (current == PlaybackSystem::club && numChannels >= 2)
        for (int i = 0; i < numSamples; ++i)
        {
            const float mono = 0.5f * (channels[0][i] + channels[1][i]);
            channels[0][i] = mono;
            channels[1][i] = mono;
        }
    for (int c = 0; c < std::min(numChannels, kMaxChannels); ++c)
    {
        auto& chain = filters[static_cast<size_t>(c)];
        float* data = channels[c];
        for (int i = 0; i < numSamples; ++i)
        {
            double x = data[i];
            for (auto& f : chain)
                x = f.process(x);
            data[i] = static_cast<float>(x);
        }
    }
}

double perceivedLevelDb(const SpectrumAnalyzer::Bands& rawDb, PlaybackSystem system)
{
    const auto& weights = erbEqualLoudnessWeights();
    const auto& response = responses().db[static_cast<size_t>(system)];
    double power = 0.0;
    for (int b = 0; b < kErbBands; ++b)
    {
        const double db = rawDb[static_cast<size_t>(b)];
        if (db > -150.0)
            power += std::pow(10.0, (db - weights[static_cast<size_t>(b)] + response[static_cast<size_t>(b)]) / 10.0);
    }
    return power > 0.0 ? 10.0 * std::log10(power) : -200.0;
}

TranslationResult analyseTranslation(const std::vector<TranslationChannel>& channels)
{
    TranslationResult result;
    result.shareChangeDb.assign(channels.size(), {});

    SpectrumAnalyzer::Bands mix {};
    mix.fill(-200.0);
    int valid = 0;
    for (const auto& channel : channels)
    {
        if (!channel.valid)
            continue;
        ++valid;
        for (int b = 0; b < kErbBands; ++b)
        {
            const double sum = std::pow(10.0, mix[static_cast<size_t>(b)] / 10.0)
                               + std::pow(10.0, channel.rawDb[static_cast<size_t>(b)] / 10.0);
            mix[static_cast<size_t>(b)] = 10.0 * std::log10(std::max(sum, 1e-20));
        }
    }
    if (valid == 0)
        return result;

    for (int s = 0; s < kPlaybackSystems; ++s)
        result.mixLevelDb[static_cast<size_t>(s)] = perceivedLevelDb(mix, static_cast<PlaybackSystem>(s));

    for (size_t c = 0; c < channels.size(); ++c)
    {
        if (!channels[c].valid)
            continue;
        const double fullShare = perceivedLevelDb(channels[c].rawDb, PlaybackSystem::fullRange) - result.mixLevelDb[0];
        for (int s = 1; s < kPlaybackSystems; ++s)
        {
            const double share = perceivedLevelDb(channels[c].rawDb, static_cast<PlaybackSystem>(s)) - result.mixLevelDb[static_cast<size_t>(s)];
            result.shareChangeDb[c][static_cast<size_t>(s)] = std::max(-60.0, share - fullShare);
        }
    }
    result.valid = valid >= 2;
    return result;
}

} // namespace ma

#include "ma/Stereo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ma
{

double coherence(double midDb, double sideDb)
{
    const double m = std::pow(10.0, midDb / 10.0), s = std::pow(10.0, sideDb / 10.0);
    return m + s > 1e-20 ? (m - s) / (m + s) : 1.0; // silence counts as mono
}

void StereoAnalyzer::prepare(double sampleRate)
{
    mid.prepare(sampleRate, BandScale::erb);
    side.prepare(sampleRate, BandScale::erb);
}

void StereoAnalyzer::reset()
{
    mid.reset();
    side.reset();
}

void StereoAnalyzer::process(const float* left, const float* right, int numSamples)
{
    for (int start = 0; start < numSamples; start += kChunk)
    {
        const int count = std::min(kChunk, numSamples - start);
        for (int i = 0; i < count; ++i)
        {
            const float l = left[start + i], r = right[start + i];
            midBuffer[static_cast<size_t>(i)] = 0.5f * (l + r);
            sideBuffer[static_cast<size_t>(i)] = 0.5f * (l - r);
        }
        const float* m[] { midBuffer.data() };
        const float* s[] { sideBuffer.data() };
        mid.process(m, 1, count);
        side.process(s, 1, count);
    }
}

double sideResponseDb(const StereoSettings& settings, double hz)
{
    double db = 0.0;
    for (const auto& bell : settings.side)
        if (std::abs(bell.db) > 0.01f)
            db += Biquad::peaking(48000.0, bell.hz, bell.db, bell.q).magnitudeDb(hz, 48000.0);
    return db;
}

bool isOn(const StereoSettings& settings)
{
    return std::any_of(settings.side.begin(), settings.side.end(), [](const auto& b) { return std::abs(b.db) > 0.01f; });
}

void StereoShaper::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    initialised = false;
    setSettings(settings);
    reset();
}

void StereoShaper::reset()
{
    for (auto& f : bells)
        f.reset();
}

void StereoShaper::setSettings(const StereoSettings& next)
{
    if (initialised && std::memcmp(&next, &settings, sizeof(settings)) == 0)
        return;
    settings = next;
    initialised = true;
    for (size_t b = 0; b < bells.size(); ++b)
    {
        const auto& bell = settings.side[b];
        if (std::abs(bell.db) <= 0.01f)
            continue;
        const auto fresh = Biquad::peaking(sampleRate, std::min<double>(bell.hz, 0.45 * sampleRate), bell.db, bell.q);
        bells[b].b0 = fresh.b0;
        bells[b].b1 = fresh.b1;
        bells[b].b2 = fresh.b2;
        bells[b].a1 = fresh.a1;
        bells[b].a2 = fresh.a2;
    }
}

void StereoShaper::process(float* const* channels, int numChannels, int numSamples)
{
    if (numChannels < 2 || !isOn(settings))
        return;
    std::array<bool, StereoSettings::kBells> active {};
    for (size_t b = 0; b < active.size(); ++b)
        active[b] = std::abs(settings.side[b].db) > 0.01f;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = channels[0][i], r = channels[1][i];
        const double m = 0.5 * (l + r);
        double s = 0.5 * (l - r);
        for (size_t b = 0; b < bells.size(); ++b)
            if (active[b])
                s = bells[b].process(s);
        channels[0][i] = static_cast<float>(m + s);
        channels[1][i] = static_cast<float>(m - s);
    }
}

} // namespace ma

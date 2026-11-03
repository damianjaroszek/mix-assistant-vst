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

bool widenOn(const StereoSettings& settings)
{
    return settings.widen.amountDb > kWidenOffDb;
}

double widenPowerRatio(const StereoSettings& settings, double hz)
{
    if (!widenOn(settings))
        return 0.0;
    const double highPass = 2.0 * Biquad::highPass(48000.0, settings.widen.lowHz).magnitudeDb(hz, 48000.0);
    return std::pow(10.0, (2.0 * settings.widen.amountDb + 2.0 * highPass) / 20.0);
}

bool isOn(const StereoSettings& settings)
{
    return widenOn(settings)
           || std::any_of(settings.side.begin(), settings.side.end(), [](const auto& b) { return std::abs(b.db) > 0.01f; });
}

bool widthOn(const WidthSettings& width)
{
    return width.active > 0.5f;
}

namespace
{
double widthWidenFromHz(const WidthSettings& width)
{
    return std::max(300.0, static_cast<double>(width.fromHz));
}
} // namespace

double processedSidePower(const StereoSettings& settings, const WidthSettings& width, double hz, double midPower, double sidePower)
{
    const bool own = widthOn(width) && width.widenDb > kWidenOffDb;
    double side = sidePower * std::pow(10.0, sideResponseDb(settings, hz) / 10.0) + (own ? 0.0 : widenPowerRatio(settings, hz) * midPower);
    if (!widthOn(width))
        return side;
    // s' = scale * LR4highpass(s): the low side out (24 dB/oct), the rest scaled.
    const double sideHigh = std::norm(Biquad::highPass(48000.0, width.fromHz).response(hz, 48000.0));
    side *= static_cast<double>(width.scale) * width.scale * sideHigh * sideHigh;
    if (own)
    {
        const double highPass = std::norm(Biquad::highPass(48000.0, widthWidenFromHz(width)).response(hz, 48000.0));
        side += std::pow(10.0, width.widenDb / 10.0) * highPass * highPass * midPower;
    }
    for (const auto& bell : width.guard)
        if (std::abs(bell.db) > 0.01f)
            side *= std::pow(10.0, Biquad::peaking(48000.0, bell.hz, bell.db, bell.q).magnitudeDb(hz, 48000.0) / 10.0);
    return side;
}

void StereoShaper::prepare(double newSampleRate)
{
    sampleRate = newSampleRate;
    delay.assign(static_cast<size_t>(std::ceil(kMaxWidenDelayMs * 0.001 * newSampleRate)) + 2, 0.0);
    delayPos = 0;
    initialised = false;
    widthInitialised = false;
    setSettings(settings);
    setWidth(width);
    reset();
}

void StereoShaper::reset()
{
    for (auto& f : bells)
        f.reset();
    for (auto& f : widenHighPass)
        f.reset();
    for (auto& f : widthSideHighPass)
        f.reset();
    for (auto& f : widthHighPass)
        f.reset();
    for (auto& f : guards)
        f.reset();
    std::fill(delay.begin(), delay.end(), 0.0);
    delayPos = 0;
    scaleNow = widthScale;
    widthWidenNow = widthWidenGain;
}

void StereoShaper::setWidth(const WidthSettings& next)
{
    if (widthInitialised && std::memcmp(&next, &width, sizeof(width)) == 0)
        return;
    const bool wasOn = widthInitialised && widthOn(width);
    width = next;
    widthInitialised = true;
    const double nyquistSafe = 0.45 * sampleRate;
    const auto sideHigh = Biquad::highPass(sampleRate, std::clamp<double>(width.fromHz, 20.0, nyquistSafe));
    for (auto& f : widthSideHighPass)
        f.copyCoefficients(sideHigh);
    const auto high = Biquad::highPass(sampleRate, std::clamp<double>(widthWidenFromHz(width), 20.0, nyquistSafe));
    for (auto& f : widthHighPass)
        f.copyCoefficients(high);
    for (size_t g = 0; g < guards.size(); ++g)
        if (std::abs(width.guard[g].db) > 0.01f)
            guards[g].copyCoefficients(Biquad::peaking(sampleRate, std::min<double>(width.guard[g].hz, nyquistSafe), width.guard[g].db,
                                                       width.guard[g].q));
    const auto size = static_cast<int>(delay.size());
    widthDelaySamples = std::clamp(static_cast<int>(std::lround(std::clamp(width.delayMs, 0.5f, kMaxWidenDelayMs) * 0.001 * sampleRate)), 1,
                                   std::max(1, size - 1));
    widthScale = widthOn(width) ? std::max(0.0f, width.scale) : 1.0;
    widthWidenGain = widthOn(width) && width.widenDb > kWidenOffDb ? std::pow(10.0, width.widenDb / 20.0) : 0.0;
    if (!wasOn && widthOn(width))
    {
        // Switching on: start from no change and ramp to the new width.
        scaleNow = 1.0;
        widthWidenNow = 0.0;
        for (auto& f : widthSideHighPass)
            f.reset();
    }
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
    widenGain = widenOn(settings) ? std::pow(10.0, settings.widen.amountDb / 20.0) : 0.0;
    const auto size = static_cast<int>(delay.size());
    delaySamples = std::clamp(static_cast<int>(std::lround(std::clamp(settings.widen.delayMs, 0.5f, kMaxWidenDelayMs) * 0.001 * sampleRate)), 1,
                              std::max(1, size - 1));
    const auto highPass = Biquad::highPass(sampleRate, std::clamp<double>(settings.widen.lowHz, 20.0, 0.45 * sampleRate));
    for (auto& f : widenHighPass)
    {
        f.b0 = highPass.b0;
        f.b1 = highPass.b1;
        f.b2 = highPass.b2;
        f.a1 = highPass.a1;
        f.a2 = highPass.a2;
    }
}

void StereoShaper::process(float* const* channels, int numChannels, int numSamples)
{
    const bool widthActive = widthOn(width);
    if (numChannels < 2 || (!isOn(settings) && !widthActive))
        return;
    std::array<bool, StereoSettings::kBells> active {};
    for (size_t b = 0; b < active.size(); ++b)
        active[b] = std::abs(settings.side[b].db) > 0.01f;
    std::array<bool, WidthSettings::kGuards> guarding {};
    for (size_t g = 0; g < guarding.size(); ++g)
        guarding[g] = widthActive && std::abs(width.guard[g].db) > 0.01f;
    // The slider's widener replaces the Brain's while it runs.
    const double brainWiden = widthActive && widthWidenGain > 0.0 ? 0.0 : widenGain;
    const bool useDelay = !delay.empty() && (brainWiden > 0.0 || (widthActive && (widthWidenGain > 0.0 || widthWidenNow > 0.0)));
    const auto size = static_cast<int>(delay.size());
    // About 50 ms from one width to the next, so moving the slider does not click.
    const double step = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = channels[0][i], r = channels[1][i];
        const double m = 0.5 * (l + r);
        double s = 0.5 * (l - r);
        for (size_t b = 0; b < bells.size(); ++b)
            if (active[b])
                s = bells[b].process(s);
        double delayed = 0.0, delayedWidth = 0.0;
        if (useDelay)
        {
            delay[static_cast<size_t>(delayPos)] = m;
            delayed = delay[static_cast<size_t>((delayPos - delaySamples + size) % size)];
            delayedWidth = delay[static_cast<size_t>((delayPos - widthDelaySamples + size) % size)];
            delayPos = (delayPos + 1) % size;
        }
        if (brainWiden > 0.0)
            s += brainWiden * widenHighPass[1].process(widenHighPass[0].process(delayed));
        if (widthActive)
        {
            scaleNow += step * (widthScale - scaleNow);
            widthWidenNow += step * (widthWidenGain - widthWidenNow);
            // The side's low end goes (LR4 high-pass on the side only, so the mono sum is untouched).
            s = scaleNow * widthSideHighPass[1].process(widthSideHighPass[0].process(s));
            if (widthWidenNow > 1e-6)
                s += widthWidenNow * widthHighPass[1].process(widthHighPass[0].process(delayedWidth));
            for (size_t g = 0; g < guards.size(); ++g)
                if (guarding[g])
                    s = guards[g].process(s);
        }
        channels[0][i] = static_cast<float>(m + s);
        channels[1][i] = static_cast<float>(m - s);
    }
}

} // namespace ma

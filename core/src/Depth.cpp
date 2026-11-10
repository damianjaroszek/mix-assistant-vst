#include "ma/Depth.h"

#include <algorithm>
#include <cmath>

namespace ma
{

bool depthOn(const DepthSettings& depth)
{
    return depth.highCutHz > 0.0f;
}

double depthResponseDb(const DepthSettings& depth, double hz)
{
    if (!depthOn(depth))
        return 0.0;
    // The filter as the Agent runs it, at the usual 48 kHz (the top bands bend with the rate).
    constexpr double rate = 48000.0;
    return std::min(0.0, Biquad::lowPass(rate, std::clamp(static_cast<double>(depth.highCutHz), 200.0, 0.45 * rate))
                             .magnitudeDb(std::min(hz, 0.49 * rate), rate));
}

void HighCut::prepare(double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    filterHz = -1.0;
    reset();
}

void HighCut::reset()
{
    for (auto& f : lowPass)
        f.reset();
}

void HighCut::setSettings(const DepthSettings& next)
{
    settings = next;
}

void HighCut::process(float* const* channels, int numChannels, int numSamples)
{
    if (!depthOn(settings))
    {
        if (filterHz > 0.0)
        {
            reset();
            filterHz = -1.0;
        }
        return;
    }
    if (filterHz != static_cast<double>(settings.highCutHz))
    {
        filterHz = settings.highCutHz;
        const auto lp = Biquad::lowPass(sampleRate, std::clamp(filterHz, 200.0, 0.45 * sampleRate));
        for (auto& f : lowPass)
            f.copyCoefficients(lp);
    }
    for (int c = 0; c < std::min(numChannels, 2); ++c)
    {
        auto& f = lowPass[static_cast<size_t>(c)];
        float* x = channels[c];
        for (int i = 0; i < numSamples; ++i)
            x[i] = static_cast<float>(f.process(x[i]));
    }
}

DepthTimes depthTimes(double bpm)
{
    DepthTimes t;
    if (bpm <= 0.0)
        return t;
    const double whole = 240000.0 / bpm; // ms in a bar of 4/4
    t.valid = true;
    t.preDelayMs = whole / 64.0;
    t.decaySeconds = whole / 2.0 / 1000.0;
    t.eighthMs = whole / 8.0;
    t.dottedEighthMs = whole * 3.0 / 16.0;
    t.quarterMs = whole / 4.0;
    return t;
}

double topTiltDb(const SpectrumAnalyzer::Bands& erbDb)
{
    double top = 0.0, mid = 0.0;
    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        const double db = erbDb[static_cast<size_t>(b)];
        const double p = db <= -150.0 ? 0.0 : std::pow(10.0, db / 10.0);
        if (hz >= 8000.0 && hz <= 16000.0)
            top += p;
        else if (hz >= 1000.0 && hz <= 4000.0)
            mid += p;
    }
    if (mid <= 0.0)
        return 0.0;
    return 10.0 * std::log10(std::max(top, 1e-30) / mid);
}

SendDuck planSendDuck(const SpectrumAnalyzer::Bands& sendDb, const SpectrumAnalyzer::Bands& voiceDb)
{
    SendDuck d;
    double send = 0.0, voice = 0.0, loudest = -1.0;
    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        const auto power = [](double db) { return db <= -150.0 || !std::isfinite(db) ? 0.0 : std::pow(10.0, db / 10.0); };
        const double v = power(voiceDb[static_cast<size_t>(b)]);
        if (hz >= 1000.0 && hz <= 5000.0)
        {
            send += power(sendDb[static_cast<size_t>(b)]);
            voice += v;
        }
        if (hz >= 800.0 && hz <= 5000.0 && v > loudest)
        {
            loudest = v;
            d.hz = hz;
        }
    }
    if (voice <= 1e-12 || send <= 1e-15)
        return d;
    d.valid = true;
    d.tailVsVoiceDb = 10.0 * std::log10(send / voice);
    const double depth = std::clamp(d.tailVsVoiceDb + kSendUnderVoiceDb, kSendDuckMinDb, kSendDuckMaxDb);
    d.depthDb = std::round(depth * 2.0) / 2.0;
    return d;
}

} // namespace ma

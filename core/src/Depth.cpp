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
    return planSendDuck(sendDb, voiceDb, 1000.0, 5000.0, 800.0, 5000.0, kSendDuckMaxDb);
}

SendDuck planSendDuck(const SpectrumAnalyzer::Bands& sendDb, const SpectrumAnalyzer::Bands& voiceDb, double fromHz, double toHz,
                      double peakFromHz, double peakToHz, double maxDb)
{
    SendDuck d;
    double send = 0.0, voice = 0.0, loudest = -1.0;
    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        const auto power = [](double db) { return db <= -150.0 || !std::isfinite(db) ? 0.0 : std::pow(10.0, db / 10.0); };
        const double v = power(voiceDb[static_cast<size_t>(b)]);
        if (hz >= fromHz && hz <= toHz)
        {
            send += power(sendDb[static_cast<size_t>(b)]);
            voice += v;
        }
        if (hz >= peakFromHz && hz <= peakToHz && v > loudest)
        {
            loudest = v;
            d.hz = hz;
        }
    }
    if (voice <= 1e-12 || send <= 1e-15)
        return d;
    d.valid = true;
    d.tailVsVoiceDb = 10.0 * std::log10(send / voice);
    const double depth = std::clamp(d.tailVsVoiceDb + kSendUnderVoiceDb, kSendDuckMinDb, std::max(kSendDuckMinDb, maxDb));
    d.depthDb = std::round(depth * 2.0) / 2.0;
    return d;
}

SourceFrame sourceFrame(const SpectrumAnalyzer::Bands& powerLinear)
{
    SourceFrame f {};
    const double low = std::log(150.0), high = std::log(12000.0);
    for (int b = 0; b < SpectrumAnalyzer::numBandsFor(BandScale::erb); ++b)
    {
        const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
        if (hz < 150.0 || hz >= 12000.0)
            continue;
        const int r = std::clamp(static_cast<int>((std::log(hz) - low) / (high - low) * kSourceRegions), 0, kSourceRegions - 1);
        const double p = powerLinear[static_cast<size_t>(b)];
        if (std::isfinite(p) && p > 0.0)
            f[static_cast<size_t>(r)] += p;
    }
    return f;
}

SendSources findSendSources(const std::vector<SourceFrame>& send, const std::vector<std::vector<SourceFrame>>& candidates)
{
    SendSources out;
    const size_t n = candidates.size();
    out.share.assign(n, 0.0);
    if (n == 0 || send.size() < 2)
        return out;
    const size_t frames = send.size();
    // The seconds the send plays in (within 40 dB of its loudest).
    double loudest = 0.0;
    for (const auto& f : send)
    {
        double sum = 0.0;
        for (double v : f)
            sum += v;
        loudest = std::max(loudest, sum);
    }
    if (loudest <= 0.0)
        return out;
    std::vector<size_t> used;
    for (size_t t = 1; t < frames; ++t)
    {
        double sum = 0.0;
        for (double v : send[t])
            sum += v;
        if (sum >= loudest * 1e-4)
            used.push_back(t);
    }
    out.seconds = static_cast<int>(used.size());
    if (out.seconds < kMinSourceSeconds)
        return out;
    // Each region weighs the same: normalised by the send's mean there.
    SourceFrame scale {};
    for (const auto t : used)
        for (int r = 0; r < kSourceRegions; ++r)
            scale[static_cast<size_t>(r)] += send[t][static_cast<size_t>(r)];
    for (auto& v : scale)
        v = v > 0.0 ? static_cast<double>(used.size()) / v : 0.0;
    const size_t rows = used.size() * kSourceRegions;
    std::vector<double> y(rows), x(rows * n);
    for (size_t i = 0; i < used.size(); ++i)
    {
        const size_t t = used[i];
        for (int r = 0; r < kSourceRegions; ++r)
        {
            const size_t row = i * kSourceRegions + static_cast<size_t>(r);
            const double s = scale[static_cast<size_t>(r)];
            y[row] = send[t][static_cast<size_t>(r)] * s;
            for (size_t c = 0; c < n; ++c)
            {
                const auto& cand = candidates[c];
                const double now = t < cand.size() ? cand[t][static_cast<size_t>(r)] : 0.0;
                const double before = t >= 1 && t - 1 < cand.size() ? cand[t - 1][static_cast<size_t>(r)] : 0.0;
                x[row * n + c] = (0.7 * now + 0.3 * before) * s;
            }
        }
    }
    // Non-negative least squares by coordinate descent on the normal equations.
    std::vector<double> gram(n * n, 0.0), xy(n, 0.0), w(n, 0.0);
    double yy = 0.0;
    for (size_t row = 0; row < rows; ++row)
    {
        yy += y[row] * y[row];
        for (size_t a = 0; a < n; ++a)
        {
            const double xa = x[row * n + a];
            if (xa == 0.0)
                continue;
            xy[a] += xa * y[row];
            for (size_t b = a; b < n; ++b)
                gram[a * n + b] += xa * x[row * n + b];
        }
    }
    for (size_t a = 0; a < n; ++a)
        for (size_t b = 0; b < a; ++b)
            gram[a * n + b] = gram[b * n + a];
    if (yy <= 0.0)
        return out;
    for (int iteration = 0; iteration < 300; ++iteration)
    {
        double moved = 0.0;
        for (size_t a = 0; a < n; ++a)
        {
            const double g = gram[a * n + a];
            if (g <= 0.0)
                continue;
            double r = xy[a];
            for (size_t b = 0; b < n; ++b)
                if (b != a)
                    r -= gram[a * n + b] * w[b];
            const double next = std::max(0.0, r / g);
            moved = std::max(moved, std::abs(next - w[a]) * std::sqrt(g));
            w[a] = next;
        }
        if (moved < 1e-9 * std::sqrt(yy))
            break;
    }
    // Fit and shares.
    double residual = 0.0;
    std::vector<double> explained(n, 0.0);
    double ySum = 0.0;
    for (size_t row = 0; row < rows; ++row)
    {
        double fitted = 0.0;
        for (size_t c = 0; c < n; ++c)
        {
            const double part = w[c] * x[row * n + c];
            fitted += part;
            explained[c] += part;
        }
        residual += (y[row] - fitted) * (y[row] - fitted);
        ySum += y[row];
    }
    // Uncentred: the regions are normalised, so the mean is the send's own shape, which the sources
    // must explain too.
    out.fit = 1.0 - residual / yy;
    double best = 0.0;
    for (size_t c = 0; c < n; ++c)
    {
        out.share[c] = ySum > 0.0 ? std::clamp(explained[c] / ySum, 0.0, 1.0) : 0.0;
        if (out.share[c] > best)
        {
            best = out.share[c];
            out.dominant = static_cast<int>(c);
        }
    }
    out.valid = out.fit >= kMinSourceFit && out.dominant >= 0;
    return out;
}

void TailMeter::prepare(double sampleRate)
{
    hop = std::max(1, static_cast<int>(std::lround(sampleRate * 0.01)));
    coefficient = std::exp(-1.0 / (0.015 * sampleRate));
    peakFallDb = 6.0 * 0.01; // the recent peak forgets 6 dB a second
    reset();
}

void TailMeter::reset()
{
    falls.fill(0);
    total = 0;
    power = 0.0;
    recentPeakDb = topDb = minDb = lastDb = -200.0;
    since3 = -1.0;
    atMin = 0.0;
    hops = inHop = 0;
}

void TailMeter::process(const float* left, const float* right, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i], r = right != nullptr ? right[i] : l;
        power = coefficient * power + (1.0 - coefficient) * 0.5 * (l * l + r * r);
        if (++inHop >= hop)
        {
            inHop = 0;
            hopDone(10.0 * std::log10(std::max(power, 1e-20)));
        }
    }
}

void TailMeter::fallDone()
{
    // From the first 3 dB down (the hit itself is over) to the lowest point reached.
    const double fellDb = topDb - 3.0 - minDb;
    const double took = (atMin - since3) * 0.01;
    if (since3 >= 0.0 && fellDb >= 4.0 && took >= 0.1 && minDb > -90.0)
    {
        const double seconds = 60.0 * took / fellDb;
        if (seconds >= 0.2 && seconds <= 12.0)
        {
            const double position = std::log(seconds / 0.2) / std::log(12.0 / 0.2);
            ++falls[static_cast<size_t>(std::clamp(static_cast<int>(position * kBins), 0, kBins - 1))];
            if (++total > 400)
            {
                total = 0;
                for (auto& count : falls)
                {
                    count /= 2;
                    total += count;
                }
            }
        }
    }
}

void TailMeter::hopDone(double levelDb)
{
    recentPeakDb = std::max(recentPeakDb - peakFallDb, levelDb);
    const bool rising = levelDb > minDb + 3.0;
    if (rising || topDb <= -199.0)
    {
        if (topDb > -199.0)
            fallDone();
        // A new fall starts from here, if it is loud enough to be the part and not its floor.
        topDb = levelDb >= recentPeakDb - 20.0 ? levelDb : -200.0;
        minDb = levelDb;
        since3 = -1.0;
        hops = 0;
        atMin = 0.0;
    }
    else
    {
        ++hops;
        if (levelDb > topDb)
        {
            // Still climbing to its top (the smoother catching up with the hit).
            topDb = minDb = levelDb;
            hops = 0;
        }
        else
        {
            if (since3 < 0.0 && levelDb <= topDb - 3.0)
            {
                // Where between the last two readings it crossed.
                const double span = lastDb - levelDb;
                since3 = hops - (span > 1e-9 ? ((topDb - 3.0) - levelDb) / span : 0.0);
            }
            if (levelDb < minDb)
            {
                minDb = levelDb;
                atMin = hops;
            }
            if (minDb <= topDb - 33.0)
            {
                // Far enough: more would only reach the floor.
                fallDone();
                topDb = -200.0;
            }
        }
    }
    lastDb = levelDb;
}

double TailMeter::seconds() const
{
    if (!valid())
        return 0.0;
    int seen = 0;
    for (int b = 0; b < kBins; ++b)
    {
        seen += falls[static_cast<size_t>(b)];
        if (2 * seen >= total)
            return 0.2 * std::pow(12.0 / 0.2, (b + 0.5) / kBins);
    }
    return 0.0;
}

double tailBars(double tailSeconds, double bpm)
{
    return bpm > 20.0 && tailSeconds > 0.0 ? tailSeconds / (240.0 / bpm) : 0.0;
}

} // namespace ma

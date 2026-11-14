#include "ma/Audibility.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kLowestHz = 60.0, kHighestHz = 12000.0;

double power(double db)
{
    return db <= -150.0 ? 0.0 : std::pow(10.0, db / 10.0);
}

double toDb(double p)
{
    return p > 1e-30 ? 10.0 * std::log10(p) : -300.0;
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
} // namespace

std::vector<LostSpan> findLostSpans(const std::vector<AudibilityChannel>& channels)
{
    std::vector<LostSpan> spans;
    const size_t n = channels.size();
    const int bands = SpectrumAnalyzer::numBandsFor(BandScale::erb);
    size_t seconds = 0;
    for (const auto& c : channels)
        if (c.valid)
            seconds = std::max(seconds, c.secondsDb.size());
    if (seconds == 0)
        return spans;

    // Per second, every channel's power per band (0 where not heard).
    const auto at = [&](size_t c, size_t t, int b) {
        const auto& s = channels[c].secondsDb;
        return t < s.size() ? power(s[t][static_cast<size_t>(b)]) : 0.0;
    };

    for (size_t c = 0; c < n; ++c)
    {
        if (!channels[c].valid || channels[c].background)
            continue;
        // Its level per second and its loud level (90th percentile of the seconds it sounds in).
        std::vector<double> level(seconds, -300.0);
        std::vector<double> sounding;
        SpectrumAnalyzer::Bands mean {};
        mean.fill(0.0);
        for (size_t t = 0; t < seconds; ++t)
        {
            double sum = 0.0;
            for (int b = 0; b < bands; ++b)
            {
                const double p = at(c, t, b);
                sum += p;
                mean[static_cast<size_t>(b)] += p;
            }
            level[t] = toDb(sum);
            if (sum > 0.0)
                sounding.push_back(level[t]);
        }
        if (sounding.size() < static_cast<size_t>(kMinLostSeconds))
            continue;
        std::sort(sounding.begin(), sounding.end());
        const double loud = sounding[std::min(sounding.size() - 1, static_cast<size_t>(0.9 * static_cast<double>(sounding.size())))];

        // Its main range: the bands within kOwnRangeDb of its loudest, 60 Hz-12 kHz.
        double peak = 0.0;
        for (int b = 0; b < bands; ++b)
        {
            const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
            if (hz >= kLowestHz && hz <= kHighestHz)
                peak = std::max(peak, mean[static_cast<size_t>(b)]);
        }
        if (peak <= 0.0)
            continue;
        std::vector<int> own;
        for (int b = 0; b < bands; ++b)
        {
            const double hz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
            if (hz >= kLowestHz && hz <= kHighestHz && toDb(mean[static_cast<size_t>(b)]) >= toDb(peak) - kOwnRangeDb)
                own.push_back(b);
        }
        if (own.empty())
            continue;

        std::vector<double> share(seconds, 0.0);
        std::vector<bool> playing(seconds, false);
        std::vector<double> playingShares;
        for (size_t t = 0; t < seconds; ++t)
        {
            playing[t] = level[t] >= loud - kPlayingDb;
            if (!playing[t])
                continue;
            double mine = 0.0, rest = 0.0;
            for (const int b : own)
            {
                mine += at(c, t, b);
                for (size_t o = 0; o < n; ++o)
                    if (o != c && channels[o].valid)
                        rest += at(o, t, b);
            }
            share[t] = toDb(mine) - toDb(mine + rest);
            playingShares.push_back(share[t]);
        }
        const double typical = median(playingShares);
        const double absolute = -10.0 * std::log10(1.0 + std::pow(10.0, kLostAbsoluteDb / 10.0)); // rest 6 dB over it
        const auto lost = [&](size_t t) { return playing[t] && share[t] <= typical - kLostBelowTypicalDb && share[t] <= absolute; };

        // Runs of lost seconds, one second gap allowed.
        size_t t = 0;
        while (t < seconds)
        {
            if (!lost(t))
            {
                ++t;
                continue;
            }
            size_t end = t, gap = 0;
            for (size_t u = t + 1; u < seconds; ++u)
            {
                if (lost(u))
                {
                    end = u;
                    gap = 0;
                }
                else if (++gap > 1)
                    break;
            }
            if (static_cast<int>(end - t + 1) >= kMinLostSeconds)
            {
                LostSpan span;
                span.channel = static_cast<int>(c);
                span.fromSecond = static_cast<int>(t);
                span.toSecond = static_cast<int>(end);
                span.lowHz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, own.front());
                span.highHz = SpectrumAnalyzer::bandCentreHz(BandScale::erb, own.back());
                span.typicalShareDb = typical;
                double mine = 0.0, rest = 0.0, best = 0.0;
                std::vector<double> others(n, 0.0);
                for (size_t u = t; u <= end; ++u)
                    for (const int b : own)
                    {
                        mine += at(c, u, b);
                        for (size_t o = 0; o < n; ++o)
                            if (o != c && channels[o].valid)
                            {
                                const double p = at(o, u, b);
                                rest += p;
                                others[o] += p;
                            }
                    }
                for (size_t o = 0; o < n; ++o)
                    if (others[o] > best)
                    {
                        best = others[o];
                        span.coveredBy = static_cast<int>(o);
                    }
                span.spanShareDb = toDb(mine) - toDb(mine + rest);
                spans.push_back(span);
            }
            t = end + 1;
        }
    }
    return spans;
}

} // namespace ma

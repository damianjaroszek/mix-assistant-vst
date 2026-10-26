#include "ma/StereoPlanner.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kBands = 40;
constexpr double kLowestHz = 120.0;  // below it the cleanup makes the channel mono
constexpr double kHighestHz = 16000.0;
constexpr double kTargetCoherence = 0.2;  // a cut brings the band back up to this
constexpr double kMaxCutDb = 9.0;
constexpr double kMinCutDb = 1.5;
constexpr double kAudibleShare = 0.05;    // of the mix in that band: less and nobody hears it
constexpr double kCulpritSideShare = 0.3; // of all side in a band where the sum is out of phase
constexpr double kNarrowMix = 0.85;       // the sum this mono in a band may take a little side
constexpr double kLiftDb = 1.0;           // "a little": control, not width
constexpr double kLiftLowHz = 1000.0, kLiftHighHz = 10000.0;

double centre(int b)
{
    return SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
}

double power(double db)
{
    return std::pow(10.0, db / 10.0);
}

double toDb(double p)
{
    return 10.0 * std::log10(std::max(p, 1e-30));
}

bool inRange(int b)
{
    return centre(b) >= kLowestHz && centre(b) <= kHighestHz;
}

// Consecutive marked bands (one unmarked band in between still joins them).
std::vector<std::pair<int, int>> groups(const std::array<bool, kBands>& marked)
{
    std::vector<std::pair<int, int>> out;
    for (int b = 0; b < kBands; ++b)
    {
        if (!marked[static_cast<size_t>(b)])
            continue;
        if (!out.empty() && b - out.back().second <= 2)
            out.back().second = b;
        else
            out.emplace_back(b, b);
    }
    return out;
}

StereoSettings::Bell bellFor(int first, int last, double gainDb)
{
    const double low = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, first);
    const double high = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, last + 1);
    const double hz = std::sqrt(low * high);
    StereoSettings::Bell bell;
    bell.hz = static_cast<float>(hz >= 1000.0 ? std::round(hz / 10.0) * 10.0 : std::round(hz));
    bell.db = static_cast<float>(std::round(gainDb * 2.0) / 2.0);
    bell.q = static_cast<float>(std::round(std::clamp(hz / (high - low), 0.7, 6.0) * 10.0) / 10.0);
    return bell;
}
} // namespace

std::vector<PhaseRange> antiPhaseRanges(const StereoMaster& master)
{
    std::vector<PhaseRange> out;
    if (!master.valid)
        return out;
    double loudest = -200.0;
    for (int b = 0; b < kBands; ++b)
        loudest = std::max(loudest, toDb(power(master.midDb[static_cast<size_t>(b)]) + power(master.sideDb[static_cast<size_t>(b)])));
    std::array<bool, kBands> marked {};
    for (int b = 0; b < kBands; ++b)
    {
        const auto at = static_cast<size_t>(b);
        const double level = toDb(power(master.midDb[at]) + power(master.sideDb[at]));
        marked[at] = inRange(b) && level > loudest - 40.0 && coherence(master.midDb[at], master.sideDb[at]) < 0.0;
    }
    for (const auto& [first, last] : groups(marked))
    {
        PhaseRange range;
        range.lowHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, first);
        range.highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, last + 1);
        for (int b = first; b <= last; ++b)
            range.worst = std::min(range.worst, coherence(master.midDb[static_cast<size_t>(b)], master.sideDb[static_cast<size_t>(b)]));
        out.push_back(range);
    }
    return out;
}

std::vector<StereoPlan> planStereo(const std::vector<StereoChannel>& channels, const StereoMaster& master)
{
    const size_t count = channels.size();
    std::vector<StereoPlan> plans(count);

    // What the mix holds per band, from the channels themselves.
    std::array<double, kBands> total {}, totalSide {};
    for (const auto& c : channels)
    {
        if (!c.valid)
            continue;
        for (size_t b = 0; b < total.size(); ++b)
        {
            total[b] += power(c.midDb[b]) + power(c.sideDb[b]);
            totalSide[b] += power(c.sideDb[b]);
        }
    }
    // The sum as the channels add up before their stereo EQ, not as measured on the master:
    // the master already carries the last pass's cuts and lifts, and planning from it would
    // undo them on the next pass. The measured master decides only whether music is there.
    std::array<double, kBands> sumMid {}, sumCoherence {};
    for (const auto& c : channels)
        if (c.valid)
            for (size_t b = 0; b < sumMid.size(); ++b)
                sumMid[b] += power(c.midDb[b]);
    for (size_t b = 0; b < sumCoherence.size(); ++b)
        sumCoherence[b] = coherence(toDb(sumMid[b]), toDb(totalSide[b]));

    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        if (!c.valid || c.role == Role::bus)
            continue;

        // 1. Anti-phase: the channel's own bands under 0, and the bands where the sum is out of
        // phase and this channel brings most of the side.
        std::array<bool, kBands> marked {}, bySum {};
        std::array<double, kBands> own {};
        for (int b = 0; b < kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            own[at] = coherence(c.midDb[at], c.sideDb[at]);
            const double share = total[at] > 0.0 ? (power(c.midDb[at]) + power(c.sideDb[at])) / total[at] : 0.0;
            if (!inRange(b) || share < kAudibleShare)
                continue;
            const double sideShare = totalSide[at] > 0.0 ? power(c.sideDb[at]) / totalSide[at] : 0.0;
            bySum[at] = sumCoherence[at] < 0.0 && own[at] < 0.5 && sideShare >= kCulpritSideShare;
            marked[at] = own[at] < 0.0 || bySum[at];
        }
        size_t slot = 0;
        for (const auto& [first, last] : groups(marked))
        {
            if (slot == plan.settings.side.size())
                break;
            double cut = 0.0, worst = 1.0;
            bool sum = false;
            std::array<double, kBands> needed {};
            for (int b = first; b <= last; ++b)
            {
                const auto at = static_cast<size_t>(b);
                if (!marked[at])
                    continue;
                worst = std::min(worst, own[at]);
                sum = sum || bySum[at];
                // Back in phase: the mix itself where it is out of phase (this channel gives up the side
                // the sum has too much of), else the channel on its own.
                const double side = power(c.sideDb[at]);
                double wanted = power(c.midDb[at]) * (1.0 - kTargetCoherence) / (1.0 + kTargetCoherence);
                if (bySum[at])
                {
                    const double sumWanted = sumMid[at] * (1.0 - kTargetCoherence) / (1.0 + kTargetCoherence);
                    wanted = std::min(wanted, std::max(side - (totalSide[at] - sumWanted), side * 1e-3));
                }
                needed[at] = std::clamp(toDb(wanted) - c.sideDb[at], -kMaxCutDb, 0.0);
                cut = std::min(cut, needed[at]);
            }
            cut = std::max(cut, -kMaxCutDb);
            if (cut > -kMinCutDb)
            {
                if (!sum)
                    continue;
                cut = -kMinCutDb;
            }
            // A bell cuts its edges less than its centre: deepen and widen it until every marked
            // band gets the cut it needs, within the limit.
            auto bell = bellFor(first, last, cut);
            for (int round = 0; round < 12; ++round)
            {
                StereoSettings trial {};
                trial.side[0] = bell;
                double shortfall = 0.0;
                for (int b = first; b <= last; ++b)
                    if (marked[static_cast<size_t>(b)])
                        shortfall = std::max(shortfall, sideResponseDb(trial, centre(b)) - needed[static_cast<size_t>(b)]);
                if (shortfall < 1.0) // within a dB is in phase enough; a wider bell would take side that is fine
                    break;
                bell.db = std::max(static_cast<float>(-kMaxCutDb), bell.db - 0.5f);
                bell.q = std::max(0.7f, std::round(bell.q * 0.85f * 10.0f) / 10.0f);
            }
            plan.settings.side[slot++] = bell;
            StereoPlan::Fix fix;
            fix.lowHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, first);
            fix.highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, last + 1);
            fix.coherenceBefore = worst;
            fix.gainDb = bell.db;
            fix.sumOutOfPhase = sum;
            fix.coherenceAfter = 1.0;
            fix.sumBefore = fix.sumAfter = 1.0;
            for (int b = first; b <= last; ++b)
            {
                const auto at = static_cast<size_t>(b);
                if (!marked[at])
                    continue;
                const double response = sideResponseDb(plan.settings, centre(b));
                fix.coherenceAfter = std::min(fix.coherenceAfter, coherence(c.midDb[at], c.sideDb[at] + response));
                const double sideAfter = totalSide[at] - power(c.sideDb[at]) * (1.0 - power(response));
                fix.sumBefore = std::min(fix.sumBefore, sumCoherence[at]);
                fix.sumAfter = std::min(fix.sumAfter, coherence(toDb(sumMid[at]), toDb(sideAfter)));
            }
            plan.fixes.push_back(fix);
        }

        // 2. A little side where the whole mix is narrow, on a part meant to be wide that
        // has some side to give and stays safely in phase after it.
        const bool wide = c.role == Role::pad || c.role == Role::lead || c.role == Role::effects || c.role == Role::send;
        if (!master.valid || !wide || c.priority < 3 || slot == plan.settings.side.size())
            continue;
        std::array<bool, kBands> narrow {};
        int narrowCount = 0;
        for (int b = 0; b < kBands; ++b)
        {
            const auto at = static_cast<size_t>(b);
            const double share = total[at] > 0.0 ? (power(c.midDb[at]) + power(c.sideDb[at])) / total[at] : 0.0;
            narrow[at] = centre(b) >= kLiftLowHz && centre(b) <= kLiftHighHz && !marked[at] && sumCoherence[at] >= kNarrowMix
                         && own[at] >= 0.3 && own[at] <= 0.9 && share >= 0.1
                         && coherence(c.midDb[at], c.sideDb[at] + kLiftDb) >= kTargetCoherence;
            narrowCount += narrow[at] ? 1 : 0;
        }
        if (narrowCount < 3)
            continue;
        auto spans = groups(narrow);
        const auto widest = *std::max_element(spans.begin(), spans.end(),
                                              [](const auto& a, const auto& b) { return a.second - a.first < b.second - b.first; });
        if (widest.second - widest.first < 2)
            continue;
        const auto bell = bellFor(widest.first, widest.second, kLiftDb);
        plan.settings.side[slot++] = bell;
        StereoPlan::Fix fix;
        fix.lift = true;
        fix.lowHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, widest.first);
        fix.highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, widest.second + 1);
        fix.gainDb = bell.db;
        fix.coherenceBefore = 1.0;
        fix.coherenceAfter = 1.0;
        for (int b = widest.first; b <= widest.second; ++b)
        {
            const auto at = static_cast<size_t>(b);
            fix.coherenceBefore = std::min(fix.coherenceBefore, own[at]);
            fix.coherenceAfter = std::min(fix.coherenceAfter, coherence(c.midDb[at], c.sideDb[at] + sideResponseDb(plan.settings, centre(b))));
        }
        plan.fixes.push_back(fix);
    }
    return plans;
}

} // namespace ma

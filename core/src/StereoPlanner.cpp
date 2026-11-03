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
// Phase 27: a side cut already there stays until the band is clearly back in phase, so a band
// hovering around 0 does not get its cut on one analysis and lose it on the next.
constexpr double kKeepCutCoherence = 0.15;
constexpr double kHeldCutDb = -1.0; // the channel's side EQ cuts this band by at least this
constexpr double kAudibleShare = 0.05;    // of the mix in that band: less and nobody hears it
constexpr double kCulpritSideShare = 0.3; // of all side in a band where the sum is out of phase
constexpr double kNarrowMix = 0.85;       // the sum this mono in a band may take a little side
constexpr double kLiftDb = 1.0;           // "a little": control, not width
constexpr double kLiftLowHz = 1000.0, kLiftHighHz = 10000.0;
constexpr double kMaxWidenLiftDb = 6.0;    // phase 25: the side EQ's ceiling
constexpr double kWidenSlack = 0.1;        // narrower than the references by less is the same width
constexpr double kHasSideDb = -15.0;       // side within this of the mid: lift it; less: the widener
constexpr float kMinWidenDb = -20.0f;      // a widener quieter than this does nothing audible
constexpr int kFirstWidthRegion = 2;       // the low mids (250-700 Hz) onwards

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

// The side a channel will have in band b with its plan (its side EQ and the widener).
double plannedSide(const StereoChannel& c, const StereoSettings& settings, int b)
{
    const auto at = static_cast<size_t>(b);
    if (c.userWidth)
        return processedSidePower(c.current, c.width, centre(b), power(c.midDb[at]), power(c.sideDb[at]));
    return power(c.sideDb[at] + sideResponseDb(settings, centre(b))) + power(c.midDb[at]) * widenPowerRatio(settings, centre(b));
}

// The channel's coherence over a range (power sums), as it is or with a plan.
double rangeCoherence(const StereoChannel& c, const StereoSettings* settings, double lowHz, double highHz)
{
    double mid = 0.0, side = 0.0;
    for (int b = 0; b < kBands; ++b)
    {
        if (centre(b) < lowHz || centre(b) > highHz)
            continue;
        mid += power(c.midDb[static_cast<size_t>(b)]);
        side += settings != nullptr ? plannedSide(c, *settings, b) : power(c.sideDb[static_cast<size_t>(b)]);
    }
    return coherence(toDb(mid), toDb(side));
}

bool mayWidenAtAll(const StereoChannel& c)
{
    return c.group >= 0 && c.group != static_cast<int>(StemGroup::bass) && c.role != Role::kick && c.role != Role::bass
           && c.role != Role::bus && c.role != Role::send;
}

// Phase 25: one channel towards its group's width in the references, region by region above the
// low mids' body. A region with side to give gets a side bell, a mono one the widener (never on a
// vocal: the lead stays in the centre); no band of the channel goes under kSafeCoherence.
void widenToReference(const StereoChannel& c, int index, const WidthTarget& width, StereoPlan& plan, size_t& slot)
{
    if (!mayWidenAtAll(c))
        return;
    const auto g = static_cast<size_t>(c.group);
    struct Region
    {
        int r = 0, first = kBands, last = -1;
        double mid = 0.0, side = 0.0, target = 1.0;
    };
    std::vector<Region> regions;
    double loudest = 0.0;
    for (int r = kFirstWidthRegion; r < kStemRegions; ++r)
    {
        Region region;
        region.r = r;
        for (int b = 0; b < kBands; ++b)
        {
            const double hz = centre(b);
            if (hz < std::max(stemRegionEdgeHz(r), kWidenLowestHz) || hz >= stemRegionEdgeHz(r + 1) || hz > kHighestHz)
                continue;
            region.first = std::min(region.first, b);
            region.last = std::max(region.last, b);
            region.mid += power(c.midDb[static_cast<size_t>(b)]);
            region.side += plannedSide(c, plan.settings, b);
        }
        loudest = std::max(loudest, region.mid + region.side);
        if (region.last >= 0 && width.known[g][static_cast<size_t>(r)])
        {
            region.target = std::clamp(width.coherence[g][static_cast<size_t>(r)], kSafeCoherence, 0.95);
            regions.push_back(region);
        }
    }

    // Only where the part really plays, and only where it is narrower than its group should be.
    std::vector<Region> lifts, widen;
    for (const auto& region : regions)
    {
        if (region.mid + region.side < loudest * 0.01)
            continue;
        if (coherence(toDb(region.mid), toDb(region.side)) <= region.target + kWidenSlack)
            continue;
        (region.side >= region.mid * power(kHasSideDb) ? lifts : widen).push_back(region);
    }

    auto safe = [&](const StereoSettings& settings) {
        for (int b = 0; b < kBands; ++b)
            if (centre(b) >= kWidenLowestHz && centre(b) <= kHighestHz
                && coherence(c.midDb[static_cast<size_t>(b)], toDb(plannedSide(c, settings, b))) < kSafeCoherence - 1e-6
                && coherence(c.midDb[static_cast<size_t>(b)], toDb(plannedSide(c, plan.settings, b))) >= kSafeCoherence)
                return false;
        return true;
    };
    auto record = [&](const Region& first, const Region& last, double target, bool widener, double gainDb) {
        StereoPlan::Fix fix;
        fix.toReference = true;
        fix.lift = !widener;
        fix.widener = widener;
        fix.lowHz = widener ? plan.settings.widen.lowHz : SpectrumAnalyzer::bandEdgeHz(BandScale::erb, first.first);
        fix.highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, last.last + 1);
        fix.gainDb = gainDb;
        fix.referenceCoherence = target;
        plan.fixes.push_back(fix);
    };

    // Neighbouring regions share one broad bell, as much as the one needing least takes.
    for (size_t k = 0; k < lifts.size();)
    {
        size_t end = k + 1;
        while (end < lifts.size() && lifts[end].r == lifts[end - 1].r + 1)
            ++end;
        if (slot >= plan.settings.side.size())
            break;
        double lift = kMaxWidenLiftDb, target = 1.0;
        for (size_t j = k; j < end; ++j)
        {
            const double wanted = lifts[j].mid * (1.0 - lifts[j].target) / (1.0 + lifts[j].target);
            lift = std::min(lift, toDb(wanted / lifts[j].side));
            target = std::min(target, lifts[j].target);
        }
        lift = std::clamp(lift, 0.0, kMaxWidenLiftDb);
        auto trial = plan.settings;
        for (; lift >= 1.0; lift -= 0.5)
        {
            trial = plan.settings;
            trial.side[slot] = bellFor(lifts[k].first, lifts[end - 1].last, lift);
            if (safe(trial))
                break;
        }
        if (lift >= 1.0)
        {
            plan.settings = trial;
            record(lifts[k], lifts[end - 1], target, false, plan.settings.side[slot].db);
            ++slot;
        }
        k = end;
    }

    if (c.role == Role::vocal || widen.empty())
        return;
    // One widener for the whole part: from the lowest region that needs it, as much as the region
    // needing least takes (none goes wider than its group in the references), within the safe margin.
    double ratio = 1e9, target = 1.0;
    for (const auto& region : widen)
    {
        const double wanted = region.mid * (1.0 - region.target) / (1.0 + region.target);
        const double needed = std::max(0.0, wanted - region.side) / std::max(region.mid, 1e-30);
        if (needed < ratio)
        {
            ratio = needed;
            target = region.target;
        }
    }
    auto trial = plan.settings;
    trial.widen.lowHz = static_cast<float>(std::round(std::max(kWidenLowestHz, stemRegionEdgeHz(widen.front().r)) / 10.0) * 10.0);
    trial.widen.delayMs = 5.0f + 1.5f * static_cast<float>(index % 4); // channels apart do not widen alike
    trial.widen.amountDb = static_cast<float>(std::round(10.0 * std::log10(std::max(ratio, 1e-6)) * 2.0) / 2.0);
    while (trial.widen.amountDb >= kMinWidenDb && !safe(trial))
        trial.widen.amountDb -= 0.5f;
    if (trial.widen.amountDb < kMinWidenDb)
        return;
    plan.settings = trial;
    record(widen.front(), widen.back(), target, true, trial.widen.amountDb);
}

// Phase 25: the parts widened together must not take the sum under the margin either: where they
// do, every widening that reaches the band steps back (the widener 1 dB, a reference lift 0.5 dB).
void keepSumInPhase(const std::vector<StereoChannel>& channels, std::vector<StereoPlan>& plans)
{
    auto sumCoherence = [&](int b, bool planned) {
        double mid = 0.0, side = 0.0;
        for (size_t i = 0; i < channels.size(); ++i)
        {
            if (!channels[i].valid)
                continue;
            mid += power(channels[i].midDb[static_cast<size_t>(b)]);
            side += planned ? plannedSide(channels[i], plans[i].settings, b) : power(channels[i].sideDb[static_cast<size_t>(b)]);
        }
        return coherence(toDb(mid), toDb(side));
    };
    for (int round = 0; round < 40; ++round)
    {
        int worst = -1;
        for (int b = 0; b < kBands; ++b)
            if (centre(b) >= kWidenLowestHz && centre(b) <= kHighestHz && sumCoherence(b, true) < kSafeCoherence
                && sumCoherence(b, true) < sumCoherence(b, false) - 1e-6 && (worst < 0 || sumCoherence(b, true) < sumCoherence(worst, true)))
                worst = b;
        if (worst < 0)
            return;
        bool changed = false;
        for (auto& plan : plans)
            for (auto& fix : plan.fixes)
            {
                if (!fix.toReference)
                    continue;
                if (fix.widener && widenOn(plan.settings) && centre(worst) >= plan.settings.widen.lowHz)
                {
                    plan.settings.widen.amountDb -= 1.0f;
                    if (!widenOn(plan.settings) || plan.settings.widen.amountDb < kMinWidenDb)
                        plan.settings.widen.amountDb = -60.0f;
                    fix.gainDb = widenOn(plan.settings) ? plan.settings.widen.amountDb : 0.0;
                    changed = true;
                }
                else if (fix.lift && centre(worst) >= fix.lowHz && centre(worst) <= fix.highHz)
                    for (auto& bell : plan.settings.side)
                        if (bell.db > 0.0f && std::abs(std::log2(bell.hz / std::sqrt(fix.lowHz * fix.highHz))) < 0.3)
                        {
                            bell.db = std::max(0.0f, bell.db - 0.5f);
                            fix.gainDb = bell.db;
                            changed = true;
                        }
            }
        if (!changed)
            return;
    }
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

WidthTarget widthTargetFrom(const StemTarget& target)
{
    WidthTarget out;
    for (int g = 0; g < kStemGroups; ++g)
        for (int r = kFirstWidthRegion; r < kStemRegions; ++r)
        {
            std::vector<double> values;
            for (const auto& profile : target.profiles)
            {
                if (!profile.hasBands || !profile.measure.groups[static_cast<size_t>(g)].present)
                    continue;
                double mid = 0.0, side = 0.0;
                for (int b = 0; b < kBands; ++b)
                {
                    const double hz = centre(b);
                    if (hz < std::max(stemRegionEdgeHz(r), kWidenLowestHz) || hz >= stemRegionEdgeHz(r + 1) || hz > kHighestHz)
                        continue;
                    mid += power(profile.groupDb[static_cast<size_t>(g)][static_cast<size_t>(b)]);
                    side += power(profile.sideDb[static_cast<size_t>(g)][static_cast<size_t>(b)]);
                }
                if (mid + side > 1e-12)
                    values.push_back(coherence(toDb(mid), toDb(side)));
            }
            if (values.empty())
                continue;
            std::sort(values.begin(), values.end());
            const size_t n = values.size();
            out.coherence[static_cast<size_t>(g)][static_cast<size_t>(r)] = n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
            out.known[static_cast<size_t>(g)][static_cast<size_t>(r)] = true;
            out.valid = true;
        }
    return out;
}

std::vector<StereoPlan> planStereo(const std::vector<StereoChannel>& channels, const StereoMaster& master, const WidthTarget* width)
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
            // A channel on its width slider counts as it plays; the others before their stereo EQ.
            const double side = c.userWidth ? plannedSide(c, c.current, static_cast<int>(b)) : power(c.sideDb[b]);
            total[b] += power(c.midDb[b]) + side;
            totalSide[b] += side;
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
        if (!c.valid || c.role == Role::bus || c.userWidth)
            continue;

        // 1. Anti-phase: the channel's own bands under 0, and the bands where the sum is out of
        // phase and this channel brings most of the side.
        std::array<bool, kBands> marked {}, bySum {}, held {};
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
            held[at] = sideResponseDb(c.current, centre(b)) < kHeldCutDb && own[at] < kKeepCutCoherence;
            marked[at] = own[at] < 0.0 || bySum[at] || held[at];
        }
        size_t slot = 0;
        for (const auto& [first, last] : groups(marked))
        {
            if (slot == plan.settings.side.size())
                break;
            double cut = 0.0, worst = 1.0;
            bool sum = false, keep = false;
            double heldDepth = 0.0;
            std::array<double, kBands> needed {};
            for (int b = first; b <= last; ++b)
            {
                const auto at = static_cast<size_t>(b);
                if (!marked[at])
                    continue;
                worst = std::min(worst, own[at]);
                sum = sum || bySum[at];
                keep = keep || held[at];
                if (held[at])
                    heldDepth = std::min(heldDepth, sideResponseDb(c.current, centre(b)));
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
            // A cut it keeps stays as deep as it is (a lighter one would come back deeper next time).
            if (keep)
                cut = std::min(cut, heldDepth);
            cut = std::max(cut, -kMaxCutDb);
            if (cut > -kMinCutDb)
            {
                if (!sum && !keep)
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

        // 2 (phase 25). With references: each part to its group's width there.
        if (width != nullptr && width->valid)
        {
            widenToReference(c, static_cast<int>(i), *width, plan, slot);
            continue;
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
    if (width != nullptr && width->valid)
    {
        keepSumInPhase(channels, plans);
        for (size_t i = 0; i < count; ++i)
            for (auto& fix : plans[i].fixes)
                if (fix.toReference)
                {
                    fix.coherenceBefore = rangeCoherence(channels[i], nullptr, fix.lowHz, fix.highHz);
                    fix.coherenceAfter = rangeCoherence(channels[i], &plans[i].settings, fix.lowHz, fix.highHz);
                }
        // A widening the sum check took all the way back is no fix.
        for (auto& plan : plans)
            plan.fixes.erase(std::remove_if(plan.fixes.begin(), plan.fixes.end(),
                                            [&](const StereoPlan::Fix& fix) {
                                                return fix.toReference && (fix.widener ? !widenOn(plan.settings) : fix.gainDb < 0.5);
                                            }),
                             plan.fixes.end());
    }
    return plans;
}

} // namespace ma

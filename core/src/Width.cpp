#include "ma/Width.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kBands = static_cast<int>(SpectrumAnalyzer::Bands {}.size());
constexpr double kHighestHz = 16000.0;
constexpr double kAudibleRangeDb = 40.0; // under the channel's loudest band by more: nobody hears its phase
constexpr double kMaxGuardDb = 24.0;
constexpr double kSumStep = 0.85;        // a widening the sum cannot take shrinks by this each round
// The sum is planned from the channels' power sums, which miss how their sides line up (a widener's
// delayed mid against the part's own side, a vocal phrase coming and going), so it keeps a margin.
constexpr double kSumPlanCoherence = kSafeWidthCoherence + 0.05;

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

// Bands where the channel plays, from its slider's crossover up.
std::array<bool, kBands> audibleBands(const WidthInput& c)
{
    std::array<bool, kBands> out {};
    double loudest = -300.0;
    for (int b = 0; b < kBands; ++b)
        loudest = std::max(loudest, toDb(power(c.midDb[static_cast<size_t>(b)]) + power(c.sideDb[static_cast<size_t>(b)])));
    for (int b = 0; b < kBands; ++b)
    {
        const double level = toDb(power(c.midDb[static_cast<size_t>(b)]) + power(c.sideDb[static_cast<size_t>(b)]));
        out[static_cast<size_t>(b)] = centre(b) >= c.fromHz && centre(b) <= kHighestHz && level > loudest - kAudibleRangeDb && level > -100.0;
    }
    return out;
}

double bandCoherence(const WidthInput& c, const WidthSettings& w, int b)
{
    const double mid = power(c.midDb[static_cast<size_t>(b)]);
    const double side = processedSidePower(c.stereo, w, centre(b), mid, power(c.sideDb[static_cast<size_t>(b)]));
    return coherence(toDb(mid), toDb(side));
}

// The slider without guards: below 100% the side scaled down, above it raised by up to
// kMaxWidthSideDb with the widener up to kMaxWidthWidenDb, `amount` (0..1) of the way.
WidthSettings baseSettings(const WidthInput& c, double amount)
{
    WidthSettings w;
    w.active = 1.0f;
    w.fromHz = c.fromHz;
    w.delayMs = c.delayMs;
    const double request = std::clamp(static_cast<double>(c.request), 0.0, static_cast<double>(kWidthMax));
    if (request <= kWidthNeutral)
    {
        w.scale = static_cast<float>(request / kWidthNeutral);
        return w;
    }
    w.scale = static_cast<float>(std::pow(10.0, kMaxWidthSideDb * amount / 20.0));
    const double gain = amount * std::pow(10.0, kMaxWidthWidenDb / 20.0);
    w.widenDb = gain > 0.01 ? static_cast<float>(20.0 * std::log10(gain)) : -60.0f;
    return w;
}

// Guard bells for the bands under the safe coherence. False when two are not enough.
bool addGuards(const WidthInput& c, const std::array<bool, kBands>& audible, WidthSettings& w)
{
    for (auto& g : w.guard)
        g = {};
    const double safeRatio = (1.0 - kSafeWidthCoherence) / (1.0 + kSafeWidthCoherence);
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        // Each band's missing cut with the guards so far.
        std::array<double, kBands> need {};
        bool any = false;
        for (int b = 0; b < kBands; ++b)
        {
            if (!audible[static_cast<size_t>(b)])
                continue;
            const double mid = power(c.midDb[static_cast<size_t>(b)]);
            const double side = processedSidePower(c.stereo, w, centre(b), mid, power(c.sideDb[static_cast<size_t>(b)]));
            if (side > safeRatio * mid * 1.02)
            {
                need[static_cast<size_t>(b)] = toDb(safeRatio * mid) - toDb(side); // negative
                any = true;
            }
        }
        if (!any)
            return true;
        // Runs of bands that need it (a band's gap still joins them).
        std::vector<std::pair<int, int>> runs;
        for (int b = 0; b < kBands; ++b)
            if (need[static_cast<size_t>(b)] < 0.0)
            {
                if (!runs.empty() && b - runs.back().second <= 2)
                    runs.back().second = b;
                else
                    runs.emplace_back(b, b);
            }
        // An existing guard over a run deepens; a new run takes a free guard.
        for (const auto& run : runs)
        {
            double deepest = 0.0;
            for (int b = run.first; b <= run.second; ++b)
                deepest = std::min(deepest, need[static_cast<size_t>(b)]);
            const double lowHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, run.first);
            const double highHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, run.second + 1);
            const double hz = std::sqrt(lowHz * highHz);
            StereoSettings::Bell* bell = nullptr;
            for (auto& g : w.guard)
                if (std::abs(g.db) > 0.01f && std::abs(std::log2(g.hz / hz)) < 0.5)
                    bell = &g;
            if (bell == nullptr)
                for (auto& g : w.guard)
                    if (std::abs(g.db) <= 0.01f)
                    {
                        bell = &g;
                        bell->hz = static_cast<float>(hz);
                        bell->q = static_cast<float>(std::clamp(hz / std::max(1.0, highHz - lowHz), 0.7, 6.0));
                        bell->db = 0.0f;
                        break;
                    }
            if (bell == nullptr)
                return false;
            bell->db = static_cast<float>(std::max(-kMaxGuardDb, static_cast<double>(bell->db) + deepest - 1.0));
        }
    }
    // Whatever the guards could not reach.
    for (int b = 0; b < kBands; ++b)
        if (audible[static_cast<size_t>(b)] && bandCoherence(c, w, b) < kSafeWidthCoherence - 0.02)
            return false;
    return true;
}

void describe(const WidthInput& c, const std::array<bool, kBands>& audible, WidthPlan& plan)
{
    plan.worstCoherence = 1.0;
    plan.worstHz = 0.0;
    for (int b = 0; b < kBands; ++b)
    {
        plan.coherenceAfter[static_cast<size_t>(b)] = bandCoherence(c, plan.settings, b);
        if (audible[static_cast<size_t>(b)] && plan.coherenceAfter[static_cast<size_t>(b)] < plan.worstCoherence)
        {
            plan.worstCoherence = plan.coherenceAfter[static_cast<size_t>(b)];
            plan.worstHz = centre(b);
        }
    }
}

// The channel's plan with the widening `amount` (0..1) of the full one; false if unsafe.
bool planAt(const WidthInput& c, const std::array<bool, kBands>& audible, double amount, WidthSettings& out)
{
    out = baseSettings(c, amount);
    return addGuards(c, audible, out);
}

// A part out of phase in more places than two guards can reach (a comb from a short stereo
// delay, say): its side comes down as a whole until it is safe.
void narrowUntilSafe(const WidthInput& c, const std::array<bool, kBands>& audible, WidthSettings& w)
{
    const auto base = w;
    double lo = 0.0, hi = 1.0;
    WidthSettings best = base;
    best.scale = 0.0f;
    best.widenDb = -60.0f;
    addGuards(c, audible, best);
    for (int i = 0; i < 12; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        WidthSettings trial = base;
        trial.scale = static_cast<float>(base.scale * mid);
        if (addGuards(c, audible, trial))
        {
            lo = mid;
            best = trial;
        }
        else
            hi = mid;
    }
    w = best;
}

WidthPlan planChannel(const WidthInput& c, double ceiling)
{
    WidthPlan plan;
    if (!c.valid || !widthRequested(c.request))
        return plan;
    const auto audible = audibleBands(c);
    const double wanted = std::max(0.0, (static_cast<double>(c.request) - kWidthNeutral) / (kWidthMax - kWidthNeutral));
    if (wanted <= 0.0)
    {
        // Narrower: never less safe, but bands already out of phase still get their guard.
        if (!planAt(c, audible, 0.0, plan.settings))
            narrowUntilSafe(c, audible, plan.settings);
        describe(c, audible, plan);
        return plan;
    }
    // The widest that stays safe (two guards at most), found by halving.
    double lo = 0.0, hi = std::min(1.0, ceiling);
    WidthSettings best;
    if (!planAt(c, audible, 0.0, best))
    {
        // Not safe even as it is: no widening, its side comes down instead.
        narrowUntilSafe(c, audible, best);
        plan.settings = best;
        describe(c, audible, plan);
        return plan;
    }
    WidthSettings trial;
    if (planAt(c, audible, hi, trial))
    {
        lo = hi;
        best = trial;
    }
    else
        for (int i = 0; i < 12; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (planAt(c, audible, mid, trial))
            {
                lo = mid;
                best = trial;
            }
            else
                hi = mid;
        }
    plan.reach = lo;
    // The slider spreads over what is safe: its top is the widest safe width.
    if (wanted < 1.0 && lo > 0.0)
        planAt(c, audible, wanted * lo, best);
    plan.settings = best;
    describe(c, audible, plan);
    return plan;
}

double sumCoherence(const std::vector<WidthInput>& channels, const std::vector<WidthPlan>* plans, int b, double& mid)
{
    mid = 0.0;
    double side = 0.0;
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const auto& c = channels[i];
        if (!c.valid || !c.inMix)
            continue;
        const double m = power(c.midDb[static_cast<size_t>(b)]);
        mid += m;
        side += processedSidePower(c.stereo, plans != nullptr ? (*plans)[i].settings : WidthSettings {}, centre(b), m,
                                   power(c.sideDb[static_cast<size_t>(b)]));
    }
    return coherence(toDb(mid), toDb(side));
}
// How many channels bring at least a fifth of the sum's side in a band.
int sideSources(const std::vector<WidthInput>& channels, const std::vector<WidthPlan>& plans, int b)
{
    std::vector<double> sides(channels.size(), 0.0);
    double total = 0.0;
    for (size_t i = 0; i < channels.size(); ++i)
    {
        const auto& c = channels[i];
        if (!c.valid || !c.inMix)
            continue;
        sides[i] = processedSidePower(c.stereo, plans[i].settings, centre(b), power(c.midDb[static_cast<size_t>(b)]),
                                      power(c.sideDb[static_cast<size_t>(b)]));
        total += sides[i];
    }
    int count = 0;
    for (double side : sides)
        count += total > 0.0 && side >= 0.2 * total ? 1 : 0;
    return count;
}
} // namespace

bool widthRequested(float request)
{
    return std::abs(request - kWidthNeutral) >= 0.5f;
}

float widthDelayMs(int slot)
{
    // 4.5 to 13.5 ms in 0.9 ms steps, neighbouring slots far apart.
    const int step = ((slot < 0 ? 0 : slot) * 7) % 11;
    return 4.5f + 0.9f * static_cast<float>(step);
}

std::vector<WidthPlan> planWidth(const std::vector<WidthInput>& channels)
{
    std::vector<double> ceiling(channels.size(), 1.0);
    std::vector<WidthPlan> plans(channels.size());
    for (size_t i = 0; i < channels.size(); ++i)
        plans[i] = planChannel(channels[i], 1.0);

    // The sum: a band under the safe coherence that the sliders made worse narrows the widenings in it.
    for (int round = 0; round < 30; ++round)
    {
        int worst = -1;
        double worstCoherence = kSumPlanCoherence;
        std::array<double, kBands> without {}, mids {};
        double loudest = -300.0;
        for (int b = 0; b < kBands; ++b)
        {
            without[static_cast<size_t>(b)] = sumCoherence(channels, nullptr, b, mids[static_cast<size_t>(b)]);
            loudest = std::max(loudest, toDb(mids[static_cast<size_t>(b)]));
        }
        for (int b = 0; b < kBands; ++b)
        {
            if (centre(b) > kHighestHz || toDb(mids[static_cast<size_t>(b)]) <= loudest - kAudibleRangeDb)
                continue;
            double mid = 0.0;
            const double now = sumCoherence(channels, &plans, b, mid);
            // The margin is for sides that meet; one part alone is the channel's own check.
            if (now >= kSafeWidthCoherence && sideSources(channels, plans, b) < 2)
                continue;
            if (now < worstCoherence && now < without[static_cast<size_t>(b)] - 1e-6)
            {
                worst = b;
                worstCoherence = now;
            }
        }
        if (worst < 0)
            break;
        bool changed = false;
        for (size_t i = 0; i < channels.size(); ++i)
        {
            const auto& c = channels[i];
            if (!c.valid || !c.inMix || plans[i].reach <= 0.0 || c.request <= kWidthNeutral)
                continue;
            const double m = power(c.midDb[static_cast<size_t>(worst)]);
            const double before = processedSidePower(c.stereo, WidthSettings {}, centre(worst), m, power(c.sideDb[static_cast<size_t>(worst)]));
            const double after = processedSidePower(c.stereo, plans[i].settings, centre(worst), m, power(c.sideDb[static_cast<size_t>(worst)]));
            if (after <= before * 1.01)
                continue;
            ceiling[i] = std::max(0.0, std::min(ceiling[i], plans[i].reach) * kSumStep);
            if (ceiling[i] < 0.02)
                ceiling[i] = 0.0;
            const double hz = centre(worst);
            plans[i] = planChannel(c, ceiling[i]);
            plans[i].limitedBySum = true;
            plans[i].sumLimitHz = hz;
            changed = true;
        }
        if (!changed)
            break;
    }
    return plans;
}

} // namespace ma

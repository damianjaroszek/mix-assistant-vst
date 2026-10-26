#include "ma/ChainPlanner.h"

#include "ma/Ducker.h"

#include <algorithm>
#include <cmath>

namespace ma
{
namespace
{
constexpr int kBands = 40;
constexpr double kJunkDb = 24.0;      // content this far under the channel's own peak is not its sound
constexpr double kOwnedDb = 6.0;      // the kick and bass this much louder in a band own it
constexpr double kMonoBelowHz = 120.0;
constexpr double kLostMarginDb = -12.0; // quiet parts this far under the rest in their own bands are lost
constexpr double kMudLowHz = 200.0, kMudHighHz = 500.0;
constexpr double kMudShare = 0.15;    // a channel with this share of the mix there crowds it

struct RoleProfile
{
    double highPassCapHz;  // highest the high-pass may go (0 = never)
    bool compress;
    double targetSpreadDb; // loud minus quiet parts a well-controlled part of this kind keeps
    double attackMs;
    double releaseNote;    // release as a note length at the song's tempo
    double maxRatio;
    double maxCrestDb;     // peaks over the loud parts' RMS beyond this only cost headroom
    double maxClipDb;
    double softness;
    double minCrestDb;     // under this the part arrived squashed
    double maxReductionDb; // most the compressor takes off the loud parts; past it the part sounds squashed
};

RoleProfile profileFor(Role role)
{
    switch (role)
    {
    case Role::kick: return { 30.0, false, 0.0, 25.0, 1.0 / 16.0, 3.0, 10.0, 3.0, 0.3, 6.0, 0.0 };
    case Role::bass: return { 30.0, true, 4.0, 10.0, 1.0 / 16.0, 4.0, 10.0, 2.0, 0.6, 4.0, 6.0 };
    case Role::percussion: return { 500.0, false, 0.0, 15.0, 1.0 / 16.0, 3.0, 13.0, 4.0, 0.3, 7.0, 0.0 };
    case Role::vocal: return { 150.0, true, 5.0, 5.0, 1.0 / 8.0, 4.0, 14.0, 2.0, 0.8, 6.0, 8.0 };
    case Role::lead: return { 250.0, true, 5.0, 10.0, 1.0 / 16.0, 4.0, 13.0, 2.0, 0.8, 5.0, 6.0 };
    case Role::pad: return { 250.0, true, 4.0, 30.0, 1.0 / 8.0, 3.0, 12.0, 0.0, 0.8, 4.0, 4.0 };
    case Role::effects: return { 300.0, false, 0.0, 10.0, 1.0 / 8.0, 2.0, 99.0, 0.0, 0.8, 0.0, 0.0 };
    case Role::send: return { 250.0, false, 0.0, 10.0, 1.0 / 8.0, 2.0, 99.0, 0.0, 0.8, 0.0, 0.0 };
    case Role::bus: return { 0.0, false, 0.0, 10.0, 1.0 / 8.0, 2.0, 99.0, 0.0, 0.8, 0.0, 0.0 };
    case Role::other: break;
    }
    return { 100.0, true, 6.0, 10.0, 1.0 / 8.0, 4.0, 14.0, 2.0, 0.7, 5.0, 6.0 };
}

double power(double db)
{
    return std::pow(10.0, db / 10.0);
}

double toDb(double p)
{
    return 10.0 * std::log10(std::max(p, 1e-30));
}

double centre(int b)
{
    return SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
}

bool ownsLowEnd(Role role)
{
    return role == Role::kick || role == Role::bass;
}

// A frequency a little under `hz`, rounded to 5 Hz: the -3 dB point sits under the content.
double belowContent(double hz)
{
    return std::max(20.0, std::round(0.8 * hz / 5.0) * 5.0);
}
} // namespace

std::vector<ChainPlan> planChains(const std::vector<ChainChannel>& channels, const ChainContext& context)
{
    const size_t count = channels.size();
    std::vector<ChainPlan> plans(count);
    const double bpm = context.bpm > 0.0 ? context.bpm : 124.0;

    // Who owns the low end: the kick and bass together, band by band.
    SpectrumAnalyzer::Bands owners {};
    bool anyOwner = false;
    for (const auto& c : channels)
        if (c.valid && ownsLowEnd(c.role))
        {
            anyOwner = true;
            for (int b = 0; b < kBands; ++b)
                owners[static_cast<size_t>(b)] += power(c.bandsDb[static_cast<size_t>(b)]);
        }

    // 1. High-pass and mono low end.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        const auto profile = profileFor(c.role);
        if (!c.valid || profile.highPassCapHz <= 0.0)
            continue;
        plan.cleanup.monoBelowHz = static_cast<float>(kMonoBelowHz);

        double peak = -300.0;
        for (int b = 0; b < kBands; ++b)
            peak = std::max(peak, c.bandsDb[static_cast<size_t>(b)]);
        int edge = 0; // lowest band that is part of the sound
        while (edge < kBands - 1 && c.bandsDb[static_cast<size_t>(edge)] < peak - kJunkDb)
            ++edge;
        const double edgeHz = SpectrumAnalyzer::bandEdgeHz(BandScale::erb, edge);

        if (ownsLowEnd(c.role))
        {
            // Only what nobody hears: rumble under the lowest note or the kick's body.
            plan.cleanup.hpfHz = static_cast<float>(std::clamp(belowContent(edgeHz), 20.0, profile.highPassCapHz));
            plan.highPassReason = ChainPlan::HighPass::subsonic;
            continue;
        }

        // Junk only if something actually plays under the sound (rumble, noise, bleed).
        double under = -300.0;
        for (int b = 0; b < edge; ++b)
            under = std::max(under, c.bandsDb[static_cast<size_t>(b)]);
        const bool junk = edge > 0 && under > -100.0 && under > peak - 60.0;
        double hz = junk ? belowContent(edgeHz) : 0.0;
        plan.highPassReason = junk ? ChainPlan::HighPass::junk : ChainPlan::HighPass::none;
        // Where the kick and bass are much louder, this channel's low end only adds mud.
        if (anyOwner && c.role != Role::other)
        {
            int owned = 0;
            while (owned < kBands && centre(owned) < profile.highPassCapHz
                   && toDb(owners[static_cast<size_t>(owned)]) - c.bandsDb[static_cast<size_t>(owned)] >= kOwnedDb)
                ++owned;
            const double ownedHz = belowContent(SpectrumAnalyzer::bandEdgeHz(BandScale::erb, owned));
            if (owned > 0 && ownedHz > hz)
            {
                hz = ownedHz;
                plan.highPassReason = ChainPlan::HighPass::lowEndOwned;
            }
        }
        hz = std::min(hz, profile.highPassCapHz);
        if (hz >= 25.0)
            plan.cleanup.hpfHz = static_cast<float>(hz);
        else
            plan.highPassReason = ChainPlan::HighPass::none;
    }

    // 2. Low-mid mud: several channels crowding 200-500 Hz; the less important ones give way.
    {
        std::vector<SpectrumAnalyzer::Bands> heard(count);
        SpectrumAnalyzer::Bands mix {};
        for (size_t i = 0; i < count; ++i)
        {
            if (!channels[i].valid)
                continue;
            for (int b = 0; b < kBands; ++b)
            {
                const double db = channels[i].bandsDb[static_cast<size_t>(b)] + cleanupResponseDb(plans[i].cleanup, centre(b));
                heard[i][static_cast<size_t>(b)] = db;
                mix[static_cast<size_t>(b)] += power(db);
            }
        }
        double mixMud = 0.0;
        std::vector<double> share(count, 0.0);
        for (int b = 0; b < kBands; ++b)
            if (centre(b) >= kMudLowHz && centre(b) <= kMudHighHz)
                mixMud += mix[static_cast<size_t>(b)];
        std::vector<size_t> crowd;
        for (size_t i = 0; i < count; ++i)
        {
            if (!channels[i].valid || mixMud <= 0.0)
                continue;
            double own = 0.0;
            for (int b = 0; b < kBands; ++b)
                if (centre(b) >= kMudLowHz && centre(b) <= kMudHighHz)
                    own += power(heard[i][static_cast<size_t>(b)]);
            share[i] = own / mixMud;
            if (share[i] >= kMudShare)
                crowd.push_back(i);
        }
        if (crowd.size() >= 3)
        {
            // The most important member keeps its low mids; kick and bass only give way when
            // their 200-500 Hz is close to their low peak (boxy rather than defining).
            std::sort(crowd.begin(), crowd.end(), [&](size_t a, size_t b) {
                return channels[a].priority != channels[b].priority ? channels[a].priority < channels[b].priority : share[a] > share[b];
            });
            for (size_t k = 1; k < crowd.size(); ++k)
            {
                const size_t i = crowd[k];
                const auto& c = channels[i];
                int loudest = -1;
                double lowPeak = -300.0;
                for (int b = 0; b < kBands; ++b)
                {
                    const double db = heard[i][static_cast<size_t>(b)];
                    if (centre(b) < 120.0)
                        lowPeak = std::max(lowPeak, db);
                    if (centre(b) >= kMudLowHz && centre(b) <= kMudHighHz && (loudest < 0 || db > heard[i][static_cast<size_t>(loudest)]))
                        loudest = b;
                }
                if (loudest < 0)
                    continue;
                if (ownsLowEnd(c.role) && heard[i][static_cast<size_t>(loudest)] < lowPeak - 6.0)
                    continue;
                auto& cleanup = plans[i].cleanup;
                cleanup.mudHz = static_cast<float>(std::round(centre(loudest) / 5.0) * 5.0);
                cleanup.mudDb = share[i] >= 0.3 && c.priority >= 5 ? -3.0f : -2.0f;
                cleanup.mudQ = 1.0f;
                plans[i].mudCrowd = static_cast<int>(crowd.size());
            }
        }
    }

    // 3. Dynamics: compression from the spread of levels, clipping from the peaks.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        const auto profile = profileFor(c.role);
        if (!c.valid || !c.hasStats || c.levelP95 < -100.0)
            continue;
        plan.dynamicsKnown = true;
        plan.spreadDb = c.levelP95 - c.levelP10;
        plan.spreadAfterDb = plan.spreadDb;
        plan.crestDb = c.truePeakDb - c.levelP95;
        plan.maxCrestDb = profile.maxCrestDb;
        plan.percussive = c.role == Role::kick || c.role == Role::percussion;
        plan.overCompressed = !c.intentionalDrive && profile.minCrestDb > 0.0 && plan.crestDb < profile.minCrestDb;

        // In the mix: do its quiet moments sink under everything else in its own best bands?
        if (profile.compress && !plan.percussive)
        {
            std::array<int, 3> top { -1, -1, -1 };
            for (int b = 0; b < kBands; ++b)
            {
                for (size_t t = 0; t < top.size(); ++t)
                {
                    if (top[t] < 0 || c.bandsDb[static_cast<size_t>(b)] > c.bandsDb[static_cast<size_t>(top[t])])
                    {
                        for (size_t m = top.size() - 1; m > t; --m)
                            top[m] = top[m - 1];
                        top[t] = b;
                        break;
                    }
                }
            }
            double margin = 0.0;
            int used = 0;
            for (int b : top)
            {
                if (b < 0)
                    continue;
                double others = 0.0;
                for (size_t j = 0; j < count; ++j)
                    if (j != i && channels[j].valid)
                        others += power(channels[j].bandsDb[static_cast<size_t>(b)] + cleanupResponseDb(plans[j].cleanup, centre(b)));
                margin += c.bandsDb[static_cast<size_t>(b)] + (c.levelP10 - c.levelP50) - toDb(others);
                ++used;
            }
            plan.lostInQuietParts = used > 0 && margin / used < kLostMarginDb;
        }

        plan.targetSpreadDb = profile.targetSpreadDb - (plan.lostInQuietParts ? 1.5 : 0.0);
        if (profile.compress && !plan.percussive && !plan.overCompressed && plan.spreadDb > plan.targetSpreadDb + 1.0)
        {
            auto& comp = plan.compressor;
            const double half = 0.5 * plan.targetSpreadDb;
            const double threshold = c.levelP10 + half;
            const double ratio = std::clamp((c.levelP95 - threshold) / half, 1.5, profile.maxRatio);
            comp.on = 1.0f;
            comp.thresholdDb = static_cast<float>(std::round(threshold * 2.0) / 2.0);
            comp.ratio = static_cast<float>(std::round(ratio * 10.0) / 10.0);
            comp.kneeDb = 6.0f;
            comp.attackMs = static_cast<float>(profile.attackMs);
            comp.releaseMs = static_cast<float>(std::clamp(syncedReleaseMs(bpm, profile.releaseNote), 30.0, 400.0));
            // Never deeper than the role takes: a quiet verse or a breakdown is arrangement, not
            // uneven playing, and squashing it flat is what an engineer would call over-compressed.
            while (compressorReductionDb(comp, c.levelP95) > profile.maxReductionDb + 0.25 && comp.thresholdDb < c.levelP95)
                comp.thresholdDb += 0.5f;
            // The median level stays where gain staging put it, so the balance holds.
            comp.makeupDb = static_cast<float>(std::round(compressorReductionDb(comp, c.levelP50) * 10.0) / 10.0);
            plan.spreadAfterDb = c.levelP95 - compressorReductionDb(comp, c.levelP95) - (c.levelP10 - compressorReductionDb(comp, c.levelP10));
            plan.arrangementDb = plan.spreadAfterDb > plan.targetSpreadDb + 1.5 ? plan.spreadAfterDb - plan.targetSpreadDb : 0.0;
        }

        const double excess = plan.crestDb - profile.maxCrestDb;
        double maxClip = profile.maxClipDb;
        if (plan.percussive && context.headroomShortDb > 0.0)
            maxClip += 1.0;
        if (!c.intentionalDrive && !plan.overCompressed && maxClip > 0.0 && excess >= 1.0)
        {
            plan.clipDb = std::min(excess, maxClip);
            plan.clipper.on = 1.0f;
            plan.clipper.ceilingDb = static_cast<float>(std::round((c.truePeakDb - plan.clipDb) * 10.0) / 10.0);
            plan.clipper.softness = static_cast<float>(profile.softness);
        }

        if (c.role == Role::bass && c.lowNoteHz > 0.0)
        {
            plan.evener.on = 1.0f;
            plan.evener.amount = 0.6f;
            plan.evener.referenceHz = static_cast<float>(c.lowNoteHz);
        }
    }
    return plans;
}

} // namespace ma

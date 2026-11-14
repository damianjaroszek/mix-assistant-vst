#include "ma/ChainPlanner.h"

#include "ma/Ducker.h"
#include "ma/Masking.h"

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
constexpr double kBodyDb = 12.0;      // within this of the channel's own peak is its body: no high-pass reaches into it
constexpr double kLowEndFloorHz = 30.0; // the kick's and the bass's high-pass never sits lower
constexpr double kTopHz = 5000.0;     // hats, shakers and rides live above this ...
constexpr double kTopShare = 0.6;     // ... with at least this share of their power
constexpr double kLostMarginDb = -12.0; // quiet parts this far under the rest in their own bands are lost
constexpr double kMudLowHz = 200.0, kMudHighHz = 500.0;
constexpr double kMudShare = 0.15;    // a channel with this share of the mix there crowds it
constexpr double kMinResonanceHz = 150.0;
constexpr double kMaxNotchDb = 6.0;      // drums, effects: a ringing shell can take a deep cut ...
constexpr double kMaxTonalNotchDb = 4.0; // ... a tonal part loses its character sooner
constexpr double kMaxTonalResonanceQ = 8.0;
constexpr double kBuriedResonanceDb = -15.0; // this far under the rest in its band, nobody hears it ring
constexpr double kFilteredDb = 24.0;           // overtones this far under the target: filtered on purpose, left alone
constexpr double kThinMarginDb = 3.0;         // overtones this far under the role's target ask for saturation
constexpr double kMinDynamicSpikeDb = 6.0;    // phase 32: a probe this far over its usual share at moments
constexpr double kMinSibilanceSpikeDb = 8.0;  // esses stick out a lot by nature; a dull voice's s does not need a de-esser
constexpr double kMinProbeShareDb = -20.0;    // ... and loud enough against the whole part to be heard
constexpr double kMinRiderSpreadDb = 4.0;     // a vocal whose middle phrases sit this far over its quiet ones
constexpr double kMaxRiderRangeDb = 4.0;
constexpr double kMaxOvertonesDb = -6.0;      // the saturator adds no more than this: past it, it is audible distortion
constexpr double kMaskingStep = 0.05;         // a rise in masking of a more important channel this big counts ...
constexpr double kMaskingFloor = 0.2;
constexpr double kSineSubMarginDb = 12.0;     // a bass this far under its target is a near-sine sub: the enhancer first
constexpr double kMinPlanHarmonicsDb = -24.0, kMaxPlanHarmonicsDb = -6.0; // the enhancer's amount the plan may use
constexpr double kBassSubHz = 120.0, kBassAudibleHz = 150.0; // a bass's sub, and where small speakers begin         // ... once it reaches this (a little under where the collision list starts)

struct SaturationProfile
{
    bool allowed;
    SaturationMode mode;
    double targetDb;           // 2nd-8th harmonics against the fundamental
    double maxDriveDb = 12.0;  // a vocal's pitch read from chops or breaths can be far off, so it stays gentle
};

// Kick and percussion are left to the clipper and the attack tools, effects and buses are
// someone's sound design. Bass gets the tube's even harmonics (an octave up, the sub on small
// speakers), vocals and pads tape (round, warm), leads the tube's bite.
SaturationProfile saturationFor(Role role)
{
    switch (role)
    {
    case Role::bass: return { true, SaturationMode::tube, -24.0 }; // above 150 Hz against the sub
    case Role::vocal: return { true, SaturationMode::tape, -12.0, 6.0 };
    case Role::lead: return { true, SaturationMode::tube, -10.0 };
    case Role::pad: return { true, SaturationMode::tape, -14.0 };
    case Role::other: return { true, SaturationMode::tape, -14.0 };
    case Role::kick:
    case Role::percussion:
    case Role::clap:
    case Role::effects:
    case Role::bus:
    case Role::send: break;
    }
    return { false, SaturationMode::tape, -200.0 };
}

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

} // namespace

double peakBudgetOffsetDb(Role role, bool topPercussion)
{
    if (topPercussion)
        return -8.0; // hats and shakers sit far under the kick
    switch (role)
    {
    case Role::bass:
    case Role::clap: return -1.0;
    case Role::percussion:
    case Role::vocal: return -2.0;
    case Role::send: return -6.0;
    case Role::pad: return -4.0;
    default: break;
    }
    return -3.0;
}

namespace
{
RoleProfile profileFor(Role role)
{
    switch (role)
    {
    case Role::kick: return { 40.0, false, 0.0, 25.0, 1.0 / 16.0, 3.0, 10.0, 3.0, 0.3, 6.0, 0.0 };
    // A sine sub has a 3 dB crest by nature, so a low crest says nothing about a bass's compression.
    case Role::bass: return { 40.0, true, 4.0, 10.0, 1.0 / 16.0, 4.0, 10.0, 2.0, 0.6, 0.0, 6.0 };
    case Role::percussion: return { 500.0, false, 0.0, 15.0, 1.0 / 16.0, 3.0, 13.0, 4.0, 0.3, 7.0, 0.0 };
    // The backbeat keeps its body (a snare's sits at 150-250 Hz): the high-pass stays under it.
    case Role::clap: return { 150.0, false, 0.0, 15.0, 1.0 / 16.0, 3.0, 13.0, 3.0, 0.3, 7.0, 0.0 };
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

struct GlueProfile
{
    double ratio;
    double attackMs;    // slow enough for the hits' first milliseconds to pass
    double releaseNote; // on the groove: back before the next hit
    double reductionDb; // on the loud parts
    double minCrestDb;  // under this the sources arrive squashed
};

// The usual bus glue (an SSL-style bus compressor): drums 4:1 with a 30 ms attack and a 1/16
// release so they pump with the groove, vocals and music 2:1, music slower and lighter.
GlueProfile glueFor(ChainPlan::BusContent content)
{
    switch (content)
    {
    case ChainPlan::BusContent::drums: return { 4.0, 30.0, 1.0 / 16.0, 3.0, 8.0 };
    case ChainPlan::BusContent::vocals: return { 2.0, 10.0, 1.0 / 8.0, 2.0, 6.0 };
    case ChainPlan::BusContent::music: break;
    }
    return { 2.0, 30.0, 1.0 / 4.0, 1.5, 5.0 };
}

// What a bus carries, from the role the Learn pass recognised on it; a drum bus usually
// sounds like a kick or percussion. Unsure: hits standing far over the loud parts are drums.
ChainPlan::BusContent busContentOf(const ChainChannel& c)
{
    if (c.contentConfidence >= 0.5)
    {
        if (c.contentRole == Role::kick || isPercussion(c.contentRole))
            return ChainPlan::BusContent::drums;
        if (c.contentRole == Role::vocal)
            return ChainPlan::BusContent::vocals;
        return ChainPlan::BusContent::music;
    }
    return c.truePeakDb > -100.0 && c.truePeakDb - c.levelP95 >= 14.0 ? ChainPlan::BusContent::drums : ChainPlan::BusContent::music;
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

std::vector<ChainPlan> planChains(const std::vector<ChainChannel>& input, const ChainContext& context)
{
    // A bus carries channels that are in the mix on their own: counting it too would make it
    // collide with its own sources. The steps below see it as absent; it only gets its glue.
    auto channels = input;
    for (auto& c : channels)
        if (c.role == Role::bus)
            c.valid = false;
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

    // Phase 39: the mix against the tonal target in a range, as it would be if every channel had
    // `planned(i)` instead of its present cleanup (so planning again gives the same answer).
    const auto offsetWith = [&](double lowHz, double highHz, const auto& planned) {
        double sum = 0.0;
        int n = 0;
        for (int b = 0; b < kBands; ++b)
        {
            const double hz = centre(b);
            if (hz < lowHz || hz > highHz)
                continue;
            double now = 0.0, then = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                if (!channels[i].valid)
                    continue;
                const double db = channels[i].bandsDb[static_cast<size_t>(b)];
                now += power(db + cleanupResponseDb(channels[i].currentCleanup, hz));
                then += power(db + cleanupResponseDb(planned(i), hz));
            }
            if (now <= 0.0 || then <= 0.0)
                continue;
            sum += context.mixMinusCentreDb[static_cast<size_t>(b)] + 10.0 * std::log10(then / now);
            ++n;
        }
        return n > 0 ? sum / n : 0.0;
    };
    // Without any high-pass the low end still sits under the target: nobody hands it over.
    const bool lowWanted = context.tonalKnown && offsetWith(60.0, 300.0, [](size_t) { return CleanupSettings {}; }) < kLowUnderCentreDb;

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
            // A club system plays down to about 35 Hz; what sits under the lowest note or the
            // kick's body only eats the master's headroom (both real mixes ended 6-12 dB over their
            // masters under 40 Hz with the old 20 Hz floor). From the body, not from the skirt.
            int body = 0;
            while (body < kBands - 1 && c.bandsDb[static_cast<size_t>(body)] < peak - kBodyDb)
                ++body;
            plan.cleanup.hpfHz = static_cast<float>(std::clamp(belowContent(SpectrumAnalyzer::bandEdgeHz(BandScale::erb, body)), kLowEndFloorHz, profile.highPassCapHz));
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
        if (anyOwner && c.role != Role::other && !lowWanted)
        {
            int owned = 0;
            // Never into a drum's own body: a tom under a loud bass still is a tom.
            while (owned < kBands && centre(owned) < profile.highPassCapHz
                   && (!isPercussion(c.role) || c.bandsDb[static_cast<size_t>(owned)] < peak - kBodyDb)
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
        // Phase 39: no mud cut where the mix without one is not over the target's centre.
        const bool mudWanted = !context.tonalKnown || offsetWith(kMudLowHz, kMudHighHz, [&](size_t i) {
                                   auto planned = plans[i].cleanup;
                                   planned.mudDb = 0.0f;
                                   return planned;
                               }) >= kMudOverCentreDb;
        if (crowd.size() >= 3 && mudWanted)
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

    // 2b. Resonances: a narrow cut only where the mix hears the ring (a resonance buried under
    // the rest costs nothing), deeper when it covers a more important channel.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        if (!c.valid || c.role == Role::bus)
            continue;
        const bool tonal = c.role == Role::bass || c.role == Role::lead || c.role == Role::pad || c.role == Role::vocal;
        const double maxDepth = tonal ? kMaxTonalNotchDb : kMaxNotchDb;
        double lowest = std::max(kMinResonanceHz, 1.5 * plan.cleanup.hpfHz);
        if (c.role == Role::bass || c.role == Role::kick || isPercussion(c.role))
            lowest = std::max(lowest, 300.0); // below that the peaks are its notes or a drum's tuned body: its tone
        size_t slot = 0;
        for (const auto& r : c.resonances)
        {
            if (r.hz <= 0.0f || r.hz < lowest || slot == plan.cleanup.notches.size())
                continue;
            // On a tonal part a single-bin line is a held note's harmonic, not a resonance; only a
            // broader, formant-like bump (harshness, boxiness) counts.
            if (tonal && r.q > kMaxTonalResonanceQ)
                continue;
            int band = 0;
            for (int b = 1; b < kBands; ++b)
                if (std::abs(std::log2(centre(b) / r.hz)) < std::abs(std::log2(centre(band) / r.hz)))
                    band = b;
            const auto at = static_cast<size_t>(band);
            const double own = c.bandsDb[at] + cleanupResponseDb(plan.cleanup, r.hz);
            double others = 0.0;
            bool coversImportant = false;
            for (size_t j = 0; j < count; ++j)
            {
                if (j == i || !channels[j].valid)
                    continue;
                const double level = channels[j].bandsDb[at] + cleanupResponseDb(plans[j].cleanup, r.hz);
                others += power(level);
                if (channels[j].priority < c.priority && level > -100.0 && own >= level - 6.0)
                    coversImportant = true;
            }
            if (own - toDb(others) < kBuriedResonanceDb)
            {
                ++plan.resonancesBuried;
                continue;
            }
            double depth = 0.6 * (r.prominenceDb - 3.0) + (coversImportant ? 1.5 : 0.0);
            depth = std::round(std::min(depth, maxDepth) * 2.0) / 2.0;
            if (depth < 1.5)
                continue;
            auto& notch = plan.cleanup.notches[slot];
            notch.hz = static_cast<float>(r.hz >= 1000.0 ? std::round(r.hz / 10.0) * 10.0 : std::round(r.hz));
            notch.db = static_cast<float>(-depth);
            notch.q = static_cast<float>(std::clamp(static_cast<double>(r.q), 3.0, 10.0));
            plan.notchProminenceDb[slot] = r.prominenceDb;
            plan.notchCoversImportant[slot] = coversImportant;
            ++slot;
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
        plan.peakKnown = c.truePeakDb > -100.0;
        plan.crestDb = plan.peakKnown ? c.truePeakDb - c.levelP95 : 0.0;
        plan.maxCrestDb = profile.maxCrestDb;
        plan.percussive = c.role == Role::kick || isPercussion(c.role);
        plan.overCompressed = plan.peakKnown && !c.intentionalDrive && profile.minCrestDb > 0.0 && plan.crestDb < profile.minCrestDb;

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
        // Hats and shakers: their peaks are short and carry little power, so they cost the master
        // no headroom, and clipping them only takes the air off the top.
        plan.topPercussion = isPercussion(c.role) && powerShareAbove(c.bandsDb, kTopHz) >= kTopShare;
        if (plan.topPercussion)
            maxClip = 0.0;
        if (plan.peakKnown && !c.intentionalDrive && !plan.overCompressed && maxClip > 0.0 && excess >= 1.0)
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

    // 3b. Phase 28: a loud master. The limiter's reduction was measured with the clippers the
    // Agents have now, so what they already take beyond the crest rule is added back first: the
    // plan then lands on the same clipping however often it is made. Inside the dead zone
    // (from kLoudMasterAimDb - 1 dB to kLoudMasterLimitDb) what is there stays.
    if (context.limiterReductionDb >= 0.0)
    {
        std::vector<size_t> carriers;
        double applied = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const auto& c = channels[i];
            const auto& plan = plans[i];
            if (!c.valid || !c.carriesMasterPeaks || plan.topPercussion || !plan.dynamicsKnown || !plan.peakKnown || c.intentionalDrive || plan.overCompressed
                || (c.role != Role::kick && !isPercussion(c.role)))
                continue;
            carriers.push_back(i);
            applied = std::max(applied, c.currentClipDb - plan.clipDb);
        }
        const double measured = context.limiterReductionDb;
        double extra = applied;
        if (measured > kLoudMasterLimitDb || measured < kLoudMasterAimDb - 1.0)
            extra = std::max(0.0, applied + measured - kLoudMasterAimDb);
        extra = std::round(extra * 2.0) / 2.0;
        for (const auto i : carriers)
        {
            auto& plan = plans[i];
            const double total = std::min(kMaxLoudMasterClipDb, std::max(plan.clipDb, plan.clipDb + extra));
            plan.loudMasterCapped = plan.clipDb + extra >= kMaxLoudMasterClipDb - 0.25;
            if (total - plan.clipDb < 0.5)
                continue;
            plan.loudMasterClipDb = total - plan.clipDb;
            plan.clipDb = total;
            plan.clipper.on = 1.0f;
            plan.clipper.ceilingDb = static_cast<float>(std::round((channels[i].truePeakDb - total) * 10.0) / 10.0);
            plan.clipper.softness = static_cast<float>(profileFor(channels[i].role).softness);
        }
    }

    // 3c. Peak budget against the kick: nothing stands over the kick's hits. Sized on the Learn pass's true
    // peaks (before every clipper), so planning again lands on the same clipping.
    {
        double kickTop = -200.0;
        for (size_t i = 0; i < count; ++i)
            if (channels[i].valid && channels[i].role == Role::kick && plans[i].peakKnown)
                kickTop = std::max(kickTop, channels[i].truePeakDb - plans[i].clipDb);
        for (size_t i = 0; i < count && kickTop > -100.0; ++i)
        {
            const auto& c = channels[i];
            auto& plan = plans[i];
            if (!c.valid || c.role == Role::kick || c.role == Role::bus || !plan.peakKnown || !plan.dynamicsKnown || c.intentionalDrive
                || plan.overCompressed)
                continue;
            const auto profile = profileFor(c.role);
            const double keepCrest = std::min(profile.maxCrestDb, 14.0);
            const double over = c.truePeakDb - plan.clipDb - (kickTop + peakBudgetOffsetDb(c.role, plan.topPercussion));
            const double room = plan.crestDb - plan.clipDb - keepCrest;
            const double extra = std::floor(std::min({ over, room, kMaxBudgetClipDb - plan.clipDb }) * 2.0) / 2.0;
            if (extra < 1.0)
                continue;
            plan.budgetClipDb = extra;
            plan.clipDb += extra;
            plan.clipper.on = 1.0f;
            plan.clipper.ceilingDb = static_cast<float>(std::round((c.truePeakDb - plan.clipDb) * 10.0) / 10.0);
            plan.clipper.softness = static_cast<float>(profile.softness);
        }
    }

    // 4. Saturation: overtones for parts too thin in them, never at the cost of a more
    // important channel. Judged on the spectra after the planned cleanup, like the mix hears.
    std::vector<MaskingChannel> masking(count);
    for (size_t j = 0; j < count; ++j)
    {
        masking[j].valid = channels[j].valid;
        masking[j].priority = channels[j].priority;
        masking[j].background = channels[j].role == Role::send;
        for (int b = 0; b < kBands; ++b)
            masking[j].rawDb[static_cast<size_t>(b)] = channels[j].bandsDb[static_cast<size_t>(b)] + cleanupResponseDb(plans[j].cleanup, centre(b));
    }
    const auto maskingBefore = analyseMasking(masking);
    auto bandOf = [](double hz) {
        int best = 0;
        for (int b = 1; b < kBands; ++b)
            if (std::abs(std::log(centre(b) / hz)) < std::abs(std::log(centre(best) / hz)))
                best = b;
        return best;
    };
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        const auto profile = saturationFor(c.role);
        if (!c.valid || !profile.allowed || c.intentionalDrive)
            continue;
        const auto& heard = masking[i].rawDb;

        // The fundamental: the usual bass note, else the strongest band above 40 Hz.
        double f0 = c.role == Role::bass && c.lowNoteHz > 0.0 ? c.lowNoteHz : 0.0;
        if (f0 <= 0.0)
        {
            int peak = -1;
            for (int b = 0; b < kBands; ++b)
                if (centre(b) >= 40.0 && (peak < 0 || heard[static_cast<size_t>(b)] > heard[static_cast<size_t>(peak)]))
                    peak = b;
            if (peak < 0 || heard[static_cast<size_t>(peak)] < -150.0)
                continue;
            f0 = centre(peak);
        }
        if (f0 * 2.0 > 16000.0)
            continue;
        // A bass line spans an octave or so, and what matters is how much of it reaches where
        // small speakers start: the sub against what it has above 150 Hz. Elsewhere the region
        // around the fundamental against its 2nd to 8th harmonics.
        const bool bass = c.role == Role::bass;
        const double fundamentalLow = bass ? 20.0 : f0 / 1.25, fundamentalHigh = bass ? kBassSubHz : f0 * 1.25;
        const double overtoneLow = bass ? kBassAudibleHz : f0 * 1.7, overtoneHigh = bass ? 1200.0 : f0 * 8.5;
        double fundamental = 0.0, overtones = 0.0;
        for (int b = 0; b < kBands; ++b)
        {
            const double hz = centre(b);
            const double p = power(heard[static_cast<size_t>(b)]);
            if (hz >= fundamentalLow && hz <= fundamentalHigh)
                fundamental += p;
            else if (hz >= overtoneLow && hz <= overtoneHigh)
                overtones += p;
        }
        if (fundamental <= 0.0)
            fundamental = power(heard[static_cast<size_t>(bandOf(f0))]);
        if (fundamental <= 1e-25)
            continue;

        plan.saturationConsidered = true;
        plan.fundamentalHz = f0;
        plan.overtonesAboveSub = bass;
        plan.overtonesDb = toDb(overtones) - toDb(fundamental);
        plan.overtonesAfterDb = plan.overtonesDb;
        plan.overtonesTargetDb = profile.targetDb;
        if (plan.overtonesDb >= profile.targetDb - kThinMarginDb)
            continue;
        // A natural voice always has overtones: one this bare is a filtered chop, made that way, and
        // saturation would build the top the producer took out (a real mix: +35 dB above 2 kHz).
        // A dull synth may just be dull, and a bass's overtones are what small speakers play.
        if (c.role == Role::vocal && plan.overtonesDb < profile.targetDb - kFilteredDb)
        {
            plan.filteredOnPurpose = true;
            continue;
        }

        // Phase 31: the harmonic enhancer, sized like the saturation: the gentlest amount that reaches
        // the target, stepped down while its overtones would cover a more important channel.
        const auto coveredBy = [&](const SpectrumAnalyzer::Bands& bands) {
            auto trial = masking;
            trial[i].rawDb = bands;
            const auto result = analyseMasking(trial);
            for (size_t j = 0; j < count; ++j)
            {
                if (j == i || !channels[j].valid || channels[j].priority >= c.priority)
                    continue;
                if (result.amount[i][j] - maskingBefore.amount[i][j] >= kMaskingStep && result.amount[i][j] >= kMaskingFloor)
                    return static_cast<int>(j);
            }
            return -1;
        };
        const auto planHarmonics = [&] {
            HarmonicSettings s;
            s.on = 1.0f;
            s.hz = static_cast<float>(std::clamp(std::round(f0 * 1.8 / 5.0) * 5.0, 70.0, kBassSubHz));
            const auto overtonesWith = [&](const SpectrumAnalyzer::Bands& bands) {
                double o = 0.0;
                for (int b = 0; b < kBands; ++b)
                    if (centre(b) >= overtoneLow && centre(b) <= overtoneHigh)
                        o += power(bands[static_cast<size_t>(b)]);
                return toDb(o) - toDb(fundamental);
            };
            double amount = kMinPlanHarmonicsDb;
            for (; amount < kMaxPlanHarmonicsDb; amount += 1.0)
            {
                s.amountDb = static_cast<float>(amount);
                if (overtonesWith(predictHarmonics(heard, s)) >= profile.targetDb)
                    break;
            }
            for (; amount >= kMinPlanHarmonicsDb; amount -= 2.0)
            {
                s.amountDb = static_cast<float>(amount);
                const auto bands = predictHarmonics(heard, s);
                const int covers = coveredBy(bands);
                if (covers < 0)
                {
                    plan.harmonics = s;
                    plan.harmonicsAfterDb = overtonesWith(bands);
                    return true;
                }
                plan.harmonicsWouldCover = covers;
            }
            return false;
        };
        plan.sineSub = bass && plan.overtonesDb < profile.targetDb - kSineSubMarginDb;
        if (plan.sineSub && planHarmonics())
        {
            plan.overtonesAfterDb = plan.harmonicsAfterDb;
            continue;
        }

        // Candidates from the gentlest up, as the real saturator measures them at the part's
        // loud level (after gain staging, which is where the drive is defined).
        const double level = c.hasStats && c.levelP95 > -100.0 ? c.levelP95 : -18.0;
        struct Candidate
        {
            SaturationSettings settings;
            SaturationPrediction prediction;
        };
        std::vector<Candidate> candidates;
        for (float mix : { 0.25f, 0.4f, 0.55f })
            for (float drive : { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f })
            {
                if (drive > profile.maxDriveDb)
                    continue;
                SaturationSettings s;
                s.on = 1.0f;
                s.mode = static_cast<float>(profile.mode);
                s.driveDb = drive;
                s.mix = mix;
                const auto p = predictSaturation(s, f0, level);
                if (p.overtonesDb <= kMaxOvertonesDb)
                    candidates.push_back({ s, p });
            }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.prediction.overtonesDb < b.prediction.overtonesDb; });

        auto withSaturation = [&](const SaturationPrediction& p) {
            auto bands = heard;
            for (int k = 2; k <= 8; ++k)
            {
                const double h = p.harmonicDb[static_cast<size_t>(k - 2)];
                if (h < -150.0 || k * f0 > 20000.0)
                    continue;
                auto& band = bands[static_cast<size_t>(bandOf(k * f0))];
                band = toDb(power(band) + fundamental * power(h));
            }
            return bands;
        };
        auto after = [&](const SaturationPrediction& p) {
            double added = 0.0;
            for (int k = 2; k <= 8; ++k)
                if (k * f0 >= overtoneLow && k * f0 <= overtoneHigh)
                    added += power(p.harmonicDb[static_cast<size_t>(k - 2)]);
            return toDb(overtones + fundamental * added) - toDb(fundamental);
        };

        // The gentlest that reaches the target, else the strongest allowed; then step down
        // while its overtones would cover a more important channel.
        int pick = -1;
        for (size_t k = 0; k < candidates.size() && pick < 0; ++k)
            if (after(candidates[k].prediction) >= profile.targetDb)
                pick = static_cast<int>(k);
        if (pick < 0)
            pick = static_cast<int>(candidates.size()) - 1;
        for (; pick >= 0; --pick)
        {
            auto trial = masking;
            trial[i].rawDb = withSaturation(candidates[static_cast<size_t>(pick)].prediction);
            const auto result = analyseMasking(trial);
            int covers = -1;
            for (size_t j = 0; j < count && covers < 0; ++j)
            {
                if (j == i || !channels[j].valid || channels[j].priority >= c.priority)
                    continue;
                const double was = maskingBefore.amount[i][j], will = result.amount[i][j];
                if (will - was >= kMaskingStep && will >= kMaskingFloor)
                    covers = static_cast<int>(j);
            }
            if (covers < 0)
                break;
            plan.saturationWouldCover = covers;
        }
        if (pick < 0)
        {
            // Phase 31: a bass the saturation would make cover someone may still take the cleaner enhancer.
            if (bass && !plan.sineSub && planHarmonics())
                plan.overtonesAfterDb = plan.harmonicsAfterDb;
            continue;
        }
        const auto& chosen = candidates[static_cast<size_t>(pick)];
        plan.saturation = chosen.settings;
        plan.overtonesAfterDb = after(chosen.prediction);
    }

    // 5. Glue on buses.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = input[i];
        auto& plan = plans[i];
        if (!c.valid || c.role != Role::bus || !c.hasStats || c.levelP95 < -100.0)
            continue;
        plan.dynamicsKnown = true;
        plan.spreadDb = c.levelP95 - c.levelP10;
        plan.spreadAfterDb = plan.spreadDb;
        plan.peakKnown = c.truePeakDb > -100.0;
        plan.crestDb = plan.peakKnown ? c.truePeakDb - c.levelP95 : 0.0;
        plan.busContent = busContentOf(c);
        const auto glue = glueFor(plan.busContent);
        plan.glueTargetDb = glue.reductionDb;
        // Already squashed before the bus (a limiter on the sources): glue would only flatten it.
        plan.overCompressed = plan.peakKnown && !c.intentionalDrive && plan.crestDb < glue.minCrestDb;
        if (plan.overCompressed || c.intentionalDrive)
            continue;
        auto& comp = plan.compressor;
        comp.on = 1.0f;
        comp.ratio = static_cast<float>(glue.ratio);
        comp.kneeDb = 6.0f;
        comp.attackMs = static_cast<float>(glue.attackMs);
        comp.releaseMs = static_cast<float>(std::clamp(syncedReleaseMs(bpm, glue.releaseNote), 50.0, 600.0));
        comp.thresholdDb = static_cast<float>(std::round(c.levelP95 * 2.0) / 2.0);
        while (compressorReductionDb(comp, c.levelP95) < glue.reductionDb && comp.thresholdDb > std::max(c.levelP95 - 30.0, -60.0))
            comp.thresholdDb -= 0.5f;
        comp.makeupDb = static_cast<float>(std::round(compressorReductionDb(comp, c.levelP50) * 10.0) / 10.0);
        plan.glue = true;
        plan.glueReductionDb = compressorReductionDb(comp, c.levelP95);
        plan.spreadAfterDb = c.levelP95 - plan.glueReductionDb - (c.levelP10 - compressorReductionDb(comp, c.levelP10));
    }

    // 6. Phase 32: problems that come and go. A probe range that sticks out of the part at moments
    // (P95 well over P50) gets a dynamic band that cuts only then, where the mix hears that range;
    // deeper where it covers a more important channel. A static cut would dull every other moment.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        auto& plan = plans[i];
        if (!c.valid || c.intentionalDrive || !c.hasStats || !c.probesValid)
            continue;
        const bool vocal = c.role == Role::vocal;
        const bool tonal = vocal || c.role == Role::lead || c.role == Role::pad || c.role == Role::other;
        if (!tonal)
            continue;
        plan.dynamicConsidered = true;
        for (int p = 0; p < kDynamicBands; ++p)
        {
            const auto probe = static_cast<DynamicProbe>(p);
            // Sibilance and proximity boom are a voice's; harshness any tonal part's.
            if (probe != DynamicProbe::harsh && !vocal)
                continue;
            const auto at = static_cast<size_t>(p);
            const double spike = c.probeP95[at] - c.probeP50[at];
            plan.probeSpikeDb[at] = spike;
            const double minSpike = probe == DynamicProbe::sibilance ? kMinSibilanceSpikeDb : kMinDynamicSpikeDb;
            if (spike < minSpike || c.probeP95[at] < kMinProbeShareDb)
                continue;
            const double hz = dynamicProbeHz(p);
            int band = 0;
            for (int b = 1; b < kBands; ++b)
                if (std::abs(std::log2(centre(b) / hz)) < std::abs(std::log2(centre(band) / hz)))
                    band = b;
            const auto bandAt = static_cast<size_t>(band);
            const double own = c.bandsDb[bandAt] + cleanupResponseDb(plan.cleanup, hz);
            double others = 0.0;
            bool coversImportant = false;
            for (size_t j = 0; j < count; ++j)
            {
                if (j == i || !channels[j].valid)
                    continue;
                const double level = channels[j].bandsDb[bandAt] + cleanupResponseDb(plans[j].cleanup, hz);
                others += power(level);
                if (channels[j].priority < c.priority && level > -100.0 && own >= level - 6.0)
                    coversImportant = true;
            }
            // The moments it sticks out are spike dB over its usual level there.
            if (own + spike - toDb(others) < kBuriedResonanceDb)
            {
                plan.probeBuried[at] = true;
                continue;
            }
            plan.probeCoversImportant[at] = coversImportant;
            auto& d = plan.dynamicEq.bands[at];
            d = dynamicProbeBand(p);
            d.on = 1.0f;
            d.thresholdDb = static_cast<float>(std::round(std::max(c.probeP50[at] + 3.0, c.probeP95[at] - 5.0) * 2.0) / 2.0);
            const double maxRange = probe == DynamicProbe::sibilance ? 6.0 : 4.0;
            d.rangeDb = static_cast<float>(std::round(std::min(maxRange, spike / 2.0 + (coversImportant ? 1.5 : 0.0)) * 2.0) / 2.0);
        }
        // The rider: a vocal whose phrases sit at different levels is evened before the compressor,
        // which then only holds the syllables. Quiet bars on purpose (P50 - P10 beyond 12 dB) are
        // the arrangement's, not the rider's: it only takes the first few dB.
        if (vocal && c.levelP50 > -100.0 && c.levelP10 > -100.0)
        {
            plan.riderSpreadDb = c.levelP50 - c.levelP10;
            if (plan.riderSpreadDb >= kMinRiderSpreadDb)
            {
                auto& r = plan.dynamicEq.rider;
                r.on = 1.0f;
                r.targetDb = static_cast<float>(std::round(c.levelP50 * 2.0) / 2.0);
                r.rangeDb = static_cast<float>(std::round(std::min(kMaxRiderRangeDb, plan.riderSpreadDb / 2.0) * 2.0) / 2.0);
                r.speedMs = 250.0f;
            }
        }
    }
    // 7. Phase 34: depth. A send (reverb, delay) brighter than the mix less kDarkerThanMixDb gets a high
    // cut: the dry parts keep the top, the room moves behind them.
    for (size_t i = 0; i < count; ++i)
    {
        const auto& c = channels[i];
        if (!c.valid || c.role != Role::send)
            continue;
        SpectrumAnalyzer::Bands send {}, rest {};
        std::array<double, kBands> restPower {};
        for (size_t j = 0; j < count; ++j)
        {
            if (j == i || !channels[j].valid || channels[j].role == Role::bus)
                continue;
            for (int b = 0; b < kBands; ++b)
                restPower[static_cast<size_t>(b)] += power(channels[j].bandsDb[static_cast<size_t>(b)] + cleanupResponseDb(plans[j].cleanup, centre(b)));
        }
        bool restPlays = false;
        for (int b = 0; b < kBands; ++b)
        {
            send[static_cast<size_t>(b)] = c.bandsDb[static_cast<size_t>(b)] + cleanupResponseDb(plans[i].cleanup, centre(b));
            rest[static_cast<size_t>(b)] = toDb(restPower[static_cast<size_t>(b)]);
            restPlays = restPlays || restPower[static_cast<size_t>(b)] > 0.0;
        }
        if (!restPlays)
            continue;
        auto& plan = plans[i];
        plan.depthConsidered = true;
        plan.sendTiltDb = topTiltDb(send);
        plan.mixTiltDb = topTiltDb(rest);
        if (plan.sendTiltDb > plan.mixTiltDb - kDarkerThanMixDb)
            plan.depth.highCutHz = static_cast<float>(kSendHighCutHz);
    }

    return plans;
}

} // namespace ma

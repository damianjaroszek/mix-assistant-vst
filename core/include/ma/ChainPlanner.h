#pragma once

#include "ChannelChain.h"
#include "Depth.h"
#include "DynamicEq.h"
#include "Harmonics.h"
#include "Instrument.h"
#include "Resonance.h"
#include "Saturator.h"
#include "SpectrumAnalyzer.h"

#include <vector>

namespace ma
{

// The mix engineer's pass over every channel at once (phase 7): high-pass, mono low end,
// low-mid mud, compression, peak clipping, bass-note evening and (phase 13) saturation for
// parts too thin in overtones, each decided from the
// channel's own measurements AND its place in the mix. A pad loses its low end because
// the kick and bass own it there, a vocal is compressed harder when its quiet words sink
// under the rest, the low-mid cut goes to whoever crowds that region least importantly.
//
// Starting values per role are the usual house / trance practice; the measurements decide
// whether a tool is needed at all and how hard it works.
struct ChainChannel
{
    Role role = Role::other;
    int priority = 5;
    SpectrumAnalyzer::Bands bandsDb {}; // raw ERB levels after the trim, before cleanup and EQ
    bool valid = false;
    // From the Learn pass, after the trim (dBFS). Without them only cleanup is planned.
    bool hasStats = false;
    double levelP10 = -200.0, levelP50 = -200.0, levelP95 = -200.0; // 100 ms RMS while playing
    double truePeakDb = -200.0;
    double lowNoteHz = 0.0;
    bool intentionalDrive = false; // meant to distort: no clipper, no "over-compressed" verdict
    Resonances resonances {};      // from the Learn pass
    // A bus: what the Learn pass recognised on it, i.e. what its sources are (drums, vocals...).
    // Buses are left out of every sum over the mix, since their sources are channels of their own.
    Role contentRole = Role::other;
    double contentConfidence = 0.0;
    // Phase 28: a kick or percussion part whose hits are the master's highest peaks, and how
    // far its clipper (as the Agent has it now) takes them down: what a loud master needs.
    bool carriesMasterPeaks = false;
    double currentClipDb = 0.0;
    // Phase 32: the Learn pass's probe ranges (DynamicProbe order) against the whole part, while it
    // plays: the usual share (P50) and the moments it sticks out (P95), dB.
    bool probesValid = false;
    std::array<double, kDynamicBands> probeP50 {}, probeP95 {};
    // Phase 39: the cleanup the Agent has now (what the measured master already carries).
    CleanupSettings currentCleanup {};
};

// Phase 28: a loud master. When the limiter of the master preview works too hard at the target
// loudness (it flattens the hits), the percussion carrying the peaks is clipped further, up to
// kMaxLoudMasterClipDb in all, aiming the limiter at kLoudMasterAimDb on the loud part.
constexpr double kLoudMasterLimitDb = 3.0;   // above this the limiter costs the hits their punch
constexpr double kLoudMasterAimDb = 2.5;
constexpr double kMaxLoudMasterClipDb = 6.0; // a clipper on a kick or percussion, in all

// Peak budget against the kick. In a club mix the kick sets the peak of the sum; a part whose peaks stand over
// the kick's (a sparse spiky track, a bass with a hard attack) costs the master its loudness and gives nothing
// back. Such a part is clipped down to the kick's peak plus its role's offset, never further than its own
// crest allows (the clipper stays over the loud parts' level by the role's usual crest) and never by more than
// kMaxBudgetClipDb in all.
constexpr double kMaxBudgetClipDb = 12.0;
double peakBudgetOffsetDb(Role role, bool topPercussion);

struct ChainContext
{
    double bpm = 0.0;            // releases follow the tempo when known
    double headroomShortDb = 0.0; // the mix's peak over the pre-master ceiling: clip percussion a little more
    // Phase 28: the master preview's limiter on the loud part (dB, 95th percentile), measured with
    // the clippers the Agents have now; negative = not measured.
    double limiterReductionDb = -1.0;
    // Phase 39: per ERB band, the mix minus the tonal target's corridor centre as the master plays
    // now (with every Agent's present cleanup); without a target comparison `tonalKnown` is false.
    // Where the target wants more, the low-mid cut and the "low end owned" high-pass stay off.
    bool tonalKnown = false;
    SpectrumAnalyzer::Bands mixMinusCentreDb {};
};

// Phase 39: the mix may be this far over the target's centre in 200-500 Hz (without any mud cut)
// before a mud cut is planned; under this far below it in 60-300 Hz (without the cleanup's high-passes)
// the high-pass stays at the junk under each part instead of handing the low end to the kick and bass.
constexpr double kMudOverCentreDb = 0.5;
constexpr double kLowUnderCentreDb = -1.0;

struct ChainPlan
{
    CleanupSettings cleanup {};
    CompressorSettings compressor {};
    ClipperSettings clipper {};
    BassEvenerSettings evener {};

    // Why, for the per-channel verdict.
    enum class HighPass { none, junk, lowEndOwned, subsonic };
    HighPass highPassReason = HighPass::none;
    int mudCrowd = 0;               // channels crowding 200-500 Hz when a mud cut was set
    std::array<double, CleanupSettings::kNotches> notchProminenceDb {}; // per resonance cut
    std::array<bool, CleanupSettings::kNotches> notchCoversImportant {};
    int resonancesBuried = 0;       // found, but the mix covers them: left alone
    bool dynamicsKnown = false;
    double spreadDb = 0.0;          // loud minus quiet parts (P95 - P10)
    double targetSpreadDb = 0.0;
    double spreadAfterDb = 0.0;     // predicted with the planned compressor
    double arrangementDb = 0.0;     // spread left on purpose (quiet bars, breakdowns): volume automation, not compression
    bool lostInQuietParts = false;  // its quiet parts sink under the rest of the mix
    bool percussive = false;        // compression left to the clipper: hits are kept
    bool topPercussion = false;     // hats, shakers: never clipped (their peaks cost the master nothing)
    bool overCompressed = false;    // already squashed before the Agent: nothing more
    double crestDb = 0.0;           // true peak over the loud parts' RMS
    bool peakKnown = false;         // phase 23: the learn pass's true peak is there; without it crest and clipping are unknown
    double maxCrestDb = 0.0;
    double clipDb = 0.0;
    // Peak budget: the part of clipDb that keeps this part's peaks under the kick's (beyond the crest rule).
    double budgetClipDb = 0.0;
    // Phase 28: the part of clipDb that is there for a loud master (beyond the crest rule).
    double loudMasterClipDb = 0.0;
    bool loudMasterCapped = false;  // at kMaxLoudMasterClipDb: more has to come from elsewhere

    // Phase 13: saturation where a tonal part is too thin in overtones (a sine-like sub, a
    // dull pad) to be heard and felt in the mix; the shaper and how hard it works are chosen
    // with the real saturator, the gentlest setting that gets there, and never so its new
    // overtones would cover a more important channel.
    SaturationSettings saturation {}; // off unless planned
    bool saturationConsidered = false; // a role that may take it, with a measurable fundamental
    bool filteredOnPurpose = false;    // a voice with overtones far under the target: no saturation
    double fundamentalHz = 0.0;
    bool overtonesAboveSub = false;    // bass: what it has above 150 Hz against its sub, not harmonics of one note
    double overtonesDb = -200.0;       // 2nd-8th harmonic region against the fundamental, now
    double overtonesTargetDb = -200.0; // what the role should have
    double overtonesAfterDb = -200.0;  // predicted with the planned saturation
    int saturationWouldCover = -1;     // the more important channel that kept it gentler or off (index)

    // Phase 31: the harmonic enhancer (overtones made from the sub only, the fundamental and the rest
    // of the part stay clean) for a bass that is close to a pure sine down there; saturation for one
    // that has some body already. Each is the other's fallback when it would cover a more important
    // channel.
    HarmonicSettings harmonics {};     // off unless planned
    bool sineSub = false;              // the bass is that thin: the enhancer comes first
    double harmonicsAfterDb = -200.0;  // overtones predicted with the planned enhancer
    int harmonicsWouldCover = -1;      // the more important channel that kept it gentler or off (index)

    // Phase 32: dynamic bands (boom, harshness, sibilance) where a range sticks out only at moments,
    // and the rider on a vocal whose phrases sit at different levels.
    DynamicEqSettings dynamicEq {};
    std::array<double, kDynamicBands> probeSpikeDb {};   // P95 - P50 of each probe (0 = not measured)
    std::array<bool, kDynamicBands> probeBuried {};      // sticks out, but the mix covers that range
    std::array<bool, kDynamicBands> probeCoversImportant {};
    bool dynamicConsidered = false;                       // a role that may take them, with probe stats
    double riderSpreadDb = 0.0;                           // P50 - P10 when the rider was considered

    // Phase 34: a reverb or delay return darker than the mix sits behind it; one as bright as the
    // dry parts stands next to them and blurs them, so its top is cut.
    DepthSettings depth {};
    bool depthConsidered = false; // a send with a spectrum
    double sendTiltDb = 0.0, mixTiltDb = 0.0; // topTiltDb of the send and of the rest of the mix

    // Phase 15: glue compression on a bus. A gentle ratio and a slow attack that lets the hits
    // through, the release on the groove, a couple of dB on the loud parts: the sources move as
    // one part. How hard depends on what the bus carries.
    enum class BusContent { drums, vocals, music };
    bool glue = false;
    BusContent busContent = BusContent::music;
    double glueReductionDb = 0.0; // planned on the loud parts
    double glueTargetDb = 0.0;
};

std::vector<ChainPlan> planChains(const std::vector<ChainChannel>& channels, const ChainContext& context = {});

} // namespace ma

#pragma once

#include "ChannelChain.h"
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
};

struct ChainContext
{
    double bpm = 0.0;            // releases follow the tempo when known
    double headroomShortDb = 0.0; // the mix's peak over the pre-master ceiling: clip percussion a little more
};

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
    bool overCompressed = false;    // already squashed before the Agent: nothing more
    double crestDb = 0.0;           // true peak over the loud parts' RMS
    double maxCrestDb = 0.0;
    double clipDb = 0.0;

    // Phase 13: saturation where a tonal part is too thin in overtones (a sine-like sub, a
    // dull pad) to be heard and felt in the mix; the shaper and how hard it works are chosen
    // with the real saturator, the gentlest setting that gets there, and never so its new
    // overtones would cover a more important channel.
    SaturationSettings saturation {}; // off unless planned
    bool saturationConsidered = false; // a role that may take it, with a measurable fundamental
    double fundamentalHz = 0.0;
    bool overtonesAboveSub = false;    // bass: what it has above 150 Hz against its sub, not harmonics of one note
    double overtonesDb = -200.0;       // 2nd-8th harmonic region against the fundamental, now
    double overtonesTargetDb = -200.0; // what the role should have
    double overtonesAfterDb = -200.0;  // predicted with the planned saturation
    int saturationWouldCover = -1;     // the more important channel that kept it gentler or off (index)

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

#pragma once

#include "Instrument.h"
#include "Stereo.h"

#include <vector>

namespace ma
{

// Phase 8, the owner's own method from mastering with a per-band correlation meter, done
// per channel instead of on the sum: not "make it wide" but keep every band in phase. Where
// a band falls into anti-phase, find the channel doing it and take side off there, on that
// channel only. Where the mix is narrow and a wide part has some side to give, lift it a
// little. The low end is mono anyway (cleanup).
struct StereoChannel
{
    Role role = Role::other;
    int priority = 5;
    bool valid = false;
    SpectrumAnalyzer::Bands midDb {}, sideDb {}; // ERB, after the trim, before the stereo EQ
};

// The measured sum, for the report and the view; planning works from the channels.
struct StereoMaster
{
    bool valid = false;
    SpectrumAnalyzer::Bands midDb {}, sideDb {};
};

struct StereoPlan
{
    StereoSettings settings {};
    struct Fix
    {
        double lowHz = 0.0, highHz = 0.0;
        double coherenceBefore = 0.0, coherenceAfter = 0.0; // the channel's own, worst band
        double sumBefore = 1.0, sumAfter = 1.0;              // the mix in the same bands (cuts only)
        double gainDb = 0.0;
        bool sumOutOfPhase = false; // the mix itself is in anti-phase there
        bool lift = false;          // side added, where the mix is narrow
    };
    std::vector<Fix> fixes;
};

std::vector<StereoPlan> planStereo(const std::vector<StereoChannel>& channels, const StereoMaster& master);

// Bands of the sum in anti-phase (coherence under 0), as frequency ranges, for the report.
struct PhaseRange
{
    double lowHz = 0.0, highHz = 0.0, worst = 1.0;
};
std::vector<PhaseRange> antiPhaseRanges(const StereoMaster& master);

} // namespace ma

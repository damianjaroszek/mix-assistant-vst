#pragma once

#include "Instrument.h"
#include "StemReference.h"
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
    int group = -1; // phase 25: its stem group (stemGroupForRole), -1 = none
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
        // Phase 25: widening towards the references (a side lift or the widener), and how wide
        // the channel's group is there in the references (coherence).
        bool toReference = false;
        bool widener = false;
        double referenceCoherence = 1.0;
    };
    std::vector<Fix> fixes;
};

// Phase 25: how wide each stem group plays in the references, per stem region above the low end
// (coherence of the region's mid and side power, median over the profiles that have the group).
struct WidthTarget
{
    bool valid = false;
    std::array<std::array<double, kStemRegions>, kStemGroups> coherence {};
    std::array<std::array<bool, kStemRegions>, kStemGroups> known {};
};
WidthTarget widthTargetFrom(const StemTarget& target);

// Phase 25: below this the mix stays as it is (mono below the cleanup's 120 Hz, the low mids' body);
// no band of a channel or of the sum is ever widened under kSafeCoherence.
constexpr double kWidenLowestHz = 300.0;
constexpr double kSafeCoherence = 0.2;

// With a width target, the small "narrow mix" lift gives way to widening every part to its group's
// width in the references: kick, bass and lead vocals stay in the centre (a vocal only gets the
// side it already has lifted), a part with side gets side bells, a mono one the widener.
std::vector<StereoPlan> planStereo(const std::vector<StereoChannel>& channels, const StereoMaster& master,
                                   const WidthTarget* width = nullptr);

// Bands of the sum in anti-phase (coherence under 0), as frequency ranges, for the report.
struct PhaseRange
{
    double lowHz = 0.0, highHz = 0.0, worst = 1.0;
};
std::vector<PhaseRange> antiPhaseRanges(const StereoMaster& master);

} // namespace ma

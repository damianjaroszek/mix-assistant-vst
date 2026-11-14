#pragma once

#include "TonalBalance.h"

#include <array>

namespace ma
{

// "Kontrola mastera": the mix as the Brain measures it before the mastering chain against what a
// plugin after the chain measures, both since the same reset. What mastering added in loudness, what
// it took off the peaks, how it moved the tone (per ERB band and per Tonal Balance Control range) and
// what came out: the facts behind "what stops this track from going louder" and "fix it in the mix".
struct MasterSide
{
    bool valid = false;
    double seconds = 0.0;
    double integratedLufs = -200.0, maxShortTermLufs = -200.0, truePeakDb = -200.0;
    Curve bandsDb {};
    double sideDb = -200.0, lowSideDb = -200.0; // side minus mid from 150 Hz up / below 120 Hz; after only
};

constexpr double kMasterMinimumSeconds = 8.0;
constexpr double kMasterToneChangeDb = 2.0;  // a range mastering moves by this much belongs in the mix
constexpr double kMasterTiltCapDb = 3.0;     // most of the measured tilt the tonal comparison takes over
constexpr double kMasterPeakLimitDb = -0.1;  // true peak over this after mastering clips converters and codecs
constexpr double kMasterLowSideDb = -12.0;   // side this close to the mid below 120 Hz: the low end is not mono

struct MasterCheck
{
    bool valid = false;
    double gainDb = 0.0;                  // loudness mastering added
    double plrBeforeDb = 0.0, plrAfterDb = 0.0; // true peak minus integrated loudness
    double densityDb = 0.0;               // what it took off the peaks (PLR before minus after)
    Curve tiltDb {};                      // after minus before, shapes only (mean over 60 Hz-8 kHz removed)
    std::array<double, kTonalZones> zoneChangeDb {};
    int largestZone = -1;                 // the range moved most, if by kMasterToneChangeDb or more
    bool overs = false, lowSideWide = false;
};

MasterCheck compareMaster(const MasterSide& before, const MasterSide& after);
// The tilt as the tonal comparison uses it: capped at +-kMasterTiltCapDb.
Curve cappedMasterTilt(const MasterCheck& check);

} // namespace ma

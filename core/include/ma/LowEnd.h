#pragma once

#include "Ducker.h"
#include "TonalBalance.h"

#include <array>
#include <vector>

namespace ma
{

// The low end judged as one thing. Four parts make a club low end work together, and the plugin
// measured each of them separately before: how the sub sits under the bass body, whether the line's
// notes are even for the ear, whether the bass comes back from the kick's duck inside the groove,
// and whether there is room over the bass (150-400 Hz) for it to stand out. Each is compared with
// the reference masters' own low end (the active tonal target's corridor centre).
constexpr double kSubTopHz = 60.0;    // sub below this
constexpr double kBassTopHz = 150.0;  // the bass body up to here
constexpr double kLowMidTopHz = 400.0; // the room over the bass

enum class LowPart
{
    subVsBody = 0, // sub against the bass body, against the references
    notes,         // how even the bass line's notes are for the ear
    duckReturn,    // how the bass comes back after the kick, against the grid
    space,         // 150-400 Hz against the references
};
constexpr int kLowParts = 4;

enum class LowStatus
{
    unknown = 0, // not measured
    good,
    warning,
    problem,
};

struct LowFinding
{
    LowStatus status = LowStatus::unknown;
    double measured = 0.0; // dB, or ms for duckReturn
    double target = 0.0;   // what the references (or the grid) say
    double delta = 0.0;    // measured - target
    int channel = -1;      // the channel the finding is about, -1 = the mix
    double extra = 0.0;    // duckReturn: the beat in ms; notes: the spread as measured, before the evener
};

struct LowEndInput
{
    bool tonalValid = false;
    Curve mixDb {};    // the mix, normalised and aligned to the target (TonalComparison::mixDb)
    Curve targetDb {}; // the corridor's centre
    double bpm = 0.0;

    struct Channel
    {
        bool bass = false; // role bass (sub included)
        double noteSpreadDb = 0.0;
        bool notesKnown = false;
        // What the Agent's note evener already takes off that spread (0 = off). The spread is measured
        // on the Agent's input, so without this the finding would never clear after it was fixed.
        double evenerAmount = 0.0;
        DuckSettings duck {};
        bool duckKeyed = false; // ducking without a cable counts as ducking
        double levelDb = -200.0; // how loud it plays, to pick the main bass
    };
    std::vector<Channel> channels;
};

struct LowEndReport
{
    std::array<LowFinding, kLowParts> parts {};
    bool valid = false;
    LowStatus worst() const;
};

// Power sum of a curve over [lowHz, highHz), in dB; -200 when it has no band there.
double curveRegionDb(const Curve& curve, double lowHz, double highHz);
// How long the duck takes to let about 90% of the level back (ms); 0 when it does not duck.
double duckRecoveryMs(const DuckSettings& duck);

LowEndReport analyseLowEnd(const LowEndInput& input);

// The thresholds, so the texts can name them.
constexpr double kSubBalanceWarnDb = 2.0, kSubBalanceProblemDb = 4.0;
constexpr double kNoteSpreadWarnDb = 3.0, kNoteSpreadProblemDb = 6.0;
constexpr double kSpaceWarnDb = 1.5, kSpaceProblemDb = 3.0;

} // namespace ma

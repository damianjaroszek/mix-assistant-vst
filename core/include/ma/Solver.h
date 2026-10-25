#pragma once

#include "ChannelEq.h"
#include "Masking.h"

#include <vector>

namespace ma
{

// Proposes corrections for the whole mix at once.
//
// Works from each channel's spectrum before the Brain's EQ (raw ERB levels after the
// trim), so solving again gives the same answer instead of stacking cuts. For every
// collision where a less important channel covers a more important one, it places a
// bell cut on the masker over the masked range, then checks the result with the masking
// model and deepens the cut while that still helps. Gain is only touched to restore the
// pre-master headroom, equally on every channel so the balance stays.
struct SolverChannel
{
    SpectrumAnalyzer::Bands rawDb {}; // ERB bands, dBFS, after trim, before the Brain's EQ
    int priority = 5;
    bool valid = false;
};

struct SolverOptions
{
    double maxCutDb = 6.0;        // deepest cut the solver may set on one band
    double minimumScore = 0.3;    // collisions weaker than this are left alone
    double headroomGainDb = 0.0;  // <= 0: gain for every channel to get back under the ceiling
};

struct SolverChange
{
    enum class Kind
    {
        eqCut,
        globalGain
    };

    Kind kind = Kind::eqCut;
    int channel = -1;       // eqCut: the channel that gets the cut
    int eqSlot = -1;        // eqCut: which EQ band of that channel
    int maskedChannel = -1; // eqCut: who it makes room for
    double lowHz = 0.0, highHz = 0.0;
    double scoreBefore = 0.0, scoreAfter = 0.0;
    double gainDb = 0.0;    // globalGain
};

struct SolverResult
{
    std::vector<EqSettings> eq; // per channel, the complete proposed EQ
    std::vector<SolverChange> changes;
    double crowdingBefore = 0.0; // sum of collision scores that count, before and after
    double crowdingAfter = 0.0;
};

SolverResult solve(const std::vector<SolverChannel>& channels, const SolverOptions& options = {});

} // namespace ma

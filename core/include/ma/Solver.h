#pragma once

#include "ChannelEq.h"
#include "Masking.h"
#include "StemReference.h"
#include "TonalBalance.h"

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
//
// With a tonal target it then works on the mix's holes and excesses: a hole is filled
// by boosting the most important channel that already has content there, as long as
// that does not create a new collision; an excess is cut on the least important channel
// that carries it. When no channel has anything in a hole, it says so and names the
// channel whose harmonics could fill it (saturation) instead of boosting noise.
struct SolverChannel
{
    SpectrumAnalyzer::Bands rawDb {}; // ERB bands, dBFS, after trim, before the Brain's EQ
    int priority = 5;
    bool valid = false;
    // Phase 17: a part the arrangement does not have yet (sketch mode). It counts in collisions
    // and the tonal balance, so the mix leaves it room, but never gets a change itself.
    bool ghost = false;
};

struct SolverOptions
{
    double maxCutDb = 6.0;        // deepest cut the solver may set on one band
    double minimumScore = 0.3;    // collisions weaker than this are left alone
    double headroomGainDb = 0.0;  // <= 0: gain for every channel to get back under the ceiling
    double maxBoostDb = 4.0;      // highest boost for filling a tonal hole
    const TonalComparison* tonal = nullptr; // the measured mix against a target, if there is one
    // Phase 9: each group's shape (drums, bass, vocals, the rest) against stem references,
    // measured over the drop. Broad bells on the group's channel that carries the region.
    const StemShapeInput* stems = nullptr;
    double maxStemBoostDb = 3.0;
    double maxStemCutDb = 4.0;
    double maxGhostCutDb = 3.0; // deepest cut that makes room for a ghost
};

struct SolverChange
{
    enum class Kind
    {
        eqCut,       // makes room for a more important channel
        globalGain,  // restores headroom
        tonalBoost,  // fills a hole using content the channel already has
        tonalCut,    // takes out an excess
        tonalAdvice, // a hole nothing in the mix can fill: needs saturation or a new sound
        stemBoost,   // a group has less in a region than in the references
        stemCut      // ... or more
    };

    Kind kind = Kind::eqCut;
    int channel = -1;       // eqCut: the channel that gets the cut
    int eqSlot = -1;        // eqCut: which EQ band of that channel
    int maskedChannel = -1; // eqCut: who it makes room for
    int sourceChannel = -1; // tonalAdvice: channel whose harmonics could reach the hole, or -1
    double deviationDb = 0.0; // tonal*: how far the region sits outside the corridor
    double lowHz = 0.0, highHz = 0.0;
    double scoreBefore = 0.0, scoreAfter = 0.0;
    double gainDb = 0.0;    // globalGain, tonalBoost, tonalCut, stem*
    int group = -1;         // stem*: which StemGroup
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

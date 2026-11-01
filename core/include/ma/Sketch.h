#pragma once

#include "SpectrumAnalyzer.h"
#include "StemReference.h"
#include "TonalBalance.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 17: sketch mode. A track in progress (kick, bass, a lead) measured against finished
// references shows holes that are not the mix's: nothing that usually plays there exists yet.
// EQ cannot fix those, and a tonal curve normalised to a sparse mix makes the parts that are
// there look too loud. So the parts a finished track of the style has and this one lacks are
// stood in by "ghosts": their spectrum and level taken from the references' stems (or a typical
// house profile without them), placed against the groups that do play. The ghosts count in the
// tonal comparison and in collisions (the solver leaves them room), never get any change, and
// tell what to add: a brief of level, range, width and the sections it plays in.

// One group of a finished track, over its drop.
struct ArrangementGroup
{
    bool expected = false; // a finished track of the style has it (most references do)
    // Its spectrum against the whole: every group's ear-weighted power together is 0 dB.
    SpectrumAnalyzer::Bands relDb {};
    double levelDb = 0.0; // ear-weighted, against the whole
    double width = 0.0;   // side share, 300 Hz..10 kHz
    bool widthValid = false;
    std::array<bool, kStemSections> inSection {}; // also plays in the references' intro / build / break
};

struct Arrangement
{
    std::array<ArrangementGroup, kStemGroups> groups {};
    bool fromReferences = false; // spectra from stems; else the typical profile's shapes
    bool valid() const;
};

// From a stem set (moved to the track's key): each group's drop spectrum averaged over the
// profiles that have one, at the level corridor's centre. Sets saved before phase 10 have no
// spectra: their levels with the typical shapes.
Arrangement arrangementFromStems(const StemTarget& target);
// A typical house / tech house drop: drums, bass and the rest; a vocal is not expected.
Arrangement typicalArrangement();

struct SketchChannel
{
    int group = -1;
    SpectrumAnalyzer::Bands db {}; // over the drop, in the scale of SketchInput::groupDb
};

struct SketchInput
{
    std::array<SpectrumAnalyzer::Bands, kStemGroups> groupDb {}; // our groups over the drop, dB (-200 absent)
    std::array<bool, kStemGroups> present {};
    std::vector<SketchChannel> channels; // each channel on its own: does any of them play in a region
    // A region a group's cleanup or the Brain's cuts take out on purpose is not a hole.
    std::array<std::array<bool, kStemRegions>, kStemGroups> cutOnPurpose {};
};

struct SketchGhost
{
    int group = -1;
    int region = -1; // -1: the whole group is missing; else the region of a group that plays but not there
    SpectrumAnalyzer::Bands db {}; // in the scale of SketchInput::groupDb
    double levelDb = 0.0;          // ear-weighted, against the whole with every ghost in
    double lowHz = 0.0, highHz = 0.0; // where the middle 80% of its ear-weighted power is
    double shortDb = 0.0;          // region ghosts: how far the group sits under the references there
    int priority = 5;              // as the channel that would play it
};

struct SketchAnalysis
{
    bool valid = false;  // something plays
    bool sketch = false; // at least one ghost
    bool fromReferences = false;
    std::vector<SketchGhost> ghosts;
    std::array<bool, kStemGroups> missing {};
    std::array<std::array<bool, kStemRegions>, kStemGroups> hole {}; // the region ghosts
    SpectrumAnalyzer::Bands ghostDb {}; // every ghost, as one power sum
    SpectrumAnalyzer::Bands oursDb {};  // our groups together
};

SketchAnalysis analyseSketch(const SketchInput& input, const Arrangement& arrangement);

// The master's curve (any scale) with the ghosts in, carried over by the master's level against
// our groups' sum over 100 Hz..5 kHz.
Curve addGhosts(const Curve& masterDb, const SketchAnalysis& sketch);

// Per region of `plain`: true when the arrangement explains it (with the ghosts in, the same
// bands are inside the corridor or at most a third as far out on the same side).
std::vector<bool> arrangementRegions(const TonalComparison& plain, const TonalComparison& withGhosts);

} // namespace ma

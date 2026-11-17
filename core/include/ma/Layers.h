#pragma once

#include "Instrument.h"
#include "SpectrumAnalyzer.h"

#include <vector>

namespace ma

{

// Phase 42: parts stacked in the same register. Three leads, two pads and a guitar can each sit
// inside the corridor and never trip a collision, and the mix still sounds crowded, because they
// all carry the same band. The engineer's answer is not a narrow cut on a pair: it is to decide
// which part owns that register and to take the others down in it.
//
// Channels are compared on their shape (power share per band) between `kLayerLowHz` and
// `kLayerHighHz`, so a loud and a quiet part carrying the same thing still count as layers. A
// group is a connected set of parts overlapping by `kLayerOverlap` or more; its leader is the
// most important one (priority, then level), and every other member gets one broad bell that
// leaves the leader `kLayerAheadDb` ahead in the shared region.
//
// Everything is measured before the Brain's EQ, so planning again gives the same cut instead of
// stacking a new one on the last.
constexpr double kLayerLowHz = 150.0, kLayerHighHz = 6000.0;
constexpr double kLayerOverlap = 0.5;    // share of the shape two parts have in common
constexpr double kLayerQuietDb = 20.0;   // a part this far under the loudest layer is not one
constexpr double kLayerAheadDb = 3.0;    // the leader stands this far over each follower there
constexpr double kMaxLayerCutDb = 6.0;
constexpr double kMinLayerCutDb = 1.0;

struct LayerChannel
{
    SpectrumAnalyzer::Bands bandsDb {}; // ERB, after trim and cleanup, before the Brain's EQ
    Role role = Role::other;
    int priority = 5;
    bool valid = false;
};

struct LayerCut
{
    int channel = -1;
    double hz = 0.0, q = 1.0, gainDb = 0.0; // the bell to set on that channel
    double underLeaderDb = 0.0;             // where it sits against the leader before the cut
};

struct LayerGroup
{
    std::vector<int> members; // the leader first, then the followers in the order they are cut
    double lowHz = 0.0, highHz = 0.0;
    double overlap = 0.0; // the weakest pair in the group
    std::vector<LayerCut> cuts;
};

std::vector<LayerGroup> findLayers(const std::vector<LayerChannel>& channels);

// Whether a role can be a layer at all: the low end is its own question (ma/LowEnd.h), and buses
// and sends carry other channels' signal.
bool layerRole(Role role);
// Only parts of the same kind are layers of each other: a hat and a synth share a register but not
// a job, and they live together by rhythm. 0 = tonal (lead, pad, vocal, the rest), 1 = percussion.
int layerClass(Role role);

} // namespace ma

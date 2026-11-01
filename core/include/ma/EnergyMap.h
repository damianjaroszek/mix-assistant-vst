#pragma once

#include "StemReference.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 20: the energy map. A track's loudness over time (EnergyTimeline, a point a second,
// against its drop) next to the references' own, lined up at the first drop: when the first
// drop comes, how many drops there are, how deep the track goes between them, and where it
// runs far from the references for a while. Advice and a view only.

// Where the drops come in: the first second at the drop's level (within 3 dB) that starts a
// run of at least 4 s, after at least 4 s without one, moved back 1.5 s for the short-term
// loudness's lag (as the Brain finds its first drop). Not-heard seconds (NaN) break a run.
std::vector<double> energyEntries(const std::vector<double>& levelDb);

// The lowest 4 s average between the first drop and the last second at the drop's level (an
// outro fading out does not count); NaN without a drop.
double deepestBetweenDrops(const std::vector<double>& levelDb);

struct EnergyCorridor
{
    bool valid = false;
    int profiles = 0;
    // Per second, relative to the references' first drop: point i is at `fromSeconds + i`.
    double fromSeconds = 0.0;
    std::vector<double> low, centre, high;
    std::vector<int> count; // how many references reach that second
    std::array<std::vector<double>, kStemGroups> groupCentre; // median per group (kEnergyFloorDb when none plays)
    StemCorridor firstDrop, drops, deepest;
    // The corridor at `secondsFromFirstDrop`, when at least half the references reach it.
    bool at(double secondsFromFirstDrop, double& lowDb, double& centreDb, double& highDb) const;
};

EnergyCorridor energyCorridor(const StemTarget& target);

struct EnergyComparison
{
    bool valid = false; // our track's first drop is known
    double firstDropSeconds = 0.0;
    int drops = 0;
    double deepestDb = 0.0; // NaN: no second drop level after the first
    double firstDropOutside = 0.0, dropsOutside = 0.0, deepestOutside = 0.0; // StemCorridor::outside
    // Stretches of at least 8 s where our loudness sits 3 dB or more outside the references'
    // (lined up at the first drop), largest first, at most three.
    struct Span
    {
        double fromSeconds = 0.0, toSeconds = 0.0; // in our track
        double deviationDb = 0.0;                  // average distance outside, + = louder
    };
    std::vector<Span> spans;
};

EnergyComparison compareEnergy(const EnergyTimeline& ours, const EnergyCorridor& references);

} // namespace ma

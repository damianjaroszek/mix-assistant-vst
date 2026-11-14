#pragma once

#include "SpectrumAnalyzer.h"

#include <array>
#include <string>
#include <vector>

namespace ma
{

// Tonal balance of the mix against a target built from reference tracks.
//
// Everything is on ERB bands. Curves are long-term average spectra with their overall
// level removed (mean over 60 Hz..8 kHz set to 0 dB), so only the shape counts, the
// way Tonal Balance Control compares tracks of different loudness.
using Curve = SpectrumAnalyzer::Bands;

constexpr BandScale kTonalScale = BandScale::erb;

Curve normaliseCurve(const Curve& levelsDb);

struct TonalTarget
{
    std::string name;
    std::vector<Curve> references; // normalised curves, one per reference track

    // Corridor: median of the references, widened to the spread between them.
    Curve centre {};
    Curve low {};
    Curve high {};

    void rebuild();
    bool valid() const { return !references.empty(); }
};

struct TonalRegion
{
    int firstBand = 0, lastBand = 0;
    double lowHz = 0.0, highHz = 0.0;
    double deviationDb = 0.0; // mean distance outside the corridor; < 0 = hole, > 0 = too much
    bool hole() const { return deviationDb < 0.0; }
};

struct TonalComparison
{
    Curve mixDb {};       // the mix, normalised and aligned to the target
    Curve deviationDb {}; // per band: 0 inside the corridor, signed distance outside it
    std::vector<TonalRegion> regions; // largest first
    bool valid = false;
};

TonalComparison compareToTarget(const Curve& mixLevelsDb, const TonalTarget& target);

// Phase 24: the four broad ranges Tonal Balance Control shows (Low, Low-Mid, High-Mid, High)
// and where the mix sits in each against the corridor's centre (the references' median).
// A mix can be inside the corridor and still lean on one of its edges over a whole range.
constexpr int kTonalZones = 4;
double tonalZoneEdgeHz(int edge); // 0..kTonalZones

struct TonalZone
{
    int firstBand = 0, lastBand = -1;
    double lowHz = 0.0, highHz = 0.0;
    double offsetDb = 0.0; // mean of the mix minus the centre over the bands it plays in; > 0 = above
    bool valid = false;    // the mix plays in most of the range
};

std::array<TonalZone, kTonalZones> zoneOffsets(const TonalComparison& comparison, const TonalTarget& target);
// Sum over the bands of the distance outside the corridor.
double outsideCorridorDb(const TonalComparison& comparison);

// Phase 38: the target used while the user has none of their own. The mean of two house masters
// (one remix, one progressive house track), smoothed over +-2 ERB bands, inside a corridor as wide
// as Tonal Balance Control's (+-2.5 dB, +-3.5 dB below 60 Hz and above 10 kHz).
TonalTarget builtinHouseTarget();
constexpr const char* kBuiltinTargetName = "Typowy house (wbudowany)";

// Plain-text file: name on the first line, then one line of band levels per reference.
std::string serialiseTarget(const TonalTarget& target);
bool parseTarget(const std::string& text, TonalTarget& target);

} // namespace ma

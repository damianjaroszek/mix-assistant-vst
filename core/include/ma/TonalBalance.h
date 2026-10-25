#pragma once

#include "SpectrumAnalyzer.h"

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

// Plain-text file: name on the first line, then one line of band levels per reference.
std::string serialiseTarget(const TonalTarget& target);
bool parseTarget(const std::string& text, TonalTarget& target);

} // namespace ma

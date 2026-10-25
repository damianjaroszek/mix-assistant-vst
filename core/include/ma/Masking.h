#pragma once

#include "SpectrumAnalyzer.h"

#include <string>
#include <vector>

namespace ma
{

// Psychoacoustic masking between mix channels, on ERB bands.
//
// Each channel's raw band levels are weighted by the ear (ISO 226), then spread across
// neighbouring bands the way a masker spreads along the cochlea (steeper towards low
// frequencies, shallower towards high). Channel A masks channel B in a band when A's
// spread excitation there rises above B's own level. Only bands that matter for B
// (within 24 dB of its strongest band) count.
struct MaskingChannel
{
    SpectrumAnalyzer::Bands rawDb {}; // ERB bands, dBFS
    int priority = 5;                 // 1 = most important
    bool valid = false;
};

struct MaskingIssue
{
    int masker = -1;
    int masked = -1;
    double score = 0.0;   // 0..1, share of the masked channel's important bands that are covered
    double lowHz = 0.0;   // strongest contiguous masked range, band edges
    double highHz = 0.0;
};

struct MaskingResult
{
    // amount[masker][masked], 0..1
    std::vector<std::vector<double>> amount;
    // How many channels compete near the top level in each band (crowding).
    std::vector<int> crowding;
    // Pairs where a less important (or equally important) channel covers a more important one,
    // strongest first.
    std::vector<MaskingIssue> issues;
};

MaskingResult analyseMasking(const std::vector<MaskingChannel>& channels, double minimumScore = 0.25);

} // namespace ma

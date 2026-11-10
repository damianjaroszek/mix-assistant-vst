#pragma once

#include "SpectrumAnalyzer.h"

#include <vector>

namespace ma
{

// Phase 38: nothing gets lost anywhere in the song, not only in the drop. Per second of the song
// each part's share of its own main range (the bands within kOwnRangeDb of its loudest, 60 Hz-12 kHz)
// against everything else there. A part is lost for a while when, playing (its level within
// kPlayingDb of its loud level), its share falls kLostBelowTypicalDb under its own typical share and
// the rest is at least kLostAbsoluteDb louder there, for kMinLostSeconds or more (one second gap
// allowed). Advice only: the fix is volume automation in FL or a quieter masker in that passage.

constexpr double kOwnRangeDb = 10.0;
constexpr double kPlayingDb = 15.0;
constexpr double kLostBelowTypicalDb = 6.0;
constexpr double kLostAbsoluteDb = 6.0;
constexpr int kMinLostSeconds = 4;

struct AudibilityChannel
{
    bool valid = false;
    bool background = false; // a send or a bus: it may hide behind the dry parts, never counted as lost
    // Per second of the song (index = second), as the mix hears it (dB per ERB band); a second the
    // channel was not heard holds all bands at or below -150.
    std::vector<SpectrumAnalyzer::Bands> secondsDb;
};

struct LostSpan
{
    int channel = -1;
    int coveredBy = -1;        // the other part with the most power in that range over the span
    int fromSecond = 0, toSecond = 0; // inclusive
    double lowHz = 0.0, highHz = 0.0; // its main range
    double typicalShareDb = 0.0;      // its share of its range where it usually plays
    double spanShareDb = 0.0;         // its share over the span (mean power)
};

std::vector<LostSpan> findLostSpans(const std::vector<AudibilityChannel>& channels);

} // namespace ma

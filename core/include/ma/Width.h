#pragma once

#include "Stereo.h"

#include <vector>

namespace ma
{

// Phase 26: a width slider on every Agent, from mono through "as it is" to as wide as is safe.
// Above `fromHz` the side is scaled (below 100%) or the side raised and a delayed mid added
// (above it); below `fromHz` the side goes, so the low end is mono. Whatever the slider says,
// no band of the channel ends under `kSafeWidthCoherence` (up to two guard bells cut the side
// there, and "max" is as far as that still works), and no band of the sum falls under it because
// of a slider (the widenings playing there step back together). Every Agent runs the same plan on
// what all the Agents publish and takes its own row, so they agree without the Brain.
constexpr float kWidthNeutral = 100.0f; // percent: no change, the processing is off
constexpr float kWidthMax = 200.0f;
constexpr double kSafeWidthCoherence = 0.2;
constexpr double kMaxWidthSideDb = 6.0;  // at the top of the slider, the side raised by this...
constexpr double kMaxWidthWidenDb = -2.0; // ...and the widener this far under the mid

struct WidthInput
{
    bool valid = false;  // measured mid and side
    bool inMix = true;   // false for a bus or a send: their sources are in the sum already
    float request = kWidthNeutral; // the slider, percent
    float fromHz = 150.0f;
    float delayMs = 8.0f;
    StereoSettings stereo {};     // what the Brain set
    SpectrumAnalyzer::Bands midDb {}, sideDb {}; // as the mix hears them (trim and EQ in), before the stereo EQ
};

struct WidthPlan
{
    WidthSettings settings {};
    double reach = 0.0;        // above 100%: how much of the full widening was safe for the channel (0..1)
    bool limitedBySum = false; // the sum made it narrower
    double sumLimitHz = 0.0;   // where
    double worstCoherence = 1.0; // the channel's own worst audible band after it (from fromHz up)
    double worstHz = 0.0;
    SpectrumAnalyzer::Bands coherenceAfter {};
};

bool widthRequested(float request);
// The widener's delay for an Agent's slot: different on every channel, so they do not fuse.
float widthDelayMs(int slot);
std::vector<WidthPlan> planWidth(const std::vector<WidthInput>& channels);

} // namespace ma

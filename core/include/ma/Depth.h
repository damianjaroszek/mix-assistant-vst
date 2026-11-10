#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>

namespace ma
{

// Phase 34: depth. A reverb or delay return sits behind the dry parts when it is darker than them
// and gets out of the way while the main voice sings. The high cut is the send's own (after its
// cleanup); the give-way under the voice is the Agent's duck keyed over the bus (ma/Unmask.h).
struct DepthSettings
{
    float highCutHz = 0.0f; // 12 dB/oct low-pass; <= 0 = off
};

bool depthOn(const DepthSettings& depth);
// What the high cut does at `hz`, dB (<= 0).
double depthResponseDb(const DepthSettings& depth, double hz);

class HighCut
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const DepthSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    double filterHz = -1.0;
    std::array<Biquad, 2> lowPass {};
    DepthSettings settings {};
};

// Times that sit on the groove, for the user's reverb and delay (ms, at `bpm`).
struct DepthTimes
{
    bool valid = false;
    double preDelayMs = 0.0;   // a 1/64: the dry hit comes first, the room after it
    double decaySeconds = 0.0; // half a bar: the tail is gone before the next phrase starts
    double eighthMs = 0.0, dottedEighthMs = 0.0, quarterMs = 0.0;
};
DepthTimes depthTimes(double bpm);

// Brightness as the depth step reads it: level 8-16 kHz minus level 1-4 kHz (power sums, dB).
double topTiltDb(const SpectrumAnalyzer::Bands& erbDb);

constexpr double kSendHighCutHz = 9000.0;
constexpr double kDarkerThanMixDb = 3.0; // a send at least this much darker than the mix already sits behind
constexpr float kSendDuckDb = 3.0f;      // under the main voice: enough to clear it, the room stays

} // namespace ma

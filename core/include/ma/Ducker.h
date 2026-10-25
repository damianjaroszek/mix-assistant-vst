#pragma once

#include "Biquad.h"

#include <array>

namespace ma
{

// Settings of the dynamic "give way" on a channel, driven by another channel's signal
// (the sidechain). Band mode dips only the colliding band with a bell whose depth follows
// the sidechain; whole mode turns the channel down like classic sidechain compression.
struct DuckSettings
{
    float on = 0.0f;           // 0 / 1
    float wholeSignal = 0.0f;  // 0 = band, 1 = whole channel
    float hz = 60.0f;          // band centre, also where the sidechain is listened to
    float q = 1.0f;
    float depthDb = 6.0f;      // most it ducks
    float thresholdDb = -30.0f;
    float attackMs = 3.0f;
    float releaseMs = 120.0f;
};

// Release that recovers within the given note length at `bpm` (1.0 / 16 = a sixteenth note).
inline double syncedReleaseMs(double bpm, double noteFraction)
{
    return bpm > 0.0 ? 240000.0 / bpm * noteFraction : 0.0;
}

// Allocation free; process() runs on the audio thread.
class Ducker
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const DuckSettings& settings);

    // `main` is processed in place; `sidechain` may be null (then nothing ducks).
    void process(float* const* main, int numMain, const float* const* sidechain, int numSidechain, int numSamples);

    double reductionDb() const { return peakReductionDb; } // deepest dip in the last process(), >= 0
    double sidechainDb() const { return sidechainLevelDb; } // detector level in the band

private:
    void updateDetector();

    double sampleRate = 48000.0;
    DuckSettings settings {};
    Biquad detector[2] {}; // band-pass, two in series
    std::array<Biquad, kMaxChannels> dip {};
    double envelope = 0.0;
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    double lastReductionDb = 0.0;
    double peakReductionDb = 0.0;
    double sidechainLevelDb = -200.0;
    double appliedGain = 1.0;
};

} // namespace ma

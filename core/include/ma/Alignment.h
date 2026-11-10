#pragma once

#include "Biquad.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 35: the bass's low end against the kick's in time. Where both play at once (a sub under the
// kick, a bass note on the beat), a few milliseconds between them shift their low ends against each
// other and part of both cancels. The meter finds the lag that lines them up best (cross-correlation
// of the two below 150 Hz, +-kMaxAlignLagMs) while the kick plays; the Agent can delay itself
// (`AlignSettings`) when it comes early. Needs the kick on the Agent's sidechain input.
struct AlignSettings
{
    float delayMs = 0.0f; // the channel's own delay, 0 = off
};

constexpr double kMaxAlignLagMs = 10.0;
constexpr float kMaxAlignDelayMs = 10.0f;

bool alignOn(const AlignSettings& align);

class LowAlignMeter
{
public:
    void prepare(double sampleRate);
    void reset();
    // `main` = this channel (mono), `key` = the sidechain (mono), the same block.
    void process(const float* main, const float* key, int numSamples);

    bool valid() const { return seconds >= 2.0 && keyEnergy > 0.0 && mainEnergy > 0.0; }
    // > 0: this channel comes later than the key, < 0: earlier.
    double bestLagMs() const;
    double correlationAtBest() const;
    double correlationAtZero() const;

private:
    static constexpr int kMaxLag = 24;     // decimated samples (at ~2 kHz: 12 ms)
    static constexpr int kHistory = 64;
    double rate = 48000.0;
    int factor = 24, phase = 0, maxLag = 20;
    std::array<Biquad, 2> mainLow {}, keyLow {};
    std::array<double, kHistory> mainHistory {}, keyHistory {};
    int write = 0;
    std::array<double, 2 * kMaxLag + 1> cross {};
    double mainEnergy = 0.0, keyEnergy = 0.0, decay = 0.999, seconds = 0.0, keyShort = 0.0;
};

// The channel's own delay, before its chain (allocated in prepare).
class AlignDelay
{
public:
    void prepare(double sampleRate, int maxChannels = 2);
    void reset();
    void setSettings(const AlignSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double rate = 48000.0;
    int delaySamples = 0, pos = 0;
    std::vector<std::vector<float>> lines;
};

} // namespace ma

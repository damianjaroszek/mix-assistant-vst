#pragma once

#include "Biquad.h"

#include <array>
#include <cmath>

namespace ma
{

// The Brain's corrections on a channel: a few bell filters, set by the solver.
struct EqBand
{
    float hz = 1000.0f;
    float gainDb = 0.0f; // 0 = band off
    float q = 1.0f;
};

constexpr int kEqBands = 4;

inline bool isOn(const EqBand& band)
{
    return std::abs(band.gainDb) >= 0.01f;
}

inline bool sameBand(const EqBand& a, const EqBand& b)
{
    return std::abs(a.hz - b.hz) < 1e-3f && std::abs(a.gainDb - b.gainDb) < 1e-3f && std::abs(a.q - b.q) < 1e-4f;
}
using EqSettings = std::array<EqBand, kEqBands>;

// Response of the whole EQ at `hz`, in dB (evaluated at 48 kHz, close enough for analysis).
double eqResponseDb(const EqSettings& eq, double hz);

// Real-time EQ for up to two channels. Coefficients are recomputed only when the
// settings change; nothing allocates.
class ChannelEq
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setBands(const EqSettings& settings);
    bool isActive() const { return active; }
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    EqSettings current {};
    bool initialised = false;
    bool active = false;
    std::array<std::array<Biquad, kEqBands>, kMaxChannels> filters {};
};

} // namespace ma

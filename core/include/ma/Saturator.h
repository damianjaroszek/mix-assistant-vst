#pragma once

#include "Oversampler.h"

#include <array>

namespace ma
{

// Creative saturation and deliberate overdrive ("dopalenie"), run at four times the
// sample rate so the harmonics it makes above Nyquist do not fold back as aliasing.
//
// The Agent's trim has already brought the channel to its gain-staging level (about
// -18 dBFS RMS), so `driveDb` means the same thing on every channel, like the input of an
// analog unit: 0 dB puts the channel's peaks around the shaper's knee. The wet signal is
// level-compensated for that reference level, so turning up the drive changes the colour
// rather than the loudness, and the dry/wet mix happens before downsampling, so both sit
// in phase.
enum class SaturationMode
{
    tape = 0,     // soft, mostly odd harmonics, rounds the peaks
    tube = 1,     // asymmetric: even harmonics, warm
    softClip = 2, // cubic clipper: transparent until the knee, then firm
    hardClip = 3, // brick-wall clipper: the deliberately dirty one
    fold = 4      // wave folder: peaks fold back, bright and synthetic
};
constexpr int kSaturationModes = 5;

struct SaturationSettings
{
    float on = 0.0f;
    float mode = 0.0f;   // SaturationMode as a float, so the settings stay plain data on the bus
    float driveDb = 6.0f;
    float mix = 1.0f;    // 0..1
};

// Allocation free; process() runs on the audio thread.
class Saturator
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kOversampling = 4;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const SaturationSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

    // Wet level compensation in use (dB), for display and tests.
    double compensationDb() const;

    // The bare transfer curve of a mode at unity drive (input and output around +-1).
    static double shape(SaturationMode mode, double x);

private:
    struct ChannelState
    {
        Oversampler4x oversampler;
        double dcIn = 0.0, dcOut = 0.0;
    };

    double oversampledSample(ChannelState& s, double x) const;
    void updateGains();

    double sampleRate = 48000.0;
    SaturationSettings settings {};
    std::array<ChannelState, kMaxChannels> state {};
    double driveGain = 1.0, makeup = 1.0, dcCoeff = 0.9995;
    bool prepared = false;
};

// What the saturator makes of a steady tone at `hz` with `levelDb` RMS (dBFS): the power of
// its 2nd to 8th harmonics, each and together, against the tone's own. Runs the real shaper
// for a quarter second, so it allocates: for planning, never on the audio thread.
struct SaturationPrediction
{
    std::array<double, 7> harmonicDb {}; // 2nd .. 8th, dB against the fundamental
    double overtonesDb = -200.0;         // all of them together
};
SaturationPrediction predictSaturation(const SaturationSettings& settings, double hz, double levelDb, double sampleRate = 48000.0);

} // namespace ma

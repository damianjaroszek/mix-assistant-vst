#pragma once

#include <array>
#include <cstddef>

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
    // Polyphase IIR half-band filter: two chains of first-order allpasses (Valenzuela and
    // Constantinides). One instance either doubles or halves the rate of one signal.
    template <size_t Coefs>
    struct HalfBand
    {
        std::array<double, Coefs> c {};
        std::array<double, Coefs> x1 {}, y1 {};
        double pendingOdd = 0.0; // downsampling: the odd input waiting for the next pair

        double allpasses(double in, int first);
        void up(double in, double& even, double& odd);
        double down(double even, double odd);
        void reset();
    };

    struct ChannelState
    {
        HalfBand<8> up1, down1; // base rate <-> 2x
        HalfBand<4> up2, down2; // 2x <-> 4x
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

} // namespace ma

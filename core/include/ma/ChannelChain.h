#pragma once

#include "Biquad.h"
#include "Oversampler.h"

#include <array>

namespace ma
{

// The mix engineer's tools on a channel (phase 7), set by the Brain's "Ustaw cały miks"
// with the whole mix in view, or by hand. All allocation free; process() runs on the
// audio thread. Settings are plain floats so they travel on the bus as they are.

// Housekeeping: a high-pass under the content that matters (24 dB/oct), the low end in
// mono (below `monoBelowHz` the side signal is removed), and one wide cut for low-mid mud.
struct CleanupSettings
{
    float hpfHz = 0.0f;       // 0 = off
    float monoBelowHz = 0.0f; // 0 = off
    float mudHz = 300.0f;
    float mudDb = 0.0f;       // 0 = off, else a cut
    float mudQ = 1.0f;
    // Narrow cuts on resonances (0 dB = off).
    struct Notch
    {
        float hz = 1000.0f;
        float db = 0.0f;
        float q = 6.0f;
    };
    static constexpr int kNotches = 3;
    std::array<Notch, kNotches> notches {};
};

// Response of the high-pass and the mud cut at `hz` (dB), for the Brain's predictions.
double cleanupResponseDb(const CleanupSettings& settings, double hz);

class CleanupFilter
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const CleanupSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    void update();

    double sampleRate = 48000.0;
    CleanupSettings settings {};
    bool initialised = false;
    std::array<std::array<Biquad, 2>, 2> highPass {}; // per channel, two sections
    std::array<Biquad, 2> mud {};
    std::array<std::array<Biquad, CleanupSettings::kNotches>, 2> notch {};
    // Linkwitz-Riley crossover at monoBelowHz: the side keeps only its high half, the mid
    // goes through the same crossover summed back (an allpass), so mid and side stay in phase.
    std::array<Biquad, 2> sideHigh {}, midLow {}, midHigh {};
};

// Feed-forward compressor, stereo linked, RMS detector (about 10 ms) with a soft knee.
// Attack and release shape the gain reduction itself, so the transient a slow attack
// lets through stays intact.
struct CompressorSettings
{
    float on = 0.0f;
    float thresholdDb = -24.0f;
    float ratio = 2.0f;
    float attackMs = 10.0f;
    float releaseMs = 120.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
};

// Static curve: how much the compressor takes off at a steady input level (dB, >= 0).
double compressorReductionDb(const CompressorSettings& settings, double inputDb);

class Compressor
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const CompressorSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);
    double reductionDb() const { return peakReductionDb; } // deepest in the last process()

private:
    double sampleRate = 48000.0;
    CompressorSettings settings {};
    double meanSquare = 0.0, rmsCoeff = 0.0;
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    double reduction = 0.0; // dB, smoothed
    double peakReductionDb = 0.0;
};

// Peak clipper at 4x oversampling: shaves the tops of transients that only cost headroom.
// Below the knee the signal is untouched; `softness` 0 is a hard clip, 1 starts rounding
// 6 dB under the ceiling.
struct ClipperSettings
{
    float on = 0.0f;
    float ceilingDb = -6.0f;
    float softness = 0.5f;
};

class Clipper
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const ClipperSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);
    double clippedDb() const { return peakClipDb; } // most taken off a peak in the last process()

    static double shape(double x, double ceiling, double kneeStart);

private:
    ClipperSettings settings {};
    std::array<Oversampler4x, kMaxChannels> oversamplers {};
    double ceiling = 0.5, kneeStart = 0.5;
    double peakClipDb = 0.0;
};

// Follows the fundamental of a bass line (25 to 160 Hz) from zero crossings of its low
// band. Cheap enough for the audio thread; wrong on chords, right on bass notes.
class LowPitchTracker
{
public:
    void prepare(double sampleRate);
    void reset();
    // Feed one mono sample; returns the current pitch estimate (0 = none).
    double process(double x);
    double pitchHz() const { return pitch; }
    double levelDb() const;

private:
    double sampleRate = 48000.0;
    Biquad lowPass[2] {};
    double meanSquare = 0.0, levelCoeff = 0.0;
    double previous = 0.0;
    double sinceCrossing = 0.0;
    double lastPeriod = 0.0;
    bool armed = false; // hysteresis: went below -threshold since the last upward crossing
    double pitch = 0.0;
    int holdSamples = 0;
};

// Evens out bass notes for the ear: a note the ear hears quieter (lower, by the equal-
// loudness contour) is lifted, a higher one lowered, relative to `referenceHz` (the line's
// usual note), by `amount` of the full difference, at most +-6 dB.
struct BassEvenerSettings
{
    float on = 0.0f;
    float amount = 0.6f;
    float referenceHz = 55.0f;
};

double bassEvenerGainDb(const BassEvenerSettings& settings, double noteHz);

class BassEvener
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const BassEvenerSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);
    double currentGainDb() const { return gainDb; }
    double currentNoteHz() const { return tracker.pitchHz(); }

private:
    double sampleRate = 48000.0;
    BassEvenerSettings settings {};
    LowPitchTracker tracker;
    std::array<float, 200> table {}; // gain per Hz from 0 to 199, rebuilt when settings change
    double gainDb = 0.0, smoothCoeff = 0.0, appliedGain = 1.0;
};

} // namespace ma

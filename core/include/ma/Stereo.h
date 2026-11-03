#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>
#include <vector>

namespace ma
{

// Phase 8: stereo per frequency band, the way a per-band correlation meter shows it.
// Mid (L+R)/2 and side (L-R)/2 power per ERB band; their balance gives the band's
// coherence, +1 mono, 0 as much side as mid, -1 all side (gone in mono).
double coherence(double midDb, double sideDb);

class StereoAnalyzer
{
public:
    void prepare(double sampleRate);
    void reset();
    void process(const float* left, const float* right, int numSamples);

    // Roughly the last second, and everything since reset.
    SpectrumAnalyzer::Bands recentMidDb() const { return mid.recentDb(); }
    SpectrumAnalyzer::Bands recentSideDb() const { return side.recentDb(); }
    SpectrumAnalyzer::Bands averageMidDb() const { return mid.averageDb(); }
    SpectrumAnalyzer::Bands averageSideDb() const { return side.averageDb(); }
    int framesAnalysed() const { return mid.framesAnalysed(); }

private:
    static constexpr int kChunk = 256;
    SpectrumAnalyzer mid, side;
    std::array<float, kChunk> midBuffer {}, sideBuffer {};
};

// Mid-side EQ on a channel: bells on the side signal only. A cut where a band falls into
// anti-phase keeps it in mono; a small lift where the mix is narrow opens it up. The mid,
// and so the mono sum, is untouched.
struct StereoSettings
{
    struct Bell
    {
        float hz = 1000.0f;
        float db = 0.0f; // 0 = off
        float q = 2.0f;
    };
    static constexpr int kBells = 4;
    std::array<Bell, kBells> side {};

    // Phase 25: width for a part that has (almost) none. A copy of the mid, delayed by a few ms and
    // high-passed, goes into the side at `amountDb` under the mid (the way Ozone's Stereoize works):
    // L and R get complementary combs, the mono sum (the mid) stays exactly as it was.
    struct Widener
    {
        float amountDb = -60.0f; // <= kWidenOffDb = off
        float lowHz = 300.0f;    // the generated side starts here (24 dB/oct)
        float delayMs = 8.0f;
    };
    Widener widen {};
};

constexpr float kWidenOffDb = -40.0f;
constexpr float kMaxWidenDelayMs = 20.0f;

// Phase 26: the Agent's width slider, worked out by `planWidth` (ma/Width.h) and kept apart from the
// Brain's settings (it is the user's, never in a Command or the A/B backup). While active, the side
// below `fromHz` is removed (the low end mono), the side above it scaled by `scale` (0 = mono),
// the slider's own widener (replacing the Brain's) adds a delayed mid from max(300 Hz, fromHz), and
// up to two guard bells cut the side wherever a band would fall under the safe coherence.
struct WidthSettings
{
    static constexpr int kGuards = 2;
    float active = 0.0f;
    float fromHz = 150.0f;
    float scale = 1.0f;
    float widenDb = -60.0f; // <= kWidenOffDb = off
    float delayMs = 8.0f;
    std::array<StereoSettings::Bell, kGuards> guard {};
};

bool widthOn(const WidthSettings& width);
// Side power after the stereo EQ, the widener(s) and the width, for a band at `hz` with this
// mid and side power (power sums, the generated side counted as uncorrelated).
double processedSidePower(const StereoSettings& settings, const WidthSettings& width, double hz, double midPower, double sidePower);

double sideResponseDb(const StereoSettings& settings, double hz);
// Side power the widener adds per unit of mid power at `hz` (0 when off).
double widenPowerRatio(const StereoSettings& settings, double hz);
bool widenOn(const StereoSettings& settings);
bool isOn(const StereoSettings& settings);

class StereoShaper
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const StereoSettings& settings);
    void setWidth(const WidthSettings& width);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    StereoSettings settings {};
    bool initialised = false;
    std::array<Biquad, StereoSettings::kBells> bells {};
    // Phase 25: the widener's delay line (allocated in prepare) and its high-pass.
    std::vector<double> delay;
    int delayPos = 0, delaySamples = 0;
    double widenGain = 0.0;
    std::array<Biquad, 2> widenHighPass {};
    // Phase 26: the width slider.
    WidthSettings width {};
    bool widthInitialised = false;
    std::array<Biquad, 2> widthSideHighPass {}; // the side below fromHz, taken out
    std::array<Biquad, 2> widthHighPass {}; // its widener's
    std::array<Biquad, WidthSettings::kGuards> guards {};
    int widthDelaySamples = 0;
    double widthScale = 1.0, widthWidenGain = 0.0;      // targets
    double scaleNow = 1.0, widthWidenNow = 0.0;          // ramped per sample
};

} // namespace ma

#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>

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
};

double sideResponseDb(const StereoSettings& settings, double hz);
bool isOn(const StereoSettings& settings);

class StereoShaper
{
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const StereoSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    double sampleRate = 48000.0;
    StereoSettings settings {};
    bool initialised = false;
    std::array<Biquad, StereoSettings::kBells> bells {};
};

} // namespace ma

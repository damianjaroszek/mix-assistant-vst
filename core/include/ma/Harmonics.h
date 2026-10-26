#pragma once

#include "Biquad.h"
#include "SpectrumAnalyzer.h"

#include <array>

namespace ma
{

// Harmonic enhancer, the "presence on small speakers" kind of saturation.
//
// The content below `hz` is saturated on its own and only the harmonics it makes above
// `hz` are added back, so a sub bass gains overtones a phone can play while its
// fundamental stays clean. The source is normalised by its envelope before the shaper,
// so `amountDb` is the harmonics' level relative to the source whatever the channel's
// level: -12 dB means overtones 12 dB under the band they come from.
struct HarmonicSettings
{
    float on = 0.0f;
    float hz = 100.0f;      // source below, harmonics above
    float amountDb = -12.0f;
};

// Allocation free; process() runs on the audio thread.
class HarmonicEnhancer
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate);
    void reset();
    void setSettings(const HarmonicSettings& settings);
    void process(float* const* channels, int numChannels, int numSamples);

private:
    struct State
    {
        Biquad low[2], high[2];
        double meanSquare = 0.0;
    };
    void updateFilters();
    static double shape(double x);

    double sampleRate = 48000.0;
    HarmonicSettings settings {};
    std::array<State, kMaxChannels> state {};
    double envelopeCoeff = 0.0;
    double calibration = 1.0; // harmonics RMS per source RMS at amount 0 dB
};

// Offline estimate of what the enhancer adds to a band spectrum (dBFS per ERB band, raw),
// for the Brain to predict a proposal without hearing it first.
SpectrumAnalyzer::Bands predictHarmonics(const SpectrumAnalyzer::Bands& bandsDb, const HarmonicSettings& settings);

} // namespace ma

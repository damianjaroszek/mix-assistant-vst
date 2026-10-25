#pragma once

#include "LevelStats.h"
#include "LoudnessMeter.h"
#include "SpectrumAnalyzer.h"
#include "TruePeakMeter.h"

namespace ma
{

// Everything the Agent measures on one channel over a passage: gated RMS and crest,
// integrated loudness, true peak and the ERB-band spectrum. Allocation-free after prepare().
class ChannelAnalyzer
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset();
    void process(const float* const* channels, int numChannels, int numSamples);

    double rmsDb() const { return levels.rmsDb(); }
    double crestDb() const { return levels.crestDb(); }
    double integratedLufs() const { return loudness.integratedLufs(); }
    double truePeakDb() const { return truePeak.truePeakDb(); }
    double gatedSeconds() const { return static_cast<double>(levels.gatedSamples()) / sampleRate; }

    // Average ERB-band levels over the passage, raw and weighted by the ear (ISO 226, 80 phon).
    SpectrumAnalyzer::Bands spectrumDb() const { return spectrum.averageDb(); }
    SpectrumAnalyzer::Bands perceivedSpectrumDb() const;
    const SpectrumAnalyzer& spectrumAnalyzer() const { return spectrum; }

private:
    double sampleRate = 48000.0;
    SpectrumAnalyzer::Bands earWeights {};
    LevelStats levels;
    LoudnessMeter loudness;
    TruePeakMeter truePeak;
    SpectrumAnalyzer spectrum;
};

// ISO 226 weights for each band of a prepared analyser (dB the ear hears it quieter than
// 1 kHz). Compute once after prepare(): it is too slow for every audio block.
SpectrumAnalyzer::Bands equalLoudnessWeights(const SpectrumAnalyzer& layout, double phon = 80.0);

// Subtracts the weights from band levels in place.
void applyWeights(SpectrumAnalyzer::Bands& bands, const SpectrumAnalyzer::Bands& weights, int numBands);

} // namespace ma

#include "ma/ChannelAnalyzer.h"

#include "ma/EqualLoudness.h"

namespace ma
{

void ChannelAnalyzer::prepare(double newSampleRate, int numChannels)
{
    sampleRate = newSampleRate;
    loudness.prepare(newSampleRate, numChannels);
    truePeak.prepare(numChannels);
    spectrum.prepare(newSampleRate, BandScale::erb);
    earWeights = equalLoudnessWeights(spectrum);
    levels.reset();
}

void ChannelAnalyzer::reset()
{
    levels.reset();
    loudness.reset();
    truePeak.reset();
    spectrum.reset();
}

void ChannelAnalyzer::process(const float* const* channels, int numChannels, int numSamples)
{
    levels.process(channels, numChannels, numSamples);
    loudness.process(channels, numChannels, numSamples);
    truePeak.process(channels, numChannels, numSamples);
    spectrum.process(channels, numChannels, numSamples);
}

SpectrumAnalyzer::Bands ChannelAnalyzer::perceivedSpectrumDb() const
{
    auto bands = spectrum.averageDb();
    applyWeights(bands, earWeights, spectrum.numBands());
    return bands;
}

SpectrumAnalyzer::Bands equalLoudnessWeights(const SpectrumAnalyzer& layout, double phon)
{
    SpectrumAnalyzer::Bands weights {};
    for (int b = 0; b < layout.numBands(); ++b)
        weights[static_cast<size_t>(b)] = equalLoudnessWeightDb(layout.centreHz(b), phon);
    return weights;
}

void applyWeights(SpectrumAnalyzer::Bands& bands, const SpectrumAnalyzer::Bands& weights, int numBands)
{
    for (int b = 0; b < numBands; ++b)
        bands[static_cast<size_t>(b)] -= weights[static_cast<size_t>(b)];
}

} // namespace ma

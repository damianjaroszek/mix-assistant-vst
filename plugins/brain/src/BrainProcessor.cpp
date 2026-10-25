#include "BrainProcessor.h"

#include "BrainEditor.h"

BrainProcessor::BrainProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "BrainState", createLayout())
{
    peakCeiling = parameters.getRawParameterValue(peakCeilingId);
    for (auto& b : bands)
        b.store(-INFINITY);
}

juce::AudioProcessorValueTreeState::ParameterLayout BrainProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Pre-master headroom: highest true peak the mix may reach before mastering (Ozone).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { peakCeilingId, 1 }, "Pre-master peak ceiling",
        juce::NormalisableRange<float> { -12.0f, 0.0f, 0.5f }, -6.0f,
        juce::AudioParameterFloatAttributes().withLabel("dBTP")));

    return layout;
}

bool BrainProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void BrainProcessor::prepareToPlay(double sampleRate, int)
{
    const int channels = getTotalNumInputChannels();
    loudness.prepare(sampleRate, channels);
    truePeakMeter.prepare(channels);
    spectrum.prepare(sampleRate);
}

void BrainProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = getTotalNumInputChannels();
    const int numSamples = buffer.getNumSamples();
    for (int c = numChannels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear(c, 0, numSamples);

    if (resetRequested.exchange(false))
    {
        loudness.reset();
        truePeakMeter.reset();
        spectrum.reset();
    }

    // Analysis only: the audio passes through untouched.
    const auto* const* input = buffer.getArrayOfReadPointers();
    loudness.process(input, numChannels, numSamples);
    truePeakMeter.process(input, numChannels, numSamples);
    spectrum.process(input, numChannels, numSamples);

    integrated.store(loudness.integratedLufs());
    shortTerm.store(loudness.shortTermLufs());
    momentary.store(loudness.momentaryLufs());
    truePeak.store(truePeakMeter.truePeakDb());

    const auto recent = spectrum.recentDb();
    for (size_t b = 0; b < bands.size(); ++b)
        bands[b].store(recent[b]);
}

void BrainProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void BrainProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* BrainProcessor::createEditor()
{
    return new BrainEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BrainProcessor();
}

#include "AgentProcessor.h"

#include "AgentEditor.h"
#include "ma/Decibels.h"

#include <cmath>

AgentProcessor::AgentProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "AgentState", createLayout())
{
    trimParam = parameters.getRawParameterValue(trimId);
}

juce::AudioProcessorValueTreeState::ParameterLayout AgentProcessor::createLayout()
{
    using Range = juce::NormalisableRange<float>;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { trimId, 1 }, "Trim", Range { -24.0f, 24.0f, 0.1f }, 0.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    // -18 dBFS RMS is the usual sweet spot for level-sensitive (analog-modelled) plugins.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { targetId, 1 }, "Target RMS", Range { -30.0f, -6.0f, 0.5f }, -18.0f,
        juce::AudioParameterFloatAttributes().withLabel("dBFS")));

    return layout;
}

bool AgentProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void AgentProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    loudness.prepare(sampleRate, getTotalNumInputChannels());
    learnStats.reset();
    trimGain.reset(sampleRate, 0.05);
    trimGain.setCurrentAndTargetValue(static_cast<float>(ma::dbToGain(trimParam->load())));

    // ~300 ms smoothing for the RMS display, per block.
    displaySmoothing = std::exp(-samplesPerBlock / (0.3 * sampleRate));
    displayRmsMeanSquare = 0.0;
}

void AgentProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = getTotalNumInputChannels();
    const int numSamples = buffer.getNumSamples();
    for (int c = numChannels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear(c, 0, numSamples);
    if (numSamples == 0)
        return;

    const auto* const* input = buffer.getArrayOfReadPointers();

    // Measure the input, before the trim: that is what gain staging corrects.
    double blockSum = 0.0, blockPeak = 0.0;
    for (int c = 0; c < numChannels; ++c)
    {
        const double rms = buffer.getRMSLevel(c, 0, numSamples);
        blockSum += rms * rms;
        blockPeak = std::max(blockPeak, static_cast<double>(buffer.getMagnitude(c, 0, numSamples)));
    }
    const double blockMeanSquare = numChannels > 0 ? blockSum / numChannels : 0.0;
    displayRmsMeanSquare = displaySmoothing * displayRmsMeanSquare + (1.0 - displaySmoothing) * blockMeanSquare;
    inputRms.store(ma::powerToDb(displayRmsMeanSquare));
    inputPeak.store(ma::gainToDb(blockPeak));

    loudness.process(input, numChannels, numSamples);
    inputLufs.store(loudness.shortTermLufs());

    auto state = learnState.load();
    if (state == LearnState::resetRequested)
    {
        learnStats.reset();
        learnState.compare_exchange_strong(state, LearnState::learning);
        state = learnState.load();
    }
    if (state == LearnState::learning)
    {
        learnStats.process(input, numChannels, numSamples);
        learnedRms.store(learnStats.rmsDb());
        learnedSecondsValue.store(static_cast<double>(learnStats.gatedSamples()) / currentSampleRate);
    }

    trimGain.setTargetValue(static_cast<float>(ma::dbToGain(trimParam->load())));
    if (trimGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = trimGain.getNextValue();
            for (int c = 0; c < numChannels; ++c)
                buffer.getWritePointer(c)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain(0, numSamples, trimGain.getTargetValue());
    }

    double outPeak = 0.0;
    for (int c = 0; c < numChannels; ++c)
        outPeak = std::max(outPeak, static_cast<double>(buffer.getMagnitude(c, 0, numSamples)));
    outputPeak.store(ma::gainToDb(outPeak));
}

void AgentProcessor::startLearning()
{
    learnedRms.store(-INFINITY);
    learnedSecondsValue.store(0.0);
    learnState.store(LearnState::resetRequested);
}

bool AgentProcessor::stopLearningAndApply()
{
    learnState.store(LearnState::idle);

    const double measured = learnedRms.load();
    if (!std::isfinite(measured))
        return false;

    const auto target = parameters.getRawParameterValue(targetId)->load();
    const auto trim = static_cast<float>(ma::suggestTrimDb(measured, target));

    auto* param = parameters.getParameter(trimId);
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->convertTo0to1(trim));
    param->endChangeGesture();
    return true;
}

void AgentProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void AgentProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* AgentProcessor::createEditor()
{
    return new AgentEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AgentProcessor();
}

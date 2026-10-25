#include "AgentProcessor.h"

#include "AgentEditor.h"
#include "MaUi.h"
#include "ma/Decibels.h"
#include "ma/EqualLoudness.h"
#include "ma/GainStaging.h"

#include <cmath>

namespace
{
const juce::Identifier profileProperty { "profile" };
}

AgentProcessor::AgentProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "AgentState", createLayout())
{
    trimParam = parameters.getRawParameterValue(trimId);
    for (size_t b = 0; b < kBands; ++b)
    {
        learnProfile[b].store(-INFINITY);
        profile[b].store(-INFINITY);
        liveBands[b].store(-INFINITY);
    }
    startTimerHz(10);
}

AgentProcessor::~AgentProcessor()
{
    stopTimer();
}

juce::StringArray AgentProcessor::roleNames()
{
    using ma::ui::pl;
    return { pl("Inny"), pl("Stopa"), pl("Bas"), pl("Perkusja"), pl("Wokal"), pl("Lead"),
             pl("Pad"), pl("Efekty"), pl("Bus"), pl("Wysyłka (pogłos, delay)") };
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
        juce::ParameterID { targetId, 1 }, "Target level", Range { -30.0f, -6.0f, 0.5f }, -18.0f,
        juce::AudioParameterFloatAttributes().withLabel("dB")));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { measureId, 2 }, "Measure", juce::StringArray { "RMS", "LUFS" }, 0));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { peakCeilingId, 2 }, "Peak ceiling", Range { -12.0f, 0.0f, 0.5f }, -1.0f,
        juce::AudioParameterFloatAttributes().withLabel("dBTP")));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { learnTimeId, 2 }, "Learn time", Range { 2.0f, 30.0f, 1.0f }, 8.0f,
        juce::AudioParameterFloatAttributes().withLabel("s")));

    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { roleId, 2 }, "Role", roleNames(), 0));

    // 1 = most important (e.g. the kick in house), 10 = gives way first.
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { priorityId, 2 }, "Priority", 1, 10, 5));

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
    const int channels = getTotalNumInputChannels();
    learnAnalyzer.prepare(sampleRate, channels);
    liveLoudness.prepare(sampleRate, channels);
    liveSpectrum.prepare(sampleRate, ma::BandScale::erb);
    liveWeights = ma::equalLoudnessWeights(liveSpectrum);
    trimGain.reset(sampleRate, 0.05);
    trimGain.setCurrentAndTargetValue(static_cast<float>(ma::dbToGain(trimParam->load())));

    // ~300 ms smoothing for the RMS display, per block.
    displaySmoothing = std::exp(-samplesPerBlock / (0.3 * sampleRate));
    displayRmsMeanSquare = 0.0;
}

bool AgentProcessor::measuresLufs() const
{
    return parameters.getRawParameterValue(measureId)->load() > 0.5f;
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

    // Live meters on the input, before the trim: that is what gain staging corrects.
    double blockSum = 0.0;
    for (int c = 0; c < numChannels; ++c)
    {
        const double rms = buffer.getRMSLevel(c, 0, numSamples);
        blockSum += rms * rms;
    }
    const double blockMeanSquare = numChannels > 0 ? blockSum / numChannels : 0.0;
    displayRmsMeanSquare = displaySmoothing * displayRmsMeanSquare + (1.0 - displaySmoothing) * blockMeanSquare;
    inputRms.store(ma::powerToDb(displayRmsMeanSquare));

    liveLoudness.process(input, numChannels, numSamples);
    inputLufs.store(liveLoudness.shortTermLufs());

    if (spectrumVisible.load())
    {
        liveSpectrum.process(input, numChannels, numSamples);
        auto bands = liveSpectrum.recentDb();
        ma::applyWeights(bands, liveWeights, liveSpectrum.numBands());
        for (size_t b = 0; b < kBands; ++b)
            liveBands[b].store(bands[b]);
    }

    // Learning: the full analysis runs only while learning, so idle Agents stay cheap.
    auto state = learnState.load();
    if (state == LearnState::resetRequested)
    {
        learnAnalyzer.reset();
        if (learnState.compare_exchange_strong(state, LearnState::learning))
            state = LearnState::learning;
    }
    if (state == LearnState::learning)
    {
        learnAnalyzer.process(input, numChannels, numSamples);
        learnMeasured.store(measuresLufs() ? learnAnalyzer.integratedLufs() : learnAnalyzer.rmsDb());
        learnTruePeak.store(learnAnalyzer.truePeakDb());
        const auto bands = learnAnalyzer.perceivedSpectrumDb();
        for (size_t b = 0; b < kBands; ++b)
            learnProfile[b].store(bands[b]);

        const double seconds = learnAnalyzer.gatedSeconds();
        learnSeconds.store(seconds);
        if (seconds >= parameters.getRawParameterValue(learnTimeId)->load())
            learnState.compare_exchange_strong(state, LearnState::finished);
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

bool AgentProcessor::isLearning() const
{
    const auto state = learnState.load();
    return state == LearnState::resetRequested || state == LearnState::learning;
}

void AgentProcessor::startLearning()
{
    learnMeasured.store(-INFINITY);
    learnTruePeak.store(-INFINITY);
    learnSeconds.store(0.0);
    learnState.store(LearnState::resetRequested);
}

void AgentProcessor::stopLearning()
{
    // Stopped by hand: apply what was measured so far (also if it just finished by itself).
    auto state = learnState.exchange(LearnState::idle);
    if (state == LearnState::learning || state == LearnState::finished)
        applyLearned();
}

void AgentProcessor::timerCallback()
{
    auto state = LearnState::finished;
    if (learnState.compare_exchange_strong(state, LearnState::idle))
        applyLearned();
}

void AgentProcessor::applyLearned()
{
    if (std::isfinite(learnMeasured.load()))
    {
        for (size_t b = 0; b < kBands; ++b)
            profile[b].store(learnProfile[b].load());
        profileValid.store(true);
    }

    const double measured = learnMeasured.load();
    const double peak = learnTruePeak.load();
    const double target = parameters.getRawParameterValue(targetId)->load();
    const double ceiling = parameters.getRawParameterValue(peakCeilingId)->load();
    const auto plan = ma::planTrim(measured, peak, target, ceiling);

    result.serial += 1;
    result.applied = plan.valid;
    result.limitedByPeak = plan.limitedByPeak;
    result.measuredDb = measured;
    result.truePeakDb = peak;
    result.trimDb = plan.trimDb;

    if (!plan.valid)
        return;

    auto* param = parameters.getParameter(trimId);
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(plan.trimDb)));
    param->endChangeGesture();
}

AgentProcessor::LearnResult AgentProcessor::lastResult() const
{
    return result;
}

void AgentProcessor::copyLiveSpectrum(double* levelsDb, double* centresHz) const
{
    for (int b = 0; b < numSpectrumBands(); ++b)
    {
        levelsDb[b] = liveBands[static_cast<size_t>(b)].load();
        centresHz[b] = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
    }
}

void AgentProcessor::copyProfile(double* levelsDb, double* centresHz) const
{
    for (int b = 0; b < numSpectrumBands(); ++b)
    {
        levelsDb[b] = profile[static_cast<size_t>(b)].load();
        centresHz[b] = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
    }
}

void AgentProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();

    // The learned profile goes with the project, as dB values separated by spaces.
    if (profileValid.load())
    {
        juce::StringArray values;
        for (int b = 0; b < numSpectrumBands(); ++b)
        {
            const double db = profile[static_cast<size_t>(b)].load();
            values.add(std::isfinite(db) ? juce::String(db, 2) : "-inf");
        }
        state.setProperty(profileProperty, values.joinIntoString(" "), nullptr);
    }
    else
    {
        state.removeProperty(profileProperty, nullptr);
    }

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void AgentProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || !xml->hasTagName(parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml(*xml);
    const auto values = juce::StringArray::fromTokens(state.getProperty(profileProperty).toString(), " ", {});
    if (values.size() == numSpectrumBands())
    {
        for (int b = 0; b < values.size(); ++b)
            profile[static_cast<size_t>(b)].store(values[b] == "-inf" ? -INFINITY : values[b].getDoubleValue());
        profileValid.store(true);
    }
    parameters.replaceState(state);
}

juce::AudioProcessorEditor* AgentProcessor::createEditor()
{
    return new AgentEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AgentProcessor();
}

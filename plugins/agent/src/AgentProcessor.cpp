#include "AgentProcessor.h"

#include "AgentEditor.h"
#include "MaUi.h"
#include "ma/Decibels.h"
#include "ma/EqualLoudness.h"
#include "ma/GainStaging.h"

#include <cmath>
#include <cstring>

namespace
{
const juce::Identifier profileProperty { "profile" };
const juce::Identifier nameProperty { "name" };

float toShared(double db)
{
    return std::isfinite(db) ? static_cast<float>(db) : ma::bus::kSilentDb;
}
} // namespace

AgentProcessor::AgentProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "AgentState", createLayout())
{
    trimParam = parameters.getRawParameterValue(trimId);
    eqBypassParam = parameters.getRawParameterValue(eqBypassId);
    for (int b = 0; b < ma::kEqBands; ++b)
    {
        auto& p = eqParams[static_cast<size_t>(b)];
        p[0] = parameters.getRawParameterValue(eqParamId(b, "Freq"));
        p[1] = parameters.getRawParameterValue(eqParamId(b, "Gain"));
        p[2] = parameters.getRawParameterValue(eqParamId(b, "Q"));
    }
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
    if (auto* region = session->region())
        ma::bus::releaseSlot(*region, slotIndex, instanceId);
}

juce::StringArray AgentProcessor::roleNames()
{
    return ma::ui::roleNames();
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

    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { eqBypassId, 3 }, "Brain EQ bypass", false));
    const float defaultHz[] = { 100.0f, 300.0f, 1000.0f, 4000.0f };
    for (int b = 0; b < ma::kEqBands; ++b)
    {
        const auto name = "Brain EQ " + juce::String(b + 1) + " ";
        Range freq { 20.0f, 20000.0f, 1.0f };
        freq.setSkewForCentre(1000.0f);
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { eqParamId(b, "Freq"), 3 }, name + "freq", freq,
                                                               defaultHz[b], juce::AudioParameterFloatAttributes().withLabel("Hz")));
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { eqParamId(b, "Gain"), 3 }, name + "gain",
                                                               Range { -12.0f, 6.0f, 0.1f }, 0.0f,
                                                               juce::AudioParameterFloatAttributes().withLabel("dB")));
        Range q { 0.3f, 8.0f, 0.01f };
        q.setSkewForCentre(1.0f);
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { eqParamId(b, "Q"), 3 }, name + "Q", q, 1.0f));
    }

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
    eq.prepare(sampleRate);
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

    if (spectrumVisible.load() || brainWantsSpectra.load())
    {
        liveSpectrum.process(input, numChannels, numSamples);
        const auto bands = liveSpectrum.recentDb();
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

    // While the Brain compares before/after, play the settings from before the last apply.
    const bool before = useBackup.load();
    const float trimDb = before ? backupTrimAudio.load() : trimParam->load();
    trimGain.setTargetValue(static_cast<float>(ma::dbToGain(trimDb)));
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

    ma::EqSettings bands {};
    const bool bypass = !before && eqBypassParam->load() > 0.5f;
    for (int b = 0; b < ma::kEqBands && !bypass; ++b)
    {
        auto& band = bands[static_cast<size_t>(b)];
        if (before)
        {
            const auto& saved = backupEqAudio[static_cast<size_t>(b)];
            band = { saved[0].load(), saved[1].load(), saved[2].load() };
        }
        else
        {
            const auto& p = eqParams[static_cast<size_t>(b)];
            band = { p[0]->load(), p[1]->load(), p[2]->load() };
        }
    }
    eq.setBands(bands);
    eq.process(buffer.getArrayOfWritePointers(), numChannels, numSamples);

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
    publishToBrain();
}

void AgentProcessor::publishToBrain()
{
    auto* region = session->region();
    if (region == nullptr)
        return;

    const auto now = ma::SharedSession::nowMs();
    brainAlive.store(ma::bus::isAlive(region->header.brainHeartbeatMs.load(), now));
    brainWantsSpectra.store(now < region->header.spectraWantedUntil.load());

    if (slotIndex < 0 || region->slots[slotIndex].owner.load() != instanceId)
        slotIndex = ma::bus::claimSlot(*region, instanceId, now);
    if (slotIndex < 0)
        return;
    handleBrainCommand(*region);

    ma::bus::AgentData data {};
    const auto name = getDisplayName().toRawUTF8();
    std::strncpy(data.name, name, sizeof(data.name) - 1);
    data.role = static_cast<std::int32_t>(parameters.getRawParameterValue(roleId)->load());
    data.priority = static_cast<std::int32_t>(parameters.getRawParameterValue(priorityId)->load());
    data.measureLufs = measuresLufs() ? 1 : 0;
    data.hasProfile = profileValid.load() ? 1 : 0;
    data.trimLimited = result.applied && result.limitedByPeak ? 1 : 0;
    data.trimDb = trimParam->load();
    data.targetDb = parameters.getRawParameterValue(targetId)->load();
    data.inputRmsDb = toShared(inputRms.load());
    data.inputLufs = toShared(inputLufs.load());
    data.outputPeakDb = toShared(outputPeak.load());
    data.learnedDb = toShared(result.applied ? result.measuredDb : -INFINITY);
    data.learnedTruePeakDb = toShared(result.applied ? result.truePeakDb : -INFINITY);
    data.liveValid = spectrumVisible.load() || brainWantsSpectra.load() ? 1 : 0;

    // The profile is stored ear-weighted; the Brain gets raw levels like the live spectrum.
    const auto& weights = ma::erbEqualLoudnessWeights();
    for (int b = 0; b < ma::bus::kBands; ++b)
    {
        data.liveDb[b] = toShared(liveBands[static_cast<size_t>(b)].load());
        data.profileDb[b] = toShared(profile[static_cast<size_t>(b)].load() + weights[static_cast<size_t>(b)]);
    }
    const auto current = currentEq();
    for (int b = 0; b < ma::kEqBands; ++b)
        data.eq[b] = current[static_cast<size_t>(b)];
    data.eqBypass = eqBypassed() ? 1 : 0;
    data.hasBackup = backupValid ? 1 : 0;
    data.lastCommand = lastCommand;
    ma::bus::publish(region->slots[slotIndex], data, now);
}

void AgentProcessor::handleBrainCommand(ma::bus::Region& region)
{
    useBackup.store(backupValid && brainAlive.load() && region.header.compareBefore.load() != 0);

    ma::bus::Command command {};
    if (slotIndex < 0 || !ma::bus::readCommand(region.slots[slotIndex], command))
        return;
    if (command.target != instanceId || command.serial == lastCommand)
        return;
    lastCommand = command.serial;

    if (command.kind == ma::bus::CommandKind::apply)
    {
        backupTrimDb = trimParam->load();
        backupEq = currentEq();
        backupBypass = eqBypassed();
        backupValid = true;
        backupTrimAudio.store(backupTrimDb);
        for (int b = 0; b < ma::kEqBands; ++b)
        {
            const auto& band = backupEq[static_cast<size_t>(b)];
            auto& saved = backupEqAudio[static_cast<size_t>(b)];
            saved[0].store(backupBypass ? 1000.0f : band.hz);
            saved[1].store(backupBypass ? 0.0f : band.gainDb);
            saved[2].store(backupBypass ? 1.0f : band.q);
        }

        ma::EqSettings next {};
        for (int b = 0; b < ma::kEqBands; ++b)
            next[static_cast<size_t>(b)] = command.eq[b];
        setParam(trimId, command.trimDb);
        setEqParams(next);
        setParam(eqBypassId, 0.0f);
    }
    else if (command.kind == ma::bus::CommandKind::undo && backupValid)
    {
        setParam(trimId, backupTrimDb);
        setEqParams(backupEq);
        setParam(eqBypassId, backupBypass ? 1.0f : 0.0f);
        backupValid = false;
        useBackup.store(false);
    }
}

void AgentProcessor::setParam(const juce::String& id, float value)
{
    if (auto* param = parameters.getParameter(id))
        param->setValueNotifyingHost(param->convertTo0to1(value));
}

void AgentProcessor::setEqParams(const ma::EqSettings& settings)
{
    for (int b = 0; b < ma::kEqBands; ++b)
    {
        const auto& band = settings[static_cast<size_t>(b)];
        if (ma::isOn(band))
        {
            setParam(eqParamId(b, "Freq"), band.hz);
            setParam(eqParamId(b, "Q"), band.q);
        }
        setParam(eqParamId(b, "Gain"), band.gainDb);
    }
}

ma::EqSettings AgentProcessor::currentEq() const
{
    ma::EqSettings out {};
    for (int b = 0; b < ma::kEqBands; ++b)
    {
        const auto& p = eqParams[static_cast<size_t>(b)];
        out[static_cast<size_t>(b)] = { p[0]->load(), p[1]->load(), p[2]->load() };
    }
    return out;
}

bool AgentProcessor::eqBypassed() const
{
    return eqBypassParam->load() > 0.5f;
}

void AgentProcessor::clearEq()
{
    for (int b = 0; b < ma::kEqBands; ++b)
        setParam(eqParamId(b, "Gain"), 0.0f);
}

void AgentProcessor::updateTrackProperties(const TrackProperties& properties)
{
    // Hosts may call this off the message thread; hop over to keep the strings single-threaded.
    juce::MessageManager::callAsync([safe = juce::WeakReference<AgentProcessor>(this), name = properties.name] {
        if (safe != nullptr)
            safe->trackName = name;
    });
}

juce::String AgentProcessor::getUserName() const
{
    return userName;
}

void AgentProcessor::setUserName(const juce::String& name)
{
    userName = name.trim();
}

juce::String AgentProcessor::getDisplayName() const
{
    if (userName.isNotEmpty())
        return userName;
    if (trackName.isNotEmpty())
        return trackName;
    return ma::ui::pl("Kanał ") + juce::String(slotIndex + 1);
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
    const auto& weights = ma::erbEqualLoudnessWeights();
    for (int b = 0; b < numSpectrumBands(); ++b)
    {
        levelsDb[b] = liveBands[static_cast<size_t>(b)].load() - weights[static_cast<size_t>(b)];
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
    state.setProperty(nameProperty, userName, nullptr);

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
    userName = state.getProperty(nameProperty).toString();
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

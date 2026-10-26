#include "AgentProcessor.h"

#include "AgentEditor.h"
#include "MaUi.h"
#include "ma/Decibels.h"
#include "ma/EqualLoudness.h"
#include "ma/GainStaging.h"

#include <algorithm>
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

// The dynamics as a flat list, for the audio-thread copy of the backup.
std::array<float, 16> packDynamics(const AgentProcessor::DynamicState& s)
{
    const auto& d = s.duck;
    const auto& h = s.harmonics;
    const auto& t = s.transients;
    return { d.on, d.wholeSignal, d.hz, d.q, d.depthDb, d.thresholdDb, d.attackMs, d.releaseMs,
             static_cast<float>(s.sync), s.polarityInverted ? 1.0f : 0.0f, h.on, h.hz, h.amountDb, t.on, t.hz, t.amountDb };
}

void storeMax(std::atomic<float>& target, float value)
{
    auto current = target.load();
    while (value > current && !target.compare_exchange_weak(current, value))
    {
    }
}
} // namespace

AgentProcessor::AgentProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)),
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
        outputBands[b].store(-INFINITY);
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

juce::StringArray AgentProcessor::duckModeNames()
{
    return { ma::ui::pl("Pasmo"), ma::ui::pl("Cały kanał") };
}

juce::StringArray AgentProcessor::duckSyncNames()
{
    return { ma::ui::pl("Ręcznie"), "1/32", "1/16", "1/8", "1/4" };
}

juce::StringArray AgentProcessor::saturationModeNames()
{
    return { ma::ui::pl("Taśma"), ma::ui::pl("Lampa"), ma::ui::pl("Miękki clip"), ma::ui::pl("Twardy clip"), ma::ui::pl("Fałdowanie") };
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

    const ma::DuckSettings duck {};
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { duckOnId, 4 }, "Duck", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { duckModeId, 4 }, "Duck mode", duckModeNames(), 0));
    Range duckFreq { 20.0f, 2000.0f, 1.0f };
    duckFreq.setSkewForCentre(150.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckFreqId, 4 }, "Duck freq", duckFreq, duck.hz,
                                                           juce::AudioParameterFloatAttributes().withLabel("Hz")));
    Range duckQ { 0.3f, 4.0f, 0.01f };
    duckQ.setSkewForCentre(1.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckQId, 4 }, "Duck Q", duckQ, duck.q));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckDepthId, 4 }, "Duck depth",
                                                           Range { 0.0f, 24.0f, 0.1f }, duck.depthDb,
                                                           juce::AudioParameterFloatAttributes().withLabel("dB")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckThresholdId, 4 }, "Duck threshold",
                                                           Range { -60.0f, 0.0f, 0.5f }, duck.thresholdDb,
                                                           juce::AudioParameterFloatAttributes().withLabel("dB")));
    Range attack { 0.1f, 50.0f, 0.1f };
    attack.setSkewForCentre(5.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckAttackId, 4 }, "Duck attack", attack, duck.attackMs,
                                                           juce::AudioParameterFloatAttributes().withLabel("ms")));
    Range release { 10.0f, 1000.0f, 1.0f };
    release.setSkewForCentre(120.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { duckReleaseId, 4 }, "Duck release", release,
                                                           duck.releaseMs, juce::AudioParameterFloatAttributes().withLabel("ms")));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { duckSyncId, 4 }, "Duck release sync", duckSyncNames(), 0));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { polarityId, 4 }, "Polarity invert", false));

    const ma::HarmonicSettings harmonic {};
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { harmonicsOnId, 5 }, "Harmonics", false));
    Range harmonicFreq { 40.0f, 8000.0f, 1.0f };
    harmonicFreq.setSkewForCentre(200.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { harmonicsFreqId, 5 }, "Harmonics split", harmonicFreq,
                                                           harmonic.hz, juce::AudioParameterFloatAttributes().withLabel("Hz")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { harmonicsAmountId, 5 }, "Harmonics amount",
                                                           Range { -30.0f, 0.0f, 0.5f }, harmonic.amountDb,
                                                           juce::AudioParameterFloatAttributes().withLabel("dB")));

    const ma::TransientSettings emphasis {};
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { attackOnId, 5 }, "Attack emphasis", false));
    Range attackFreq { 500.0f, 8000.0f, 1.0f };
    attackFreq.setSkewForCentre(2500.0f);
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { attackFreqId, 5 }, "Attack emphasis from", attackFreq,
                                                           emphasis.hz, juce::AudioParameterFloatAttributes().withLabel("Hz")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { attackAmountId, 5 }, "Attack emphasis amount",
                                                           Range { 0.0f, 12.0f, 0.5f }, emphasis.amountDb,
                                                           juce::AudioParameterFloatAttributes().withLabel("dB")));

    const ma::SaturationSettings saturation {};
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { satOnId, 6 }, "Saturation", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { satModeId, 6 }, "Saturation mode",
                                                            saturationModeNames(), 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { satDriveId, 6 }, "Saturation drive",
                                                           Range { 0.0f, 24.0f, 0.1f }, saturation.driveDb,
                                                           juce::AudioParameterFloatAttributes().withLabel("dB")));
    layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { satMixId, 6 }, "Saturation mix",
                                                           Range { 0.0f, 100.0f, 1.0f }, 100.0f,
                                                           juce::AudioParameterFloatAttributes().withLabel("%")));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { intentionalDriveId, 6 }, "Intentional overdrive", false));

    return layout;
}

bool AgentProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != out)
        return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto& side = layouts.getChannelSet(true, 1);
        return side.isDisabled() || side == juce::AudioChannelSet::mono() || side == juce::AudioChannelSet::stereo();
    }
    return true;
}

void AgentProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int channels = getMainBusNumInputChannels();
    learnAnalyzer.prepare(sampleRate, channels);
    liveLoudness.prepare(sampleRate, channels);
    liveSpectrum.prepare(sampleRate, ma::BandScale::erb);
    eq.prepare(sampleRate);
    ducker.prepare(sampleRate);
    harmonics.prepare(sampleRate);
    transients.prepare(sampleRate);
    saturator.prepare(sampleRate);
    learnFeatures.prepare(sampleRate);
    outputSpectrum.prepare(sampleRate, ma::BandScale::erb);
    sidechainCorrelation.prepare(sampleRate);
    mainMono.assign(static_cast<size_t>(std::max(samplesPerBlock, 512)), 0.0f);
    sideMono.assign(mainMono.size(), 0.0f);
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

    auto mainBuffer = getBusBuffer(buffer, true, 0);
    const int numChannels = mainBuffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    for (int c = numChannels; c < getMainBusNumOutputChannels(); ++c)
        buffer.clear(c, 0, numSamples);
    if (numSamples == 0)
        return;

    // The bus buffer owns the channel pointer list, so it has to live as long as `sidechain`.
    const bool hasSidechain = getBusCount(true) > 1 && getBus(true, 1)->isEnabled();
    auto sideBuffer = hasSidechain ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<float> {};
    const int numSidechain = sideBuffer.getNumChannels();
    const float* const* sidechain = numSidechain > 0 ? sideBuffer.getArrayOfReadPointers() : nullptr;
    float sidePeak = 0.0f;
    for (int c = 0; c < numSidechain; ++c)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax(sidechain[c], numSamples);
        sidePeak = std::max({ sidePeak, std::abs(range.getStart()), std::abs(range.getEnd()) });
    }
    storeMax(sidechainPeakHold, sidePeak);

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                hostBpm.store(*bpm);

    const auto* const* input = mainBuffer.getArrayOfReadPointers();

    // Live meters on the input, before the trim: that is what gain staging corrects.
    double blockSum = 0.0;
    for (int c = 0; c < numChannels; ++c)
    {
        const double rms = mainBuffer.getRMSLevel(c, 0, numSamples);
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
        learnFeatures.reset();
        if (learnState.compare_exchange_strong(state, LearnState::learning))
            state = LearnState::learning;
    }
    if (state == LearnState::learning)
    {
        const int frames = learnAnalyzer.spectrumAnalyzer().framesAnalysed();
        learnAnalyzer.process(input, numChannels, numSamples);
        learnFeatures.process(input, numChannels, numSamples);
        const auto& analyser = learnAnalyzer.spectrumAnalyzer();
        if (analyser.framesAnalysed() != frames)
            learnFeatures.addSpectrumFrame(analyser.lastFramePower(), analyser.numBands());
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
                mainBuffer.getWritePointer(c)[i] *= g;
        }
    }
    else
    {
        mainBuffer.applyGain(0, numSamples, trimGain.getTargetValue());
    }

    // Dynamics: the live parameters, or the backup while the Brain compares.
    DynamicState dynamics;
    if (before)
    {
        std::array<float, kDynamicValues> v {};
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = backupDynamicsAudio[i].load();
        dynamics.duck = { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] };
        dynamics.sync = static_cast<int>(v[8]);
        dynamics.polarityInverted = v[9] > 0.5f;
        dynamics.harmonics = { v[10], v[11], v[12] };
        dynamics.transients = { v[13], v[14], v[15] };
    }
    else
    {
        dynamics = currentDynamics();
    }
    const double syncFraction = ma::bus::duckSyncFraction(dynamics.sync);
    if (syncFraction > 0.0 && hostBpm.load() > 0.0)
        dynamics.duck.releaseMs = static_cast<float>(ma::syncedReleaseMs(hostBpm.load(), syncFraction));
    releaseInUse.store(dynamics.duck.releaseMs);

    if (dynamics.polarityInverted)
        mainBuffer.applyGain(0, numSamples, -1.0f);

    // How this channel lines up with the sidechain in the low end, as the mix will sum them.
    if (sidechain != nullptr && numChannels > 0)
    {
        const auto chunk = static_cast<int>(mainMono.size());
        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = std::min(chunk, numSamples - start);
            for (int i = 0; i < n; ++i)
            {
                float m = 0.0f, sc = 0.0f;
                for (int c = 0; c < numChannels; ++c)
                    m += mainBuffer.getReadPointer(c)[start + i];
                for (int c = 0; c < numSidechain; ++c)
                    sc += sidechain[c][start + i];
                mainMono[static_cast<size_t>(i)] = m;
                sideMono[static_cast<size_t>(i)] = sc;
            }
            sidechainCorrelation.process(mainMono.data(), sideMono.data(), n);
        }
        lowCorrelation.store(sidechainCorrelation.lowBand());
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
    eq.process(mainBuffer.getArrayOfWritePointers(), numChannels, numSamples);

    // Saturation sees the gain-staged level, so its drive means the same on every channel.
    saturator.setSettings(currentSaturation());
    saturator.process(mainBuffer.getArrayOfWritePointers(), numChannels, numSamples);

    harmonics.setSettings(dynamics.harmonics);
    harmonics.process(mainBuffer.getArrayOfWritePointers(), numChannels, numSamples);
    transients.setSettings(dynamics.transients);
    transients.process(mainBuffer.getArrayOfWritePointers(), numChannels, numSamples);

    ducker.setSettings(dynamics.duck);
    ducker.process(mainBuffer.getArrayOfWritePointers(), numChannels, sidechain, numSidechain, numSamples);
    storeMax(reductionHold, static_cast<float>(ducker.reductionDb()));

    if (brainWantsSpectra.load())
    {
        outputSpectrum.process(mainBuffer.getArrayOfReadPointers(), numChannels, numSamples);
        const auto heard = outputSpectrum.recentDb();
        for (size_t b = 0; b < kBands; ++b)
            outputBands[b].store(heard[b]);
    }

    double outPeak = 0.0;
    for (int c = 0; c < numChannels; ++c)
        outPeak = std::max(outPeak, static_cast<double>(mainBuffer.getMagnitude(c, 0, numSamples)));
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
    updateDynamicMeters();
    publishToBrain();
}

void AgentProcessor::updateDynamicMeters()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const float peak = sidechainPeakHold.exchange(0.0f);
    const double peakDb = ma::gainToDb(peak);
    if (peakDb > -80.0)
        lastSidechainMs = now;
    sidechainSeen = lastSidechainMs > 0.0 && now - lastSidechainMs < 3000.0;
    // Peak hold that falls about 12 dB a second (the timer runs at 10 Hz).
    const double fallen = std::isfinite(sidechainShownDb) ? sidechainShownDb - 1.2 : -INFINITY;
    sidechainShownDb = std::max(peakDb, fallen);
    if (sidechainShownDb < -100.0)
        sidechainShownDb = -INFINITY;
    // Deepest dip over the last second (ten timer ticks), so one kick per beat reads steadily.
    reductionHistory[reductionIndex] = reductionHold.exchange(0.0f);
    reductionIndex = (reductionIndex + 1) % reductionHistory.size();
    reductionShownDb = *std::max_element(reductionHistory.begin(), reductionHistory.end());
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
        data.outputDb[b] = brainWantsSpectra.load() ? toShared(outputBands[static_cast<size_t>(b)].load()) : ma::bus::kSilentDb;
        data.profileDb[b] = toShared(profile[static_cast<size_t>(b)].load() + weights[static_cast<size_t>(b)]);
    }
    const auto current = currentEq();
    for (int b = 0; b < ma::kEqBands; ++b)
        data.eq[b] = current[static_cast<size_t>(b)];
    data.eqBypass = eqBypassed() ? 1 : 0;
    data.hasBackup = backupValid ? 1 : 0;
    data.lastCommand = lastCommand;
    const auto dynamics = currentDynamics();
    data.duck = dynamics.duck;
    data.duckSync = dynamics.sync;
    data.polarityInverted = dynamics.polarityInverted ? 1 : 0;
    data.harmonics = dynamics.harmonics;
    data.transients = dynamics.transients;
    data.saturation = currentSaturation();
    data.intentionalDrive = intentionalDrive() ? 1 : 0;
    data.detectedRole = static_cast<std::int32_t>(result.guess.role);
    data.detectedConfidence = static_cast<float>(result.guess.confidence);
    data.sidechainConnected = sidechainSeen ? 1 : 0;
    data.sidechainDb = toShared(sidechainShownDb);
    data.duckReductionDb = static_cast<float>(reductionShownDb);
    data.sidechainLowCorrelation = static_cast<float>(lowCorrelation.load());
    data.correlationValid = correlationValid() ? 1 : 0;
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
        backupDynamics = currentDynamics();
        backupValid = true;
        const auto packed = packDynamics(backupDynamics);
        for (size_t i = 0; i < packed.size(); ++i)
            backupDynamicsAudio[i].store(packed[i]);
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
        setDynamics({ command.duck, command.duckSync, command.polarityInverted != 0, command.harmonics, command.transients });
    }
    else if (command.kind == ma::bus::CommandKind::adapt && !eqBypassed())
    {
        ma::EqSettings next {};
        for (int b = 0; b < ma::kEqBands; ++b)
            next[static_cast<size_t>(b)] = command.eq[b];
        setEqParams(next);
    }
    else if (command.kind == ma::bus::CommandKind::undo && backupValid)
    {
        setParam(trimId, backupTrimDb);
        setEqParams(backupEq);
        setParam(eqBypassId, backupBypass ? 1.0f : 0.0f);
        setDynamics(backupDynamics);
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

void AgentProcessor::setDynamics(const DynamicState& state)
{
    const auto& d = state.duck;
    setParam(duckOnId, d.on);
    setParam(duckModeId, d.wholeSignal);
    setParam(duckFreqId, d.hz);
    setParam(duckQId, d.q);
    setParam(duckDepthId, d.depthDb);
    setParam(duckThresholdId, d.thresholdDb);
    setParam(duckAttackId, d.attackMs);
    setParam(duckReleaseId, d.releaseMs);
    setParam(duckSyncId, static_cast<float>(state.sync));
    setParam(polarityId, state.polarityInverted ? 1.0f : 0.0f);
    setParam(harmonicsOnId, state.harmonics.on);
    setParam(harmonicsFreqId, state.harmonics.hz);
    setParam(harmonicsAmountId, state.harmonics.amountDb);
    setParam(attackOnId, state.transients.on);
    setParam(attackFreqId, state.transients.hz);
    setParam(attackAmountId, state.transients.amountDb);
}

AgentProcessor::DynamicState AgentProcessor::currentDynamics() const
{
    const auto value = [this](const char* id) { return parameters.getRawParameterValue(id)->load(); };
    DynamicState state;
    state.duck.on = value(duckOnId) > 0.5f ? 1.0f : 0.0f;
    state.duck.wholeSignal = value(duckModeId) > 0.5f ? 1.0f : 0.0f;
    state.duck.hz = value(duckFreqId);
    state.duck.q = value(duckQId);
    state.duck.depthDb = value(duckDepthId);
    state.duck.thresholdDb = value(duckThresholdId);
    state.duck.attackMs = value(duckAttackId);
    state.duck.releaseMs = value(duckReleaseId);
    state.sync = juce::roundToInt(value(duckSyncId));
    state.polarityInverted = value(polarityId) > 0.5f;
    state.harmonics.on = value(harmonicsOnId) > 0.5f ? 1.0f : 0.0f;
    state.harmonics.hz = value(harmonicsFreqId);
    state.harmonics.amountDb = value(harmonicsAmountId);
    state.transients.on = value(attackOnId) > 0.5f ? 1.0f : 0.0f;
    state.transients.hz = value(attackFreqId);
    state.transients.amountDb = value(attackAmountId);
    return state;
}

ma::SaturationSettings AgentProcessor::currentSaturation() const
{
    const auto value = [this](const char* id) { return parameters.getRawParameterValue(id)->load(); };
    ma::SaturationSettings settings;
    settings.on = value(satOnId) > 0.5f ? 1.0f : 0.0f;
    settings.mode = static_cast<float>(juce::roundToInt(value(satModeId)));
    settings.driveDb = value(satDriveId);
    settings.mix = value(satMixId) / 100.0f;
    return settings;
}

bool AgentProcessor::intentionalDrive() const
{
    return parameters.getRawParameterValue(intentionalDriveId)->load() > 0.5f;
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
    // A channel meant to distort may peak where it likes: only the level target counts.
    const double ceiling = intentionalDrive() ? 100.0 : parameters.getRawParameterValue(peakCeilingId)->load();
    const auto plan = ma::planTrim(measured, peak, target, ceiling);

    result.serial += 1;
    result.applied = plan.valid;
    result.limitedByPeak = plan.limitedByPeak;
    result.measuredDb = measured;
    result.truePeakDb = peak;
    result.trimDb = plan.trimDb;
    recogniseRole();

    if (!plan.valid)
        return;

    auto* param = parameters.getParameter(trimId);
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(plan.trimDb)));
    param->endChangeGesture();
}

void AgentProcessor::recogniseRole()
{
    // The learning pass has ended, so the audio thread no longer touches the features.
    // The profile is stored ear-weighted; the recogniser wants raw levels.
    ma::SpectrumAnalyzer::Bands raw {};
    const auto& weights = ma::erbEqualLoudnessWeights();
    for (size_t b = 0; b < kBands; ++b)
        raw[b] = learnProfile[b].load() + weights[b];
    result.guess = ma::recogniseRole(learnFeatures.summary(), raw);
    result.roleSet = result.prioritySet = false;
    if (result.guess.role == ma::Role::other || result.guess.confidence < 0.5)
        return;
    // Only a channel nobody has set up yet: a role chosen by hand always wins.
    if (juce::roundToInt(parameters.getRawParameterValue(roleId)->load()) != static_cast<int>(ma::Role::other))
        return;
    setParam(roleId, static_cast<float>(result.guess.role));
    result.roleSet = true;
    if (juce::roundToInt(parameters.getRawParameterValue(priorityId)->load()) == 5)
    {
        setParam(priorityId, static_cast<float>(ma::defaultPriority(result.guess.role)));
        result.prioritySet = true;
    }
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

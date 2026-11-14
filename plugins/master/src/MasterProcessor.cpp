#include "MasterProcessor.h"

#include "MasterEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kUpdateHz = 10;

float bandRangeDb(const std::array<std::atomic<float>, ma::bus::kBands>& levels, double lowHz, double highHz)
{
    double power = 0.0;
    const int count = ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb);
    for (int b = 0; b < count && b < ma::bus::kBands; ++b)
    {
        const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, b);
        const float db = levels[static_cast<size_t>(b)].load();
        if (hz >= lowHz && hz < highHz && db > -150.0f)
            power += std::pow(10.0, db / 10.0);
    }
    return power > 0.0 ? static_cast<float>(10.0 * std::log10(power)) : -200.0f;
}
} // namespace

MasterProcessor::MasterProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto* set : { &bands, &midBands, &sideBands })
        for (auto& b : *set)
            b.store(-200.0f);
    session = ma::SharedSession::get();
    startTimerHz(kUpdateHz);
}

MasterProcessor::~MasterProcessor()
{
    stopTimer();
}

bool MasterProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono()) && layouts.getMainInputChannelSet() == out;
}

void MasterProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    rate = sampleRate;
    loudness.prepare(sampleRate, 2);
    truePeak.prepare(2);
    spectrum.prepare(sampleRate, ma::BandScale::erb);
    midSpectrum.prepare(sampleRate, ma::BandScale::erb);
    sideSpectrum.prepare(sampleRate, ma::BandScale::erb);
    mid.assign(static_cast<size_t>(std::max(samplesPerBlock, 8192)), 0.0f);
    side.assign(mid.size(), 0.0f);
    analysedSamples = 0.0;
    prepared = true;
}

void MasterProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (!prepared)
        return;
    if (resetRequested.exchange(false))
    {
        loudness.reset();
        truePeak.reset();
        spectrum.reset();
        midSpectrum.reset();
        sideSpectrum.reset();
        analysedSamples = 0.0;
    }
    const int channels = std::min(2, buffer.getNumChannels());
    const int total = buffer.getNumSamples();
    if (channels < 1 || total <= 0)
        return;
    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getReadPointer(channels > 1 ? 1 : 0);
    const float* both[] = { left, right };
    loudness.process(both, 2, total);
    truePeak.process(both, 2, total);
    spectrum.process(both, 2, total);
    for (int start = 0; start < total; start += static_cast<int>(mid.size()))
    {
        const int n = std::min(static_cast<int>(mid.size()), total - start);
        for (int i = 0; i < n; ++i)
        {
            mid[static_cast<size_t>(i)] = 0.5f * (left[start + i] + right[start + i]);
            side[static_cast<size_t>(i)] = 0.5f * (left[start + i] - right[start + i]);
        }
        const float* m[] = { mid.data() };
        const float* s[] = { side.data() };
        midSpectrum.process(m, 1, n);
        sideSpectrum.process(s, 1, n);
    }

    integrated.store(static_cast<float>(loudness.integratedLufs()));
    shortTerm.store(static_cast<float>(loudness.shortTermLufs()));
    peak.store(static_cast<float>(truePeak.truePeakDb()));
    seconds.store(static_cast<float>(spectrum.framesAnalysed() * 2048.0 / rate)); // counted like the Brain counts its own
    const auto all = spectrum.averageDb(), m = midSpectrum.averageDb(), s = sideSpectrum.averageDb();
    for (size_t b = 0; b < bands.size() && b < all.size(); ++b)
    {
        bands[b].store(static_cast<float>(all[b]));
        midBands[b].store(static_cast<float>(m[b]));
        sideBands[b].store(static_cast<float>(s[b]));
    }
}

void MasterProcessor::timerCallback()
{
    auto* region = session != nullptr ? session->region() : nullptr;
    const auto now = ma::SharedSession::nowMs();
    ma::bus::BrainEntry* brain = nullptr;
    if (region != nullptr)
    {
        if (ma::bus::findBrain(*region, sessionId.load(), now) != nullptr && sessionId.load() != 0)
            brainSeenMs = now;
        else if (brainSeenMs == 0)
            brainSeenMs = now;
        const auto chosen = ma::bus::chooseSession(*region, sessionId.load(), ma::SharedSession::processId(), now, now - brainSeenMs);
        if (chosen != sessionId.load())
        {
            sessionId.store(chosen);
            brainSeenMs = now;
            epochKnown = false;
        }
        brain = sessionId.load() != 0 ? ma::bus::findBrain(*region, sessionId.load(), now) : nullptr;
    }
    brainAlive.store(brain != nullptr);

    // The Brain started over (apply, undo, its reset): so do we, to measure the same stretch.
    if (brain != nullptr)
    {
        const auto epoch = brain->resetEpoch.load();
        if (epochKnown && epoch != seenEpoch)
        {
            resetRequested.store(true);
            maxShortTerm = -200.0f;
        }
        seenEpoch = epoch;
        epochKnown = true;
    }
    if (resetRequested.load())
        maxShortTerm = -200.0f;

    const float st = shortTerm.load();
    if (std::isfinite(st) && seconds.load() >= 3.0f)
        maxShortTerm = std::max(maxShortTerm, st);
    ma::bus::MasterValues v;
    v.seconds = seconds.load();
    v.integratedLufs = std::isfinite(integrated.load()) ? integrated.load() : -200.0f;
    v.maxShortTermLufs = maxShortTerm;
    v.truePeakDb = std::isfinite(peak.load()) ? peak.load() : -200.0f;
    const float sideUp = bandRangeDb(sideBands, 150.0, 1e9), midUp = bandRangeDb(midBands, 150.0, 1e9);
    const float sideLow = bandRangeDb(sideBands, 0.0, 120.0), midLow = bandRangeDb(midBands, 0.0, 120.0);
    v.sideDb = sideUp > -150.0f && midUp > -150.0f ? sideUp - midUp : -200.0f;
    v.lowSideDb = midLow > -150.0f ? std::max(-200.0f, sideLow - midLow) : -200.0f;
    for (size_t b = 0; b < bands.size(); ++b)
        v.bandsDb[b] = bands[b].load();
    shown = v;
    if (brain != nullptr)
        ma::bus::publishMaster(*brain, v, now);
}

void MasterProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("MasterCheck");
    state.setProperty("sessionId", juce::String(static_cast<juce::int64>(sessionId.load())), nullptr);
    juce::MemoryOutputStream out(destData, false);
    state.writeToStream(out);
}

void MasterProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData(data, static_cast<size_t>(sizeInBytes));
    if (state.isValid() && state.hasType("MasterCheck"))
        sessionId.store(static_cast<std::uint64_t>(state.getProperty("sessionId").toString().getLargeIntValue()));
}

juce::AudioProcessorEditor* MasterProcessor::createEditor()
{
    return new MasterEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MasterProcessor();
}

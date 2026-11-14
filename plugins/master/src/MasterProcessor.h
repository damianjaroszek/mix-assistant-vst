#pragma once

#include "SharedSession.h"
#include "ma/LoudnessMeter.h"
#include "ma/MixBus.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

// "Kontrola mastera": sits after the mastering chain on the master channel, changes nothing in the
// sound and measures what comes out (loudness, true peak, ERB spectrum, side against mid). Its
// timer writes the numbers into its Brain's entry on the shared bus; the Brain compares them with
// the mix it measures before the chain. Everything restarts when the Brain starts over.
class MasterProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    MasterProcessor();
    ~MasterProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Message thread.
    void requestReset() { resetRequested.store(true); }
    bool brainFound() const { return brainAlive.load(); }
    ma::bus::MasterValues values() const { return shown; }
    // mixcheck: the timer ticked by rendered time.
    void useTestTicks() { stopTimer(); }
    void testTick() { timerCallback(); }

private:
    void timerCallback() override;

    ma::LoudnessMeter loudness;
    ma::TruePeakMeter truePeak;
    ma::SpectrumAnalyzer spectrum, midSpectrum, sideSpectrum;
    std::vector<float> mid, side;
    double rate = 44100.0;
    bool prepared = false;

    std::atomic<bool> resetRequested { false };
    std::atomic<float> integrated { -200.0f }, shortTerm { -200.0f }, peak { -200.0f }, seconds { 0.0f };
    std::array<std::atomic<float>, ma::bus::kBands> bands {}, midBands {}, sideBands {};
    double analysedSamples = 0.0;

    std::shared_ptr<ma::SharedSession> session;
    std::atomic<std::uint64_t> sessionId { 0 };
    std::uint64_t brainSeenMs = 0;
    std::uint32_t seenEpoch = 0;
    bool epochKnown = false;
    std::atomic<bool> brainAlive { false };
    float maxShortTerm = -200.0f;
    ma::bus::MasterValues shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterProcessor)
};

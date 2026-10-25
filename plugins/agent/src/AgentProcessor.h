#pragma once

#include "ma/LevelStats.h"
#include "ma/LoudnessMeter.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

// Agent: measures one mixer channel and applies its gain-staging trim.
// Phase 1 scope: "Learn" measures the gated input RMS over a passage and sets
// the trim so the channel sits at the target level.
class AgentProcessor : public juce::AudioProcessor
{
public:
    AgentProcessor();

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
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Message thread API.
    void startLearning();
    // Stops learning and applies the trim. Returns false if nothing was heard.
    bool stopLearningAndApply();
    bool isLearning() const { return learnState.load() != LearnState::idle; }
    double learnedRmsDb() const { return learnedRms.load(); }
    double learnedSeconds() const { return learnedSecondsValue.load(); }

    double inputRmsDb() const { return inputRms.load(); }
    double inputPeakDb() const { return inputPeak.load(); }
    double inputShortTermLufs() const { return inputLufs.load(); }
    double outputPeakDb() const { return outputPeak.load(); }

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* trimId = "trim";
    static constexpr const char* targetId = "target";

private:
    enum class LearnState { idle, resetRequested, learning };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float>* trimParam = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> trimGain;

    ma::LoudnessMeter loudness;
    ma::LevelStats learnStats;
    double currentSampleRate = 48000.0;
    double displayRmsMeanSquare = 0.0;
    double displaySmoothing = 0.0;

    std::atomic<LearnState> learnState { LearnState::idle };
    std::atomic<double> learnedRms { -INFINITY };
    std::atomic<double> learnedSecondsValue { 0.0 };

    std::atomic<double> inputRms { -INFINITY };
    std::atomic<double> inputPeak { -INFINITY };
    std::atomic<double> inputLufs { -INFINITY };
    std::atomic<double> outputPeak { -INFINITY };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentProcessor)
};

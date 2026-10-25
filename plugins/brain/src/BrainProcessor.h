#pragma once

#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

// Brain: sits last on the master. Phase 0 scope is measuring the mix:
// loudness, true peak, PLR, third-octave spectrum and the pre-master headroom
// check. Talking to the Agents comes in phase 2.
class BrainProcessor : public juce::AudioProcessor
{
public:
    BrainProcessor();

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
    void requestReset() { resetRequested.store(true); }
    double integratedLufs() const { return integrated.load(); }
    double shortTermLufs() const { return shortTerm.load(); }
    double momentaryLufs() const { return momentary.load(); }
    double truePeakDb() const { return truePeak.load(); }
    double bandDb(int band) const { return bands[static_cast<size_t>(band)].load(); }
    float peakCeilingDb() const { return peakCeiling->load(); }

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* peakCeilingId = "peakCeiling";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    std::atomic<float>* peakCeiling = nullptr;

    ma::LoudnessMeter loudness;
    ma::TruePeakMeter truePeakMeter;
    ma::SpectrumAnalyzer spectrum;

    std::atomic<bool> resetRequested { false };
    std::atomic<double> integrated { -INFINITY };
    std::atomic<double> shortTerm { -INFINITY };
    std::atomic<double> momentary { -INFINITY };
    std::atomic<double> truePeak { -INFINITY };
    std::array<std::atomic<double>, ma::SpectrumAnalyzer::kNumBands> bands;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainProcessor)
};

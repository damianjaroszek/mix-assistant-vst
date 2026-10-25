#pragma once

#include "SharedSession.h"
#include "ma/CorrelationMeter.h"
#include "ma/LoudnessMeter.h"
#include "ma/Masking.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

// What the Brain knows about the mix right now. Built on the message thread a few
// times a second from the master measurements and the Agents' published data.
struct MixSnapshot
{
    enum class Status
    {
        good,
        warning,
        problem,
        unknown
    };

    struct Channel
    {
        std::uint64_t owner = 0;
        ma::bus::AgentData data {};
        juce::String name;
        // Raw ERB band levels after the Agent's trim, smoothed over a few seconds.
        std::array<double, ma::bus::kBands> bandsDb {};
        bool spectrumValid = false;
    };

    struct ReportItem
    {
        Status status = Status::unknown;
        juce::String title;
        juce::String detail;
    };

    std::vector<Channel> channels;
    ma::MaskingResult masking;
    std::vector<ReportItem> report;
};

// Brain: sits last on the master. Measures the mix (loudness, true peak, PLR, spectrum,
// stereo correlation), collects what the Agents publish and reports collisions and
// anything that keeps the mix from being correct.
class BrainProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    BrainProcessor();
    ~BrainProcessor() override;

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
    double lowCorrelation() const { return correlationLow.load(); }
    double fullCorrelation() const { return correlationFull.load(); }

    const MixSnapshot& snapshot() const { return mix; }
    // Collision analysis on the learned profiles instead of the live spectra.
    bool usesLearnedProfiles() const { return useProfiles; }
    void setUseLearnedProfiles(bool shouldUse);
    // While an editor is open the Agents are asked to keep their live spectra running.
    void setEditorOpen(bool open) { editorOpen = open; }
    juce::String describeIssue(const ma::MaskingIssue& issue) const;

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* peakCeilingId = "peakCeiling";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void collectChannels(ma::bus::Region& region, std::uint64_t now);
    void buildReport();

    std::atomic<float>* peakCeiling = nullptr;

    ma::LoudnessMeter loudness;
    ma::TruePeakMeter truePeakMeter;
    ma::SpectrumAnalyzer spectrum;
    ma::CorrelationMeter correlation;

    std::atomic<bool> resetRequested { false };
    std::atomic<double> integrated { -INFINITY };
    std::atomic<double> shortTerm { -INFINITY };
    std::atomic<double> momentary { -INFINITY };
    std::atomic<double> truePeak { -INFINITY };
    std::array<std::atomic<double>, ma::SpectrumAnalyzer::kNumBands> bands;
    std::atomic<double> correlationLow { 1.0 };
    std::atomic<double> correlationFull { 1.0 };
    std::atomic<bool> stereo { true };

    // Message thread only.
    std::shared_ptr<ma::SharedSession> session = ma::SharedSession::get();
    MixSnapshot mix;
    bool useProfiles = false;
    bool editorOpen = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainProcessor)
};

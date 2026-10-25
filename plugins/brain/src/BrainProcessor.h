#pragma once

#include "SharedSession.h"
#include "ma/CorrelationMeter.h"
#include "ma/LoudnessMeter.h"
#include "ma/Masking.h"
#include "ma/Solver.h"
#include "ma/TonalBalance.h"
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
        // Raw ERB band levels after the Agent's trim, smoothed over a few seconds,
        // before the Brain's EQ (what the solver works from) ...
        std::array<double, ma::bus::kBands> bandsDb {};
        // ... and with it (what the mix hears).
        std::array<double, ma::bus::kBands> heardDb {};
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
    ma::TonalComparison tonal; // valid once a target is chosen and enough of the mix was heard
};

// The solver's proposal, waiting for the user to pick what to apply.
struct Proposal
{
    struct Item
    {
        ma::SolverChange change;
        std::uint64_t owner = 0;       // eqCut: channel that gets the cut
        std::uint64_t maskedOwner = 0; // eqCut: channel it makes room for
        ma::EqBand band;
        juce::String text;
        bool selected = true;
        bool advisory = false; // advice only, nothing to apply
    };

    std::vector<Item> items;
    std::vector<std::pair<std::uint64_t, ma::EqSettings>> eq; // complete proposed EQ per channel
    double crowdingBefore = 0.0, crowdingAfter = 0.0;
    bool made = false;
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
    // Master spectrum on ERB bands: recent (display) and averaged since the last reset (tonal balance).
    double bandDb(int band) const { return bands[static_cast<size_t>(band)].load(); }
    static int numMasterBands() { return ma::SpectrumAnalyzer::numBandsFor(ma::kTonalScale); }
    ma::Curve averageSpectrum() const;
    double secondsAnalysed() const { return analysedSeconds.load(); }
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

    // Phase 3: propose, apply, undo, compare.
    const Proposal& proposal() const { return currentProposal; }
    void propose();
    void toggleProposalItem(int index);
    int applySelected(); // returns how many Agents got a command
    int undoLast();
    bool canUndo() const;
    void setComparing(bool shouldCompare);
    bool isComparing() const { return comparing; }
    // Loudness matching for the comparison: dB added while playing "before", NaN until measured.
    double compareGainDb() const;

    // Tonal targets built from reference tracks, kept as files in the user's app data folder.
    juce::StringArray targetNames() const;
    const ma::TonalTarget& target() const { return currentTarget; }
    bool selectTarget(const juce::String& name);
    bool createTarget(const juce::String& name);
    void removeLastReference();
    // Capture: reset the measurement, play a reference through the master, then finish.
    void startCapture();
    bool finishCapture(); // false when too little was heard
    void cancelCapture();
    bool isCapturing() const { return capturing; }
    static constexpr double kMinimumSeconds = 10.0;

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* peakCeilingId = "peakCeiling";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void collectChannels(ma::bus::Region& region, std::uint64_t now);
    void buildReport();
    void updateLoudnessMatch(std::uint64_t now);
    void sendCommand(std::uint64_t owner, const ma::bus::Command& command);
    static juce::File targetFolder();
    void saveTarget();

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
    std::array<std::atomic<double>, ma::SpectrumAnalyzer::kMaxBands> bands;
    std::array<std::atomic<double>, ma::SpectrumAnalyzer::kMaxBands> averageBands;
    std::atomic<double> analysedSeconds { 0.0 };
    double hopSeconds = 2048.0 / 48000.0;
    std::atomic<double> correlationLow { 1.0 };
    std::atomic<double> correlationFull { 1.0 };
    std::atomic<bool> stereo { true };
    std::atomic<float> outputGainDb { 0.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGain;

    // Message thread only.
    std::shared_ptr<ma::SharedSession> session = ma::SharedSession::get();
    MixSnapshot mix;
    bool useProfiles = false;
    bool editorOpen = false;
    Proposal currentProposal;
    std::uint32_t commandSerial = static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt());
    bool comparing = false;
    std::uint64_t compareSwitchedMs = 0;
    std::uint64_t resetAtMs = 0; // re-measure once the Agents have switched (they poll at 10 Hz)
    double loudnessBefore = -INFINITY, loudnessAfter = -INFINITY;
    ma::TonalTarget currentTarget;
    bool capturing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainProcessor)
};

#pragma once

#include "SharedSession.h"
#include "ma/ChannelAnalyzer.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

// Agent: measures one mixer channel and applies its gain-staging trim.
//
// "Learn" measures a passage (RMS or LUFS, true peak, ERB spectrum), stops by itself
// after the chosen time and sets the trim so the channel sits at the target level,
// without letting the true peak cross the ceiling. The channel's role, priority and
// learned spectral profile are saved with the project for the Brain (phase 2).
class AgentProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    AgentProcessor();
    ~AgentProcessor() override;

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
    void updateTrackProperties(const TrackProperties& properties) override;

    // Channel name shown in the Brain: the user's own name, else the host's track name.
    juce::String getUserName() const;
    void setUserName(const juce::String& name);
    juce::String getDisplayName() const;
    bool isConnectedToBrain() const { return brainAlive.load(); }

    // Message thread API.
    void startLearning();
    void stopLearning(); // applies what was measured so far
    bool isLearning() const;
    double learningMeasuredDb() const { return learnMeasured.load(); }
    double learningSeconds() const { return learnSeconds.load(); }

    struct LearnResult
    {
        int serial = 0; // increments with every finished learning pass
        bool applied = false;
        bool limitedByPeak = false;
        double measuredDb = 0.0;
        double truePeakDb = 0.0;
        double trimDb = 0.0;
    };
    LearnResult lastResult() const;

    // Live meters (input before the trim, output after it).
    double inputRmsDb() const { return inputRms.load(); }
    double inputShortTermLufs() const { return inputLufs.load(); }
    double outputPeakDb() const { return outputPeak.load(); }

    // The live spectrum is only computed while an editor or the Brain is looking at it.
    void setSpectrumVisible(bool visible) { spectrumVisible.store(visible); }
    void copyLiveSpectrum(double* levelsDb, double* centresHz) const;
    void copyProfile(double* levelsDb, double* centresHz) const; // learned, ear-weighted
    bool hasProfile() const { return profileValid.load(); }
    static int numSpectrumBands() { return ma::SpectrumAnalyzer::numBandsFor(ma::BandScale::erb); }

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* trimId = "trim";
    static constexpr const char* targetId = "target";
    static constexpr const char* measureId = "measure";
    static constexpr const char* peakCeilingId = "peakCeiling";
    static constexpr const char* learnTimeId = "learnTime";
    static constexpr const char* roleId = "role";
    static constexpr const char* priorityId = "priority";

    static juce::StringArray roleNames();

private:
    enum class LearnState { idle, resetRequested, learning, finished };
    static constexpr int kBands = ma::SpectrumAnalyzer::kMaxBands;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void applyLearned();
    bool measuresLufs() const;
    void publishToBrain();

    std::atomic<float>* trimParam = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> trimGain;

    ma::ChannelAnalyzer learnAnalyzer;
    ma::LoudnessMeter liveLoudness;
    ma::SpectrumAnalyzer liveSpectrum;
    double displayRmsMeanSquare = 0.0;
    double displaySmoothing = 0.0;

    std::atomic<LearnState> learnState { LearnState::idle };
    std::atomic<double> learnMeasured { -INFINITY };
    std::atomic<double> learnTruePeak { -INFINITY };
    std::atomic<double> learnSeconds { 0.0 };
    std::array<std::atomic<double>, kBands> learnProfile;

    std::array<std::atomic<double>, kBands> profile;
    std::atomic<bool> profileValid { false };

    std::atomic<bool> spectrumVisible { false };
    std::atomic<bool> brainWantsSpectra { false };
    std::array<std::atomic<double>, kBands> liveBands; // raw dBFS, not ear-weighted

    std::atomic<double> inputRms { -INFINITY };
    std::atomic<double> inputLufs { -INFINITY };
    std::atomic<double> outputPeak { -INFINITY };

    LearnResult result; // message thread only

    // Connection to the Brain (message thread only).
    std::shared_ptr<ma::SharedSession> session = ma::SharedSession::get();
    std::uint64_t instanceId = ma::SharedSession::newInstanceId();
    int slotIndex = -1;
    std::atomic<bool> brainAlive { false };
    juce::String userName, trackName;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AgentProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentProcessor)
};

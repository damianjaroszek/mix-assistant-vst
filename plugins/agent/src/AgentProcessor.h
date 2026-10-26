#pragma once

#include "SharedSession.h"
#include "ma/ChannelAnalyzer.h"
#include "ma/ChannelChain.h"
#include "ma/ChannelEq.h"
#include "ma/CorrelationMeter.h"
#include "ma/Ducker.h"
#include "ma/Harmonics.h"
#include "ma/Instrument.h"
#include "ma/Saturator.h"
#include "ma/Transient.h"
#include "ma/LoudnessMeter.h"
#include "ma/SpectrumAnalyzer.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

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
        // Role recognised from the same passage; set on the channel when it still had none.
        ma::RoleGuess guess {};
        bool roleSet = false, prioritySet = false;
        // Dynamics of the passage for the Brain's engineer pass (pre-trim).
        bool statsValid = false;
        double levelP10 = -200.0, levelP50 = -200.0, levelP95 = -200.0, lowNoteHz = 0.0;
        ma::Resonances resonances {}; // narrow peaks that stood out through the passage
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

    // The Brain's corrections (phase 3): four bells, set by "Zastosuj" in the Brain.
    static constexpr const char* eqBypassId = "eqBypass";
    static juce::String eqParamId(int band, const char* what) { return "eq" + juce::String(band + 1) + what; }
    ma::EqSettings currentEq() const;
    bool eqBypassed() const;
    void clearEq();
    // A/B in the Brain: the Agent plays its settings from before the last "Zastosuj".
    bool isComparingBefore() const { return useBackup.load(); }
    bool hasBackup() const { return backupValid; }

    // Dynamic give-way (phase 4): the channel dips while the sidechain input plays,
    // e.g. the bass in its low band on every kick. Release can follow the host tempo.
    static constexpr const char* duckOnId = "duckOn";
    static constexpr const char* duckModeId = "duckMode";
    static constexpr const char* duckFreqId = "duckFreq";
    static constexpr const char* duckQId = "duckQ";
    static constexpr const char* duckDepthId = "duckDepth";
    static constexpr const char* duckThresholdId = "duckThreshold";
    static constexpr const char* duckAttackId = "duckAttack";
    static constexpr const char* duckReleaseId = "duckRelease";
    static constexpr const char* duckSyncId = "duckSync";
    static constexpr const char* polarityId = "polarity";
    // Overtones for small speakers (phase 5).
    static constexpr const char* harmonicsOnId = "harmonicsOn";
    static constexpr const char* harmonicsFreqId = "harmonicsFreq";
    static constexpr const char* harmonicsAmountId = "harmonicsAmount";
    static constexpr const char* attackOnId = "attackOn";
    static constexpr const char* attackFreqId = "attackFreq";
    static constexpr const char* attackAmountId = "attackAmount";
    // Creative saturation and deliberate overdrive (phase 6). "Celowy przester" tells the
    // Learn pass and the Brain that this channel is meant to distort: no peak limit on the
    // trim, no clipping warnings.
    static constexpr const char* satOnId = "satOn";
    static constexpr const char* satModeId = "satMode";
    static constexpr const char* satDriveId = "satDrive";
    static constexpr const char* satMixId = "satMix";
    static constexpr const char* intentionalDriveId = "intentionalDrive";
    // The mix engineer's chain (phase 7), set by "Ustaw cały miks" in the Brain or by hand.
    static constexpr const char* hpfId = "cleanHpf";
    static constexpr const char* monoBelowId = "cleanMono";
    static constexpr const char* mudFreqId = "mudFreq";
    static constexpr const char* mudGainId = "mudGain";
    static constexpr const char* mudQId = "mudQ";
    // Resonance cuts: frequency, gain, Q per notch. Plain strings, so the audio thread reads them without allocating.
    static constexpr const char* notchIds[3][3] { { "res1Freq", "res1Gain", "res1Q" },
                                                  { "res2Freq", "res2Gain", "res2Q" },
                                                  { "res3Freq", "res3Gain", "res3Q" } };
    static constexpr const char* compOnId = "compOn";
    static constexpr const char* compThresholdId = "compThreshold";
    static constexpr const char* compRatioId = "compRatio";
    static constexpr const char* compAttackId = "compAttack";
    static constexpr const char* compReleaseId = "compRelease";
    static constexpr const char* compKneeId = "compKnee";
    static constexpr const char* compMakeupId = "compMakeup";
    static constexpr const char* clipOnId = "clipOn";
    static constexpr const char* clipCeilingId = "clipCeiling";
    static constexpr const char* clipSoftnessId = "clipSoftness";
    static constexpr const char* evenOnId = "evenOn";
    static constexpr const char* evenAmountId = "evenAmount";
    static constexpr const char* evenRefId = "evenRef";
    double compressorReductionDb() const { return compShownDb; }
    double clipperDb() const { return clipShownDb; }
    double evenerGainDb() const { return evenerGain.load(); }
    double evenerNoteHz() const { return evenerNote.load(); }
    static juce::StringArray duckModeNames();
    static juce::StringArray duckSyncNames();
    static juce::StringArray saturationModeNames();
    ma::SaturationSettings currentSaturation() const;
    bool intentionalDrive() const;

    struct DynamicState
    {
        ma::DuckSettings duck {};
        int sync = 0;
        bool polarityInverted = false;
        ma::HarmonicSettings harmonics {};
        ma::TransientSettings transients {};
        ma::CleanupSettings cleanup {};
        ma::CompressorSettings compressor {};
        ma::ClipperSettings clipper {};
        ma::BassEvenerSettings evener {};
    };
    DynamicState currentDynamics() const;
    bool sidechainConnected() const { return sidechainSeen; }
    double sidechainLevelDb() const { return sidechainShownDb; }
    double duckReductionDb() const { return reductionShownDb; }
    double effectiveReleaseMs() const { return releaseInUse.load(); }
    double hostTempo() const { return hostBpm.load(); }
    double sidechainLowCorrelation() const { return lowCorrelation.load(); }
    bool correlationValid() const { return sidechainSeen && std::isfinite(inputRms.load()) && inputRms.load() > -60.0; }

private:
    enum class LearnState { idle, resetRequested, learning, finished };
    static constexpr int kBands = ma::SpectrumAnalyzer::kMaxBands;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void applyLearned();
    void recogniseRole();
    bool measuresLufs() const;
    void publishToBrain();
    void handleBrainCommand(ma::bus::Region& region);
    void setParam(const juce::String& id, float value);
    void setEqParams(const ma::EqSettings& eq);
    void setDynamics(const DynamicState& state);
    void updateDynamicMeters();

    std::atomic<float>* trimParam = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> trimGain;
    ma::ChannelEq eq;
    std::atomic<float>* eqBypassParam = nullptr;
    std::array<std::array<std::atomic<float>*, 3>, ma::kEqBands> eqParams {}; // freq, gain, q

    ma::Ducker ducker;
    ma::HarmonicEnhancer harmonics;
    ma::TransientEnhancer transients;
    ma::Saturator saturator;
    ma::CleanupFilter cleanup;
    ma::Compressor compressor;
    ma::Clipper clipper;
    ma::BassEvener evener;
    std::atomic<float> compHold { 0.0f }, clipHold { 0.0f }; // dB, max since the timer last read them
    std::atomic<double> evenerGain { 0.0 }, evenerNote { 0.0 };
    std::array<float, 10> compHistory {}, clipHistory {};    // message thread: last second of maxima
    double compShownDb = 0.0, clipShownDb = 0.0;
    ma::InstrumentFeatures learnFeatures; // audio thread while learning, read once it has finished
    ma::SpectrumAnalyzer outputSpectrum;
    std::array<std::atomic<double>, kBands> outputBands; // raw dBFS after the whole chain
    ma::CorrelationMeter sidechainCorrelation; // this channel (L) against the sidechain (R)
    std::vector<float> mainMono, sideMono;     // sized in prepareToPlay
    std::atomic<double> hostBpm { 0.0 };
    std::atomic<double> releaseInUse { 120.0 };
    std::atomic<float> sidechainPeakHold { 0.0f }; // linear, max since the timer last read it
    std::atomic<float> reductionHold { 0.0f };     // dB, max since the timer last read it
    std::atomic<double> lowCorrelation { 0.0 };
    // Message thread only.
    bool sidechainSeen = false;
    double sidechainShownDb = -INFINITY, reductionShownDb = 0.0;
    std::array<float, 10> reductionHistory {};
    size_t reductionIndex = 0;
    double lastSidechainMs = 0.0;

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

    // Settings from before the last applied Brain command, for undo and A/B.
    bool backupValid = false;
    float backupTrimDb = 0.0f;
    ma::EqSettings backupEq {};
    bool backupBypass = false;
    DynamicState backupDynamics {};
    std::uint32_t lastCommand = 0;
    // Audio thread copy of the backup, used while the Brain compares before/after.
    std::atomic<bool> useBackup { false };
    std::atomic<float> backupTrimAudio { 0.0f };
    std::array<std::array<std::atomic<float>, 3>, ma::kEqBands> backupEqAudio {};
    // DuckSettings (8), sync, polarity, harmonics (3), transients (3), cleanup (5), compressor (7), clipper (3), evener (3)
    static constexpr int kDynamicValues = 43;
    std::array<std::atomic<float>, kDynamicValues> backupDynamicsAudio {};

    JUCE_DECLARE_WEAK_REFERENCEABLE(AgentProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentProcessor)
};

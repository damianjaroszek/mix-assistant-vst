#pragma once

#include "SharedSession.h"
#include "ma/ChannelAnalyzer.h"
#include "ma/Stereo.h"
#include "ma/Width.h"
#include "ma/ChannelChain.h"
#include "ma/ChannelEq.h"
#include "ma/CorrelationMeter.h"
#include "ma/Ducker.h"
#include "ma/Harmonics.h"
#include "ma/Instrument.h"
#include "ma/Saturator.h"
#include "ma/Transient.h"
#include "ma/Unmask.h"
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
        double roleOffsetDb = 0.0; // phase 29: the role's place under the target
        double targetDb = 0.0;     // what the trim aimed at (target plus that)
        // Role recognised from the same passage; set on the channel when it still had none.
        ma::RoleGuess guess {};
        bool roleSet = false, prioritySet = false;
        // Hardly played in the passage (under kLearnFloorDb, or active < kLearnMinActiveShare): trim left alone.
        bool barelyPlayed = false;
        bool boostCapped = false; // the role target wanted more than kLearnMaxBoostDb
        double activeShare = 0.0;
        // Dynamics of the passage for the Brain's engineer pass (pre-trim).
        bool statsValid = false;
        double levelP10 = -200.0, levelP50 = -200.0, levelP95 = -200.0, lowNoteHz = 0.0, tailPitchHz = 0.0;
        ma::Resonances resonances {}; // narrow peaks that stood out through the passage
    };
    LearnResult lastResult() const;
    static constexpr double kLearnFloorDb = -60.0;       // a passage measured under this is too quiet to set a trim from
    static constexpr double kLearnMinActiveShare = 0.15; // ... or one the part plays less of than this
    // A part the producer kept far under its role's level (a piano bed, an atmosphere) is lifted at most
    // this much by Learn; the Brain's group levels move it further if the mix needs it.
    static constexpr double kLearnMaxBoostDb = 12.0;

    // Live meters (input before the trim, output after it).
    double inputRmsDb() const { return inputRms.load(); }
    double inputShortTermLufs() const { return inputLufs.load(); }
    double outputPeakDb() const { return outputPeak.load(); }

    // The live spectrum is only computed while an editor or the Brain is looking at it.
    void setSpectrumVisible(bool visible) { spectrumVisible.store(visible); }
    int editorTab = 0; // the editor's last tab, so reopening the window shows it again (message thread only)
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
    static constexpr const char* roleLevelId = "roleLevel"; // phase 29: Learn sets the trim by the role
    // The mix engineer's chain (phase 7), set by "Analizuj" (whole chain) in the Brain or by hand.
    static constexpr const char* hpfId = "cleanHpf";
    static constexpr const char* monoBelowId = "cleanMono";
    static constexpr const char* mudFreqId = "mudFreq";
    static constexpr const char* mudGainId = "mudGain";
    static constexpr const char* mudQId = "mudQ";
    // Resonance cuts: frequency, gain, Q per notch. Plain strings, so the audio thread reads them without allocating.
    // Mid-side EQ (phase 8): bells on the side signal, set by "Analizuj" (whole chain).
    static constexpr const char* sideBellIds[4][3] { { "side1Freq", "side1Gain", "side1Q" },
                                                     { "side2Freq", "side2Gain", "side2Q" },
                                                     { "side3Freq", "side3Gain", "side3Q" },
                                                     { "side4Freq", "side4Gain", "side4Q" } };
    // Phase 25: the widener, a delayed copy of the mid in the side (at -40 dB it is off).
    static constexpr const char* widenAmountId = "widenAmount";
    static constexpr const char* widenLowId = "widenFrom";
    static constexpr const char* widenDelayId = "widenDelay";
    // Phase 26: the width slider (percent: 0 mono, 100 as it is, 200 as wide as stays in phase) and
    // where the low end stays mono. The user's own: never set by the Brain, not in the A/B backup.
    static constexpr const char* widthId = "width";
    static constexpr const char* widthFromId = "widthFrom";
    // What the shared width plan made of the slider (valid once planned).
    struct WidthStatus
    {
        bool planned = false;
        float request = 100.0f;
        ma::WidthSettings settings {};
        double reach = 0.0;
        bool limitedBySum = false;
        double sumLimitHz = 0.0;
        double worstCoherence = 1.0, worstHz = 0.0;
    };
    WidthStatus widthStatus() const { return width; }
    // Live coherence per ERB band for the editor (+1 mono, -1 anti-phase); false while not measured.
    // After the mid-side EQ; `audible` marks bands within 40 dB of the channel's loudest.
    bool copyStereo(double* coherence, bool* audible) const;
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
    // Phase 30: giving way to other channels without a cable, set by "Analizuj" in the Brain.
    static constexpr const char* unmaskIds[ma::kMaxUnmask][5] { { "unmask1On", "unmask1Freq", "unmask1Q", "unmask1Depth", "unmask1Bands" },
                                                                { "unmask2On", "unmask2Freq", "unmask2Q", "unmask2Depth", "unmask2Bands" } };
    struct UnmaskStatus
    {
        bool on = false;
        ma::UnmaskSettings settings {};
        juce::String keyName;   // empty while the key's Agent is not found
        double reductionDb = 0.0; // deepest over the last second
    };
    UnmaskStatus unmaskStatus(int index) const; // message thread
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
        ma::StereoSettings stereo {};
        ma::SaturationSettings saturation {};
        // Phase 30: dips for other channels; the keys (their AgentData::keyId) are not floats, so they
        // stay out of the packed copies and travel next to them.
        std::array<ma::UnmaskSettings, ma::kMaxUnmask> unmask {};
        std::array<std::uint64_t, ma::kMaxUnmask> unmaskKey {};
        std::uint64_t duckKey = 0; // phase 30: the duck's key without a cable
    };
    DynamicState currentDynamics() const;
    // Phase 23: tools the Brain set that are still as it left them, and tools changed by hand since
    // (ma::bus::ToolBit), for the Brain's verdict and this editor's section headers.
    // `manual`: on, but never set by the Brain.
    void toolOrigins(int& brain, int& changed, int& manual) const;
    // Phase 31: what the Brain decided about each tool at its last "Analizuj" (message thread), kept
    // with the project so it still reads after reopening.
    struct BrainNotes
    {
        std::int64_t madeMs = 0; // 0 = the Brain has not analysed this channel yet
        bool wholeMix = false;
        std::array<int, ma::bus::kToolCount> decision {}; // ma::bus::ToolDecision
        std::array<juce::String, ma::bus::kToolCount> text;
    };
    const BrainNotes& brainNotes() const { return brainNotesNow; }
    bool sidechainConnected() const { return sidechainSeen; }
    double sidechainLevelDb() const { return sidechainShownDb; }
    // Phase 30: the Agent the duck listens to without a cable (empty while none, or a cable is in).
    juce::String duckKeyChannel() const { return sidechainSeen ? juce::String() : duckKeyName; }
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
    void planWidthNow(ma::bus::Region& region, std::uint64_t now);
    void updateKeys(ma::bus::Region& region, std::uint64_t now); // phase 30, message thread
    ma::WidthSettings liveWidth() const; // audio thread

    std::atomic<float>* trimParam = nullptr;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> trimGain;
    ma::ChannelEq eq;
    std::atomic<float>* eqBypassParam = nullptr;
    std::array<std::array<std::atomic<float>*, 3>, ma::kEqBands> eqParams {}; // freq, gain, q

    ma::Ducker ducker;
    // Phase 30: the dips, the key this channel publishes for others, and where the keys are.
    ma::Unmasker unmasker;
    ma::KeyFollower keyFollower;
    std::array<std::atomic<std::uint64_t>, ma::kMaxUnmask> unmaskKeyNow {}; // what currentDynamics() reports
    std::array<std::atomic<int>, ma::kMaxUnmask> unmaskSlot {}, backupUnmaskSlot {}; // -1 = not found
    std::atomic<std::uint64_t> duckKeyNow { 0 };
    std::atomic<int> duckSlot { -1 }, backupDuckSlot { -1 };
    std::atomic<bool> cableActive { false }; // a sidechain cable brought signal in the last seconds
    juce::String duckKeyName;                // message thread
    std::atomic<int> audioSlot { -1 };       // this Agent's slot, for the audio thread
    std::atomic<std::uint64_t> keyId { 0 };  // kept in the project
    std::uint64_t keySinceMs = 0;            // message thread
    std::array<std::atomic<float>, ma::kMaxUnmask> unmaskHold {};
    std::array<std::array<float, 10>, ma::kMaxUnmask> unmaskHistory {};
    std::array<double, ma::kMaxUnmask> unmaskShownDb {};
    std::array<juce::String, ma::kMaxUnmask> unmaskKeyName; // message thread
    int keyTick = 0;
    ma::HarmonicEnhancer harmonics;
    ma::TransientEnhancer transients;
    ma::Saturator saturator;
    ma::CleanupFilter cleanup;
    ma::Compressor compressor;
    ma::Clipper clipper;
    ma::BassEvener evener;
    ma::StereoShaper stereoShaper;
    ma::StereoAnalyzer liveStereo; // on the input, while the Brain or the editor looks
    std::array<std::atomic<float>, kBands> liveMid, liveSide;
    std::atomic<bool> stereoSeen { false };
    // Phase 26: the planned width for the audio thread (WidthSettings as floats), and whether the
    // stereo analysis has to run for it (this slider or another Agent's is in use).
    static constexpr int kWidthFloats = 5 + 3 * ma::WidthSettings::kGuards;
    std::array<std::atomic<float>, kWidthFloats> widthFloats {};
    std::atomic<bool> widthWanted { false };
    WidthStatus width;
    int widthTick = 0;
    float widthPlannedFor = -1.0f, widthFromPlannedFor = -1.0f;
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
    // A bus: how far its sources moved since its Learn pass (the Brain's gain), added to the
    // learned levels it publishes. Message thread only.
    double inputShiftDb = 0.0, backupInputShiftDb = 0.0;

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
    // Phase 23: what the Brain last set (as the parameters read right after), and the one before for undo.
    struct BrainSet
    {
        bool valid = false;
        ma::EqSettings eq {};
        DynamicState dynamics {};
    };
    BrainSet brainSet, brainSetBackup;
    BrainNotes brainNotesNow;
    void readBrainNotes(ma::bus::Region& region);
    void rememberBrainSet();
    std::uint32_t lastCommand = 0;
    // Audio thread copy of the backup, used while the Brain compares before/after.
    std::atomic<bool> useBackup { false };
    std::atomic<float> backupTrimAudio { 0.0f };
    std::array<std::array<std::atomic<float>, 3>, ma::kEqBands> backupEqAudio {};
    // DuckSettings (8), sync, polarity, harmonics (3), transients (3), cleanup (5), compressor (7), clipper (3), evener (3)
    // stereo (12 + the widener's 3, phase 25), saturation (4), unmask (2 x 5, phase 30)
    static constexpr int kDynamicValues = 72;
    std::array<std::atomic<float>, kDynamicValues> backupDynamicsAudio {};

    JUCE_DECLARE_WEAK_REFERENCEABLE(AgentProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentProcessor)
};

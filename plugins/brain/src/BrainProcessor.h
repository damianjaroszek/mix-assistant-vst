#pragma once

#include "SharedSession.h"
#include "ma/ChainPlanner.h"
#include "ma/StereoPlanner.h"
#include "ma/StemReference.h"
#include "ma/CorrelationMeter.h"
#include "ma/LoudnessMeter.h"
#include "ma/MasterPreview.h"
#include "ma/KickTuning.h"
#include "ma/Masking.h"
#include "ma/Solver.h"
#include "ma/TonalBalance.h"
#include "ma/Translation.h"
#include "ma/SpectrumAnalyzer.h"
#include "ma/TruePeakMeter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>
#include <memory>
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
        // Raw ERB band levels after the Agent's trim, smoothed over a few seconds, before
        // anything else (what the engineer pass plans the cleanup from) ...
        std::array<double, ma::bus::kBands> rawDb {};
        // ... after the cleanup (high-pass, mud), before the Brain's EQ (what the solver works from) ...
        std::array<double, ma::bus::kBands> bandsDb {};
        // ... and with it (what the mix hears).
        std::array<double, ma::bus::kBands> heardDb {};
        bool spectrumValid = false;
        // Measured after the Agent's whole chain (harmonics and duck included), for translation.
        std::array<double, ma::bus::kBands> outputDb {};
        bool outputValid = false;
        // Mid and side per ERB band after the trim, before the stereo EQ, smoothed (phase 8).
        std::array<double, ma::bus::kBands> midDb {}, sideDb {};
        bool stereoValid = false;
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
    ma::TranslationResult translation; // per channel, as `channels`
    // The sum's mid and side per ERB band, smoothed over a few seconds, and where it is in anti-phase.
    ma::StereoMaster master;
    std::vector<ma::PhaseRange> antiPhase;
    // Phase 9: the mix's groups (drums, bass, vocals, the rest) against stem references, both
    // measured over the loudest part only, so they compare drop to drop.
    ma::StemMeasure stemMeasure;
    ma::StemComparison stems;
    double dropSeconds = 0.0; // how much of the loudest part has been heard since the last reset
    // Phase 12 and 16: the intro, the build and the break (indexed by ma::StemSection) against
    // the references' own, and how much quieter each is than the drop (NaN until both are heard).
    struct Section
    {
        ma::StemMeasure measure;
        ma::StemComparison stems;
        double seconds = 0.0;
        double belowDropDb = NAN;
    };
    std::array<Section, ma::kStemSections> sections {};
    const Section& section(ma::StemSection s) const { return sections[static_cast<size_t>(s)]; }
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

        // Phase 4: besides the solver's EQ changes, a dynamic give-way or a polarity flip.
        enum class Action
        {
            solver,
            duck,
            polarity,
            harmonics,
            transients,
            chain,
            level // phase 9: a stem group's level, on every channel of the group
        };
        // Phase 7: the engineer pass ("Ustaw cały miks") sets the channel's chain.
        enum class ChainTool
        {
            none,
            cleanup,
            compressor,
            clipper,
            evener,
            stereo,
            saturation // phase 13
        };
        ChainTool tool = ChainTool::none; // with Action::solver unused
        ma::CleanupSettings cleanup {};
        ma::CompressorSettings compressor {};
        ma::ClipperSettings clipper {};
        ma::BassEvenerSettings evener {};
        ma::StereoSettings stereo {};
        ma::SaturationSettings saturation {};

        Action action = Action::solver;
        ma::DuckSettings duck {};
        int duckSync = 0;
        ma::HarmonicSettings harmonics {};
        ma::TransientSettings transients {};
        int group = -1;      // level: which ma::StemGroup
        double gainDb = 0.0; // level
    };

    std::vector<Item> items;
    std::vector<std::pair<std::uint64_t, ma::EqSettings>> eq; // complete proposed EQ per channel

    // The engineer's verdict per channel, made with "Ustaw cały miks".
    struct Verdict
    {
        std::uint64_t owner = 0;
        juce::String name, role;
        MixSnapshot::Status status = MixSnapshot::Status::unknown;
        juce::String summary;
        juce::StringArray reasons;
    };
    std::vector<Verdict> verdicts;
    bool wholeMix = false;
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
    void requestReset()
    {
        resetRequested.store(true);
        mix.master.valid = false; // the stereo picture starts over too
        dropResetPending = true;  // and so does the drop
        previewResetRequested.store(true);
        appliedPreviewGain = NAN;
        loudestShortTerm = -INFINITY;
        peakHold.clear();
    }
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
    double tempoBpm() const { return hostBpm.load(); } // 0 until the host reports it

    // Listening through a simulated playback system (phase 5). Not saved with the project,
    // so a forgotten preview never ends up in a render after reopening it.
    static juce::StringArray systemNames();
    void setPreviewSystem(int index) { previewSystem.store(juce::jlimit(0, ma::kPlaybackSystems - 1, index)); }
    int previewSystemIndex() const { return previewSystem.load(); }

    const MixSnapshot& snapshot() const { return mix; }
    // Collision analysis on the learned profiles instead of the live spectra.
    bool usesLearnedProfiles() const { return useProfiles; }
    void setUseLearnedProfiles(bool shouldUse);
    // While an editor is open the Agents are asked to keep their live spectra running.
    void setEditorOpen(bool open) { editorOpen = open; }
    int editorTab = 0; // the editor's last tab, so reopening the window shows it again (message thread only)
    // A collision the Agents already deal with still shows in the averaged spectra: the duck
    // works only while the kick hits, and the solver's cut stops at its limit.
    enum class IssueHandling
    {
        open,
        ducked,    // the masker gives way to the kick in that band, on every hit
        cutAtLimit // the masker already carries the deepest cut the solver sets there
    };
    IssueHandling issueHandling(const ma::MaskingIssue& issue) const;
    juce::String describeIssue(const ma::MaskingIssue& issue) const;
    static juce::String glueText(const ma::ChainPlan& plan, double bpm);
    juce::String saturationText(const ma::ChainPlan& plan) const;
    // Phase 14: the kick's tuning against the key (and the bass's usual note).
    const MixSnapshot::Channel* kickChannel() const;
    ma::KickTuning kickTuning() const;
    juce::String kickTuningText(const ma::KickTuning& tuning) const;
    static juce::String kickNoteText(double hz);

    // Phase 3: propose, apply, undo, compare.
    const Proposal& proposal() const { return currentProposal; }
    void propose();
    // Phase 7: every channel's chain (cleanup, compression, clipper, bass notes) planned with the
    // whole mix in view, then the usual proposal on top of it, plus a verdict per channel.
    void proposeWholeMix();
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

    // Phase 9: references from stems. A set (one per style) holds any number of reference
    // tracks, each loaded as its stems (and, if there is one, the full track for a check of
    // the separation). Kept as .mastems files next to the tonal targets.
    juce::StringArray stemTargetNames() const;
    // The set as compared: moved to the track's key (phase 10) where the key is known.
    const ma::StemTarget& stemTarget() const { return keyedStems; }
    // Phase 12 and 16: how the report and the editor speak of each section.
    struct SectionText
    {
        juce::String title, name, inReferences, waiting, fine, tooLoud, tooQuiet;
    };
    static SectionText sectionText(ma::StemSection section);
    // Phase 12 and 16: the same set's intros, builds and breaks.
    const ma::StemTarget& stemSectionTarget(ma::StemSection s) const { return keyedSectionStems[static_cast<size_t>(s)]; }
    bool selectStemTarget(const juce::String& name);
    bool createStemTarget(const juce::String& name);
    void removeLastStemProfile();
    // Analyses one reference track's stems in the background; false when it cannot start
    // (no set chosen, already busy). The outcome lands in stemMessage().
    bool loadStems(const juce::Array<juce::File>& files);
    bool isAnalysingStems() const { return stemJob != nullptr; }
    double stemProgress() const;
    const juce::String& stemMessage() const { return stemNote; }
    bool stemMessageIsWarning() const { return stemWarning; }
    static juce::String stemGroupName(int group);
    static constexpr double kMinimumDropSeconds = 8.0;

    // Phase 10: the track's key, heard on the master (kept with the project) or set by hand.
    // The stem references are moved to it.
    const ma::KeyEstimate& detectedKey() const { return heardKey; }
    double keySecondsHeard() const { return heardKeySeconds; }
    int keyOverrideIndex() const { return keyOverride; } // -1 = detect
    void setKeyOverride(int index);
    void restartKeyDetection();
    ma::MusicalKey trackKey() const; // unknown until sure (or set by hand)
    static juce::String keyText(ma::MusicalKey key); // "f-moll (4A)"
    static constexpr double kMinimumKeySeconds = 20.0;

    // Phase 11: before Ozone. A master preview (analysis only) brings the mix to the target
    // loudness through a limiter and measures what it has to take off; the report for
    // mastering collects everything the Brain knows into an HTML file.
    static constexpr const char* masterTargetId = "masterTarget";
    float masterTargetLufs() const { return masterTarget->load(); }
    ma::PreviewStats previewStats() const;
    static constexpr double kMinimumPreviewSeconds = 10.0;
    juce::String masteringReportHtml() const;
    bool saveMasteringReport(const juce::File& file) const;

    juce::AudioProcessorValueTreeState parameters;

    static constexpr const char* peakCeilingId = "peakCeiling";
    // Adaptive mode (phase 6): the solver keeps running and moves the Agents' EQ slowly
    // towards its answer, within hard limits. Off by default; switching it off freezes
    // whatever is set.
    static constexpr const char* adaptiveId = "adaptive";
    bool isAdaptive() const { return adaptive->load() > 0.5f; }
    struct AdaptiveStatus
    {
        bool running = false;       // conditions met, the solver is following the mix
        juce::String waitingFor;    // why not, when it is on but holding
        std::uint64_t lastChangeMs = 0;
        int channelsMoved = 0;      // in the last step that changed anything
    };
    const AdaptiveStatus& adaptiveStatus() const { return adaptiveState; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void collectChannels(ma::bus::Region& region, std::uint64_t now);
    void buildReport();
    void updateLoudnessMatch(std::uint64_t now);
    void sendCommand(std::uint64_t owner, const ma::bus::Command& command);
    void proposeDynamics();
    void proposeHarmonics(); // translation steps and harmonics for tonal holes
    static void applyCleanup(MixSnapshot::Channel& channel, const ma::CleanupSettings& cleanup);
    Proposal::Verdict judge(size_t channel, const ma::ChainPlan& plan, const ma::StereoPlan& stereoPlan) const;
    void adaptStep(std::uint64_t now);
    ma::SolverResult solveNow(double headroomGainDb, const ma::StemShapeInput* stems = nullptr) const;
    juce::String kickName() const;
    juce::String stereoCulprit(double lowHz, double highHz) const; // phase 8 report // the channel most likely feeding the sidechains
    static juce::File targetFolder();
    void saveTarget();
    void saveStemTarget();
    void refreshKeyedStems();
    void updateKey();
    void accumulateDrop();
    void finishStemJob();
    // Phase 9: each channel over the loudest part heard since the last reset (raw, after the
    // trim, as power sums over the drop's updates; a channel silent in one counts as zero).
    struct DropSum
    {
        ma::SpectrumAnalyzer::Bands power {}, side {};
    };
    // Phase 12 and 16: the same over a section, with its loudness for the contrast to the drop.
    struct SectionSums
    {
        std::map<std::uint64_t, DropSum> sums;
        long updates = 0;
        double levelSum = 0.0;
        void clear() { *this = {}; }
    };
    // Per stem group, what the drop (or `section`) sounds like through each channel's cleanup (and
    // EQ when `withEq`); `eqs` (per channel) replaces the Agents' EQ, e.g. with a proposal's.
    std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups> stemGroupDb(bool withEq, std::array<bool, ma::kStemGroups>& present,
                                                                         std::array<ma::SpectrumAnalyzer::Bands, ma::kStemGroups>* side,
                                                                         const std::vector<ma::EqSettings>* eqs,
                                                                         const SectionSums* section = nullptr) const;
    void updateSongSections();
    // A group short in a region its channels' cleanup or the Brain's cuts take out on purpose
    // (more than 3 dB of it): the report does not ask for it back.
    bool cutOnPurpose(int group, int region) const;
    bool stemsReady() const;
    ma::StemShapeInput stemShapeInput() const; // per channel of mix.channels

    std::atomic<float>* peakCeiling = nullptr;
    std::atomic<float>* adaptive = nullptr;
    std::atomic<float>* masterTarget = nullptr;

    ma::LoudnessMeter loudness;
    ma::TruePeakMeter truePeakMeter;
    ma::SpectrumAnalyzer spectrum;
    ma::CorrelationMeter correlation;
    ma::StereoAnalyzer masterStereo;
    std::array<std::atomic<float>, ma::SpectrumAnalyzer::kMaxBands> masterMid, masterSide;
    ma::KeyDetector keyDetector;
    std::array<std::atomic<float>, 12> keyChroma, keyBass;
    std::atomic<double> keySeconds { 0.0 };
    std::atomic<bool> keyResetRequested { false };
    ma::MasterPreview masterPreview;
    std::atomic<double> previewGainDb { 0.0 };
    std::atomic<bool> previewResetRequested { false };
    std::atomic<double> previewSeconds { 0.0 }, previewAverage { 0.0 }, previewTypical { 0.0 }, previewMax { 0.0 },
        previewHeavy { 0.0 }, previewLufs { -INFINITY }, previewPeak { -INFINITY };

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
    std::atomic<double> hostBpm { 0.0 };
    std::atomic<int> previewSystem { 0 };
    ma::SystemSimulator simulator;
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
    AdaptiveStatus adaptiveState;
    std::uint64_t lastAdaptMs = 0;
    std::uint64_t playingSinceMs = 0; // adaptive mode: music has played without a break since then
    // Per channel and EQ slot: solver rounds in a row that wanted a band gone, or a new one
    // started (hysteresis, so a collision on the edge does not make bands come and go).
    struct AdaptVotes
    {
        std::array<int, ma::kEqBands> release {}, enter {};
    };
    std::map<std::uint64_t, AdaptVotes> adaptVotes;

    std::map<std::uint64_t, DropSum> drop;
    long dropUpdates = 0;
    double dropLevel = -INFINITY; // short-term loudness of the loudest part so far
    bool dropResetPending = false;
    int dropHoldUpdates = 0;
    ma::StemTarget currentStems; // as saved
    ma::StemTarget keyedStems;   // moved to keyedTo
    std::array<ma::StemTarget, ma::kStemSections> keyedSectionStems;
    // Phase 12: each channel over the break, and the levels that tell drop and break apart.
    SectionSums breakSums;
    double dropDrumsSum = 0.0, dropLevelSum = 0.0;
    // Phase 16: everything heard, per second of the song (the host's position), so the intro
    // (before the first drop) and its build (the 8 s before it) are known once the drop is.
    std::map<int, SectionSums> songBins;
    double firstDropSecond = INFINITY; // where the drop came in, in the song
    std::atomic<double> songSeconds { -1.0 }; // the host's position while playing, else -1
    SectionSums introSums, buildSums;
    bool sectionReady(ma::StemSection s) const;
    const SectionSums& sectionSums(ma::StemSection s) const
    {
        return s == ma::StemSection::intro ? introSums : s == ma::StemSection::build ? buildSums : breakSums;
    }
    ma::MusicalKey keyedTo;
    // Key heard before this session (from the project) plus what the master plays now.
    std::array<double, 12> savedChroma {}, savedBass {};
    double savedKeySeconds = 0.0;
    ma::KeyEstimate heardKey;
    double heardKeySeconds = 0.0;
    int keyOverride = -1;
    double appliedPreviewGain = NAN;
    double loudestShortTerm = -INFINITY;
    std::map<std::uint64_t, float> peakHold; // each channel's highest output peak since the last reset
    void updatePreview();
    juce::String loudestPeakChannel() const;
    juce::String previewVerdict(MixSnapshot::Status& status) const;
    struct StemJob;
    std::unique_ptr<StemJob> stemJob;
    juce::String stemNote;
    bool stemWarning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainProcessor)
};

#pragma once

#include "AgentProcessor.h"
#include "MaUi.h"

#include <juce_audio_processors/juce_audio_processors.h>

// Phase 31: what the Brain decided about each tool at its last "Analizuj", one row per tool: the tool,
// the decision, where it stands now, and why. The whole reason shows as a tooltip over its row.
class BrainDecisionsView : public juce::Component, public juce::TooltipClient
{
public:
    void setState(const AgentProcessor::BrainNotes& notes, int brain, int changed, int manual, bool connected);
    void paint(juce::Graphics&) override;
    juce::String getTooltip() override;

private:
    juce::Rectangle<int> rowArea(int tool) const;
    AgentProcessor::BrainNotes notes;
    int brainBits = 0, changedBits = 0, manualBits = 0;
    bool brainConnected = false;
    juce::int64 shownMinute = -1;
};

class AgentEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AgentEditor(AgentProcessor&);
    ~AgentEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kWidth = 940, kHeight = 600;

private:
    void timerCallback() override;
    void toggleLearning();
    void showResult(const AgentProcessor::LearnResult&);

    AgentProcessor& processor;
    int shownResultSerial = 0;

    juce::Label title, connection;
    juce::TextEditor nameEditor;
    juce::Label roleLabel, priorityLabel;
    juce::ComboBox roleBox;
    juce::Slider prioritySlider;

    juce::Label inputLabel, inputValue, lufsLabel, lufsValue, outputLabel, outputValue;
    ma::ui::LevelBar inputBar, outputBar;

    juce::Label measureLabel, trimLabel, ceilingLabel, learnTimeLabel;
    juce::ComboBox measureBox;
    juce::Slider targetSlider, trimSlider, ceilingSlider, learnTimeSlider;

    juce::TextButton learnButton;
    juce::ToggleButton roleLevelButton; // phase 29
    juce::Label status;

    juce::Label eqLabel, eqStatus;
    ma::ui::EqCurveView eqCurve;
    juce::ToggleButton eqBypassButton;
    juce::TextButton eqClearButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> eqBypassAttachment;

    juce::Label duckLabel, duckStatus;
    juce::Label unmaskLabel, unmaskStatus; // phase 30
    juce::TextButton unmaskOffButton;
    juce::ToggleButton duckOnButton, polarityButton;
    juce::ComboBox duckModeBox, duckSyncBox;
    juce::Label duckFreqLabel, duckDepthLabel, duckThresholdLabel, duckAttackLabel, duckReleaseLabel;
    juce::Slider duckFreqSlider, duckDepthSlider, duckThresholdSlider, duckAttackSlider, duckReleaseSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> duckOnAttachment, polarityAttachment;

    juce::Label harmonicsLabel, harmonicsFreqLabel, harmonicsAmountLabel;
    juce::ToggleButton harmonicsOnButton;
    juce::Slider harmonicsFreqSlider, harmonicsAmountSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> harmonicsOnAttachment;
    juce::Label attackLabel, attackFreqLabel, attackAmountLabel;
    juce::ToggleButton attackOnButton;
    juce::Slider attackFreqSlider, attackAmountSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attackOnAttachment;

    juce::Label satLabel, satDriveLabel, satMixLabel;
    juce::ToggleButton satOnButton, intentionalButton;
    juce::ComboBox satModeBox;
    juce::Slider satDriveSlider, satMixSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> satOnAttachment, intentionalAttachment, roleLevelAttachment;

    // Phase 7: the engineer's chain.
    juce::Label chainTitle, cleanLabel, compLabel, clipLabel, evenLabel;
    // Phase 23: per tool (bit order of ma::bus::ToolBit) whether the Brain set it, it was changed by hand
    // since, or it was set by hand only.
    std::array<juce::Label, ma::bus::kToolCount> origins;
    juce::Label& origin(int bit);
    void updateOrigins();
    juce::Label hpfLabel, monoLabel, mudFreqLabel, mudGainLabel;
    juce::Slider hpfSlider, monoSlider, mudFreqSlider, mudGainSlider;
    juce::ToggleButton compOnButton, clipOnButton, evenOnButton;
    juce::Label compThresholdLabel, compRatioLabel, compAttackLabel, compReleaseLabel, compKneeLabel, compMakeupLabel;
    juce::Slider compThresholdSlider, compRatioSlider, compAttackSlider, compReleaseSlider, compKneeSlider, compMakeupSlider;
    juce::Label clipCeilingLabel, clipSoftnessLabel, evenAmountLabel, evenRefLabel;
    juce::Slider clipCeilingSlider, clipSoftnessSlider, evenAmountSlider, evenRefSlider;
    juce::Label cleanStatus, compStatus, clipStatus, evenStatus;
    juce::Label stereoLabel, stereoStatus;
    // Phase 26: the width slider.
    juce::Label widthLabel, widthFromLabel, widthStatusLabel, widthMonoMark, widthNeutralMark, widthMaxMark;
    juce::Slider widthSlider, widthFromSlider;
    // Phase 33: the pan.
    juce::Label panLabel, panMonoLabel, panStatus;
    // Phase 34: a send's high cut.
    juce::Label highCutLabel;
    juce::Slider highCutSlider;
    juce::Label alignLabel;
    juce::Slider alignSlider;
    juce::Slider panSlider, panMonoSlider;
    ma::ui::CoherenceView stereoView;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compOnAttachment, clipOnAttachment, evenOnAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> chainAttachments;
    void updateChainStatus();

    juce::Label spectrumLabel;
    juce::ToggleButton showProfileButton;
    ma::ui::SpectrumView spectrumView { -84.0 };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SliderAttachment> priorityAttachment, targetAttachment, trimAttachment, ceilingAttachment, learnTimeAttachment;
    std::unique_ptr<ComboAttachment> roleAttachment, measureAttachment, duckModeAttachment, duckSyncAttachment, satModeAttachment;
    std::unique_ptr<SliderAttachment> duckFreqAttachment, duckDepthAttachment, duckThresholdAttachment, duckAttackAttachment,
        duckReleaseAttachment, harmonicsFreqAttachment, harmonicsAmountAttachment,
        attackFreqAttachment, attackAmountAttachment, satDriveAttachment, satMixAttachment;
    void updateDuckStatus();
    void updateUnmaskStatus();

    // Phase 32: the dynamic bands and the vocal rider.
    juce::Label dynamicTitle, dynamicIntro, riderLabel, riderStatus, dynamicDecision;
    std::array<juce::Label, ma::kDynamicBands> dynamicLabels, dynamicStatus;
    std::array<juce::ToggleButton, ma::kDynamicBands> dynamicOnButtons;
    std::array<juce::Label, ma::kDynamicBands> dynamicFreqLabels, dynamicThresholdLabels, dynamicRangeLabels;
    std::array<juce::Slider, ma::kDynamicBands> dynamicFreqSliders, dynamicThresholdSliders, dynamicRangeSliders;
    juce::ToggleButton riderOnButton;
    juce::Label riderTargetLabel, riderRangeLabel;
    juce::Slider riderTargetSlider, riderRangeSlider;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> dynamicButtonAttachments;
    void updateDynamicStatus();
    void updatePanStatus();
    void layoutDynamics(juce::Rectangle<int> area);

    // Fixed window: a header plus one tab of controls at a time.
    ma::ui::Page levelPage, eqPage, tonePage, chainPage, dynamicsPage, stereoPage, decisionsPage;
    BrainDecisionsView decisionsView; // phase 31
    juce::Label toneDecisions;        // phase 31: the Brain's word on the three tone tools, under them
    juce::TooltipWindow tooltipWindow { this, 500 }; // without one no tooltip in the editor ever showed
    ma::ui::Tabs tabs;
    void layoutLevel(juce::Rectangle<int> area);
    void layoutEq(juce::Rectangle<int> area);
    void layoutTone(juce::Rectangle<int> area);
    void layoutChain(juce::Rectangle<int> area);
    void layoutStereo(juce::Rectangle<int> area);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentEditor)
};

#pragma once

#include "AgentProcessor.h"
#include "MaUi.h"

#include <juce_audio_processors/juce_audio_processors.h>

class AgentEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AgentEditor(AgentProcessor&);
    ~AgentEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

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
    juce::Label status;

    juce::Label eqLabel, eqStatus;
    ma::ui::EqCurveView eqCurve;
    juce::ToggleButton eqBypassButton;
    juce::TextButton eqClearButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> eqBypassAttachment;

    juce::Label duckLabel, duckStatus;
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
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> satOnAttachment, intentionalAttachment;

    // Phase 7: the engineer's chain, right column.
    juce::Label chainTitle, cleanLabel, compLabel, clipLabel, evenLabel;
    juce::Label hpfLabel, monoLabel, mudFreqLabel, mudGainLabel;
    juce::Slider hpfSlider, monoSlider, mudFreqSlider, mudGainSlider;
    juce::ToggleButton compOnButton, clipOnButton, evenOnButton;
    juce::Label compThresholdLabel, compRatioLabel, compAttackLabel, compReleaseLabel, compKneeLabel, compMakeupLabel;
    juce::Slider compThresholdSlider, compRatioSlider, compAttackSlider, compReleaseSlider, compKneeSlider, compMakeupSlider;
    juce::Label clipCeilingLabel, clipSoftnessLabel, evenAmountLabel, evenRefLabel;
    juce::Slider clipCeilingSlider, clipSoftnessSlider, evenAmountSlider, evenRefSlider;
    juce::Label cleanStatus, compStatus, clipStatus, evenStatus;
    juce::Label stereoLabel, stereoStatus;
    ma::ui::CoherenceView stereoView;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> compOnAttachment, clipOnAttachment, evenOnAttachment;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> chainAttachments;
    void updateChainStatus();
    void layoutChain(juce::Rectangle<int> area);

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentEditor)
};

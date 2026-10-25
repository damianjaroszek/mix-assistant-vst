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

    juce::Label spectrumLabel;
    juce::ToggleButton showProfileButton;
    ma::ui::SpectrumView spectrumView { -84.0 };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SliderAttachment> priorityAttachment, targetAttachment, trimAttachment, ceilingAttachment, learnTimeAttachment;
    std::unique_ptr<ComboAttachment> roleAttachment, measureAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentEditor)
};

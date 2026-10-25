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

    AgentProcessor& processor;

    juce::Label title, inputLabel, inputValue, lufsLabel, lufsValue, outputLabel, outputValue, status;
    ma::ui::LevelBar inputBar, outputBar;
    juce::TextButton learnButton;
    juce::Slider targetSlider, trimSlider;
    juce::Label targetLabel, trimLabel;

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<Attachment> targetAttachment, trimAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AgentEditor)
};

#pragma once

#include "BrainProcessor.h"
#include "MaUi.h"

#include <juce_audio_processors/juce_audio_processors.h>

class BrainEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BrainEditor(BrainProcessor&);
    ~BrainEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    BrainProcessor& processor;

    juce::Label title;
    juce::Label integratedLabel, integratedValue, shortTermLabel, shortTermValue;
    juce::Label truePeakLabel, truePeakValue, plrLabel, plrValue;
    juce::Label ceilingLabel, headroomStatus;
    juce::Slider ceilingSlider;
    juce::TextButton resetButton;
    ma::ui::SpectrumView spectrumView;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ceilingAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainEditor)
};

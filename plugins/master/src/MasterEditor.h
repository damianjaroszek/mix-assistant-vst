#pragma once

#include "MasterProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

// One fixed-size page: where the plugin goes, whether it reached the Brain, and what it measures.
class MasterEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MasterEditor(MasterProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    MasterProcessor& processor;
    juce::Label title, hint, status, numbers;
    juce::TextButton resetButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterEditor)
};

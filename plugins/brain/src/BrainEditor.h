#pragma once

#include "BrainProcessor.h"
#include "MaUi.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// Third-octave spectrum of the mix, -72 dB to 0 dB per band.
class SpectrumView : public juce::Component
{
public:
    void setBands(const std::array<double, ma::SpectrumAnalyzer::kNumBands>& newBands);
    void paint(juce::Graphics&) override;

private:
    std::array<double, ma::SpectrumAnalyzer::kNumBands> bands {};
};

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
    SpectrumView spectrumView;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ceilingAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainEditor)
};

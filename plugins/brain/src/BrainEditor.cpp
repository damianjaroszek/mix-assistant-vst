#include "BrainEditor.h"

#include <array>
#include <cmath>

using ma::ui::pl;
namespace colours = ma::ui::colours;

namespace
{
void styleLabel(juce::Label& label, const juce::String& text, juce::Colour colour, float size = 14.0f)
{
    label.setText(text, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, colour);
    label.setFont(juce::FontOptions(size));
}
} // namespace

BrainEditor::BrainEditor(BrainProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    styleLabel(title, pl("Mix Assistant Mózg"), colours::text, 18.0f);
    styleLabel(integratedLabel, "LUFS integrated", colours::quiet);
    styleLabel(shortTermLabel, "LUFS short-term", colours::quiet);
    styleLabel(truePeakLabel, "True peak (max)", colours::quiet);
    styleLabel(plrLabel, pl("PLR (szczyt do głośności)"), colours::quiet);
    styleLabel(ceilingLabel, pl("Limit szczytu pre-master"), colours::quiet);
    for (auto* value : { &integratedValue, &shortTermValue, &truePeakValue, &plrValue })
        styleLabel(*value, {}, colours::text);
    styleLabel(headroomStatus, {}, colours::quiet);

    for (auto* label : { &title, &integratedLabel, &integratedValue, &shortTermLabel, &shortTermValue,
                         &truePeakLabel, &truePeakValue, &plrLabel, &plrValue, &ceilingLabel, &headroomStatus })
        addAndMakeVisible(label);

    ceilingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    ceilingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
    ceilingSlider.setColour(juce::Slider::thumbColourId, colours::accent);
    addAndMakeVisible(ceilingSlider);
    ceilingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, BrainProcessor::peakCeilingId, ceilingSlider);

    resetButton.setButtonText(pl("Zeruj pomiar"));
    resetButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible(resetButton);

    addAndMakeVisible(spectrumView);

    setSize(560, 440);
    startTimerHz(20);
}

BrainEditor::~BrainEditor()
{
    stopTimer();
}

void BrainEditor::timerCallback()
{
    const double integrated = processor.integratedLufs();
    const double peak = processor.truePeakDb();

    integratedValue.setText(ma::ui::formatDb(integrated, "LUFS"), juce::dontSendNotification);
    shortTermValue.setText(ma::ui::formatDb(processor.shortTermLufs(), "LUFS"), juce::dontSendNotification);
    truePeakValue.setText(ma::ui::formatDb(peak, "dBTP"), juce::dontSendNotification);
    plrValue.setText(std::isfinite(integrated) && std::isfinite(peak) ? juce::String(peak - integrated, 1) + " dB" : "-",
                     juce::dontSendNotification);

    const double ceiling = processor.peakCeilingDb();
    if (!std::isfinite(peak))
    {
        headroomStatus.setText(pl("Czekam na sygnał..."), juce::dontSendNotification);
        headroomStatus.setColour(juce::Label::textColourId, colours::quiet);
    }
    else if (peak <= ceiling)
    {
        headroomStatus.setText(pl("Zapas na mastering OK: ") + juce::String(ceiling - peak, 1) + pl(" dB poniżej limitu."),
                               juce::dontSendNotification);
        headroomStatus.setColour(juce::Label::textColourId, colours::good);
    }
    else
    {
        headroomStatus.setText(pl("Za mało zapasu: szczyt ") + juce::String(peak - ceiling, 1)
                                   + pl(" dB ponad limitem. Ścisz sumę albo kanały."),
                               juce::dontSendNotification);
        headroomStatus.setColour(juce::Label::textColourId, colours::critical);
    }

    std::array<double, ma::SpectrumAnalyzer::kNumBands> bands {}, centres {};
    for (int b = 0; b < ma::SpectrumAnalyzer::kNumBands; ++b)
    {
        bands[static_cast<size_t>(b)] = processor.bandDb(b);
        centres[static_cast<size_t>(b)] = ma::SpectrumAnalyzer::bandCentreHz(b);
    }
    spectrumView.setBands(bands.data(), centres.data(), ma::SpectrumAnalyzer::kNumBands);
}

void BrainEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void BrainEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto header = area.removeFromTop(28);
    resetButton.setBounds(header.removeFromRight(120));
    title.setBounds(header);
    area.removeFromTop(8);

    const auto row = [&](juce::Label& label, juce::Label& value) {
        auto r = area.removeFromTop(22);
        label.setBounds(r.removeFromLeft(220));
        value.setBounds(r);
    };
    row(integratedLabel, integratedValue);
    row(shortTermLabel, shortTermValue);
    row(truePeakLabel, truePeakValue);
    row(plrLabel, plrValue);

    area.removeFromTop(8);
    auto ceilingRow = area.removeFromTop(26);
    ceilingLabel.setBounds(ceilingRow.removeFromLeft(220));
    ceilingSlider.setBounds(ceilingRow);
    headroomStatus.setBounds(area.removeFromTop(24));

    area.removeFromTop(8);
    spectrumView.setBounds(area);
}

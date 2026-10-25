#include "AgentEditor.h"

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

void styleSlider(juce::Slider& slider)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
    slider.setColour(juce::Slider::thumbColourId, colours::accent);
    slider.setColour(juce::Slider::trackColourId, colours::accent.withAlpha(0.5f));
}
} // namespace

AgentEditor::AgentEditor(AgentProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    styleLabel(title, "Mix Assistant Agent", colours::text, 18.0f);
    styleLabel(inputLabel, pl("Wejście (RMS)"), colours::quiet);
    styleLabel(lufsLabel, "LUFS (3 s)", colours::quiet);
    styleLabel(outputLabel, pl("Wyjście (szczyt)"), colours::quiet);
    styleLabel(targetLabel, pl("Cel RMS"), colours::quiet);
    styleLabel(trimLabel, "Trim", colours::quiet);
    styleLabel(inputValue, {}, colours::text);
    styleLabel(lufsValue, {}, colours::text);
    styleLabel(outputValue, {}, colours::text);
    styleLabel(status, pl("Puść najgęstszy fragment utworu i kliknij „Naucz poziom”."), colours::quiet, 13.0f);
    status.setJustificationType(juce::Justification::topLeft);

    for (auto* label : { &title, &inputLabel, &lufsLabel, &outputLabel, &targetLabel, &trimLabel,
                         &inputValue, &lufsValue, &outputValue, &status })
        addAndMakeVisible(label);

    addAndMakeVisible(inputBar);
    addAndMakeVisible(outputBar);

    learnButton.setButtonText(pl("Naucz poziom"));
    learnButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    learnButton.setColour(juce::TextButton::buttonOnColourId, colours::accent);
    learnButton.onClick = [this] { toggleLearning(); };
    addAndMakeVisible(learnButton);

    styleSlider(targetSlider);
    styleSlider(trimSlider);
    addAndMakeVisible(targetSlider);
    addAndMakeVisible(trimSlider);
    targetAttachment = std::make_unique<Attachment>(processor.parameters, AgentProcessor::targetId, targetSlider);
    trimAttachment = std::make_unique<Attachment>(processor.parameters, AgentProcessor::trimId, trimSlider);

    setSize(460, 330);
    startTimerHz(30);
}

AgentEditor::~AgentEditor()
{
    stopTimer();
}

void AgentEditor::toggleLearning()
{
    if (!processor.isLearning())
    {
        processor.startLearning();
        learnButton.setButtonText(pl("Zakończ i ustaw"));
        learnButton.setToggleState(true, juce::dontSendNotification);
        status.setText(pl("Słucham... puść drop albo refren przez kilka sekund."), juce::dontSendNotification);
        return;
    }

    const double measured = processor.learnedRmsDb();
    const bool applied = processor.stopLearningAndApply();
    learnButton.setButtonText(pl("Naucz poziom"));
    learnButton.setToggleState(false, juce::dontSendNotification);

    if (applied)
    {
        const auto trim = processor.parameters.getRawParameterValue(AgentProcessor::trimId)->load();
        status.setText(pl("Zmierzone RMS: ") + ma::ui::formatDb(measured, "dBFS")
                           + pl(". Ustawiono trim ") + (trim >= 0 ? "+" : "") + juce::String(trim, 1) + " dB.",
                       juce::dontSendNotification);
    }
    else
    {
        status.setText(pl("Nic nie było słychać, trim bez zmian."), juce::dontSendNotification);
    }
}

void AgentEditor::timerCallback()
{
    const double rms = processor.inputRmsDb();
    inputBar.setLevel(rms);
    inputBar.setTarget(processor.parameters.getRawParameterValue(AgentProcessor::targetId)->load());
    inputValue.setText(ma::ui::formatDb(rms, "dBFS"), juce::dontSendNotification);
    lufsValue.setText(ma::ui::formatDb(processor.inputShortTermLufs(), "LUFS"), juce::dontSendNotification);

    const double out = processor.outputPeakDb();
    outputBar.setLevel(out);
    outputValue.setText(ma::ui::formatDb(out, "dBFS"), juce::dontSendNotification);

    if (processor.isLearning())
        status.setText(pl("Słucham: ") + juce::String(processor.learnedSeconds(), 1) + " s, RMS "
                           + ma::ui::formatDb(processor.learnedRmsDb(), "dBFS"),
                       juce::dontSendNotification);
}

void AgentEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void AgentEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    title.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);

    const auto meterRow = [&](juce::Label& label, juce::Label& value, ma::ui::LevelBar* bar) {
        auto row = area.removeFromTop(24);
        label.setBounds(row.removeFromLeft(130));
        value.setBounds(row.removeFromRight(100));
        if (bar != nullptr)
            bar->setBounds(row.reduced(4, 6));
        area.removeFromTop(4);
    };
    meterRow(inputLabel, inputValue, &inputBar);
    meterRow(lufsLabel, lufsValue, nullptr);
    meterRow(outputLabel, outputValue, &outputBar);

    area.removeFromTop(8);
    auto row = area.removeFromTop(26);
    targetLabel.setBounds(row.removeFromLeft(130));
    targetSlider.setBounds(row);
    area.removeFromTop(6);
    row = area.removeFromTop(26);
    trimLabel.setBounds(row.removeFromLeft(130));
    trimSlider.setBounds(row);

    area.removeFromTop(12);
    learnButton.setBounds(area.removeFromTop(32).removeFromLeft(180));
    area.removeFromTop(8);
    status.setBounds(area);
}

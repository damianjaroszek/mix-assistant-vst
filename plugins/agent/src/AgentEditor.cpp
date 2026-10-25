#include "AgentEditor.h"

#include <array>

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

void styleCombo(juce::ComboBox& box, const juce::StringArray& items)
{
    box.addItemList(items, 1);
    box.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    box.setColour(juce::ComboBox::textColourId, colours::text);
}

juce::String signedDb(double db)
{
    return (db >= 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}
} // namespace

AgentEditor::AgentEditor(AgentProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    styleLabel(title, "Mix Assistant Agent", colours::text, 18.0f);
    styleLabel(roleLabel, pl("Rola"), colours::quiet);
    styleLabel(priorityLabel, pl("Priorytet"), colours::quiet);
    styleLabel(inputLabel, pl("Wejście (RMS)"), colours::quiet);
    styleLabel(lufsLabel, "LUFS (3 s)", colours::quiet);
    styleLabel(outputLabel, pl("Wyjście (szczyt)"), colours::quiet);
    styleLabel(measureLabel, pl("Miara i cel"), colours::quiet);
    styleLabel(trimLabel, "Trim", colours::quiet);
    styleLabel(ceilingLabel, pl("Limit szczytu"), colours::quiet);
    styleLabel(learnTimeLabel, pl("Czas nauki"), colours::quiet);
    styleLabel(inputValue, {}, colours::text);
    styleLabel(lufsValue, {}, colours::text);
    styleLabel(outputValue, {}, colours::text);
    styleLabel(status, pl("Puść najgęstszy fragment utworu i kliknij „Naucz poziom”."), colours::quiet, 13.0f);
    status.setJustificationType(juce::Justification::topLeft);
    styleLabel(spectrumLabel, pl("Widmo tak, jak słyszy je ucho"), colours::quiet, 13.0f);
    styleLabel(eqLabel, pl("Korekcja od Mózgu"), colours::quiet);
    styleLabel(eqStatus, {}, colours::quiet, 13.0f);
    eqStatus.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(eqLabel);
    addAndMakeVisible(eqStatus);
    addAndMakeVisible(eqCurve);

    eqBypassButton.setButtonText(pl("Wyłącz"));
    eqBypassButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    addAndMakeVisible(eqBypassButton);
    eqBypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, AgentProcessor::eqBypassId, eqBypassButton);

    eqClearButton.setButtonText(pl("Wyzeruj"));
    eqClearButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    eqClearButton.onClick = [this] { processor.clearEq(); };
    addAndMakeVisible(eqClearButton);

    connection.setJustificationType(juce::Justification::centredRight);
    connection.setFont(juce::FontOptions(13.0f));

    nameEditor.setText(processor.getUserName(), false);
    nameEditor.setTextToShowWhenEmpty(processor.getDisplayName(), colours::quiet);
    nameEditor.setColour(juce::TextEditor::backgroundColourId, colours::panel);
    nameEditor.setColour(juce::TextEditor::textColourId, colours::text);
    nameEditor.setTooltip(pl("Nazwa w Mózgu. Pusta = nazwa ścieżki z FL."));
    nameEditor.onTextChange = [this] { processor.setUserName(nameEditor.getText()); };
    addAndMakeVisible(nameEditor);

    for (auto* label : { &title, &connection, &roleLabel, &priorityLabel, &inputLabel, &lufsLabel, &outputLabel, &measureLabel,
                         &trimLabel, &ceilingLabel, &learnTimeLabel, &inputValue, &lufsValue, &outputValue, &status,
                         &spectrumLabel })
        addAndMakeVisible(label);

    styleCombo(roleBox, AgentProcessor::roleNames());
    styleCombo(measureBox, { "RMS (dBFS)", "LUFS" });
    addAndMakeVisible(roleBox);
    addAndMakeVisible(measureBox);

    for (auto* slider : { &prioritySlider, &targetSlider, &trimSlider, &ceilingSlider, &learnTimeSlider })
    {
        styleSlider(*slider);
        addAndMakeVisible(slider);
    }

    addAndMakeVisible(inputBar);
    addAndMakeVisible(outputBar);

    learnButton.setButtonText(pl("Naucz poziom"));
    learnButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    learnButton.setColour(juce::TextButton::buttonOnColourId, colours::accent);
    learnButton.onClick = [this] { toggleLearning(); };
    addAndMakeVisible(learnButton);

    showProfileButton.setButtonText(pl("Pokaż nauczony profil"));
    showProfileButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    addAndMakeVisible(showProfileButton);
    addAndMakeVisible(spectrumView);

    auto& state = processor.parameters;
    roleAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::roleId, roleBox);
    measureAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::measureId, measureBox);
    priorityAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::priorityId, prioritySlider);
    targetAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::targetId, targetSlider);
    trimAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::trimId, trimSlider);
    ceilingAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::peakCeilingId, ceilingSlider);
    learnTimeAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::learnTimeId, learnTimeSlider);

    shownResultSerial = processor.lastResult().serial;
    processor.setSpectrumVisible(true);
    setSize(520, 720);
    startTimerHz(30);
}

AgentEditor::~AgentEditor()
{
    stopTimer();
    processor.setSpectrumVisible(false);
}

void AgentEditor::toggleLearning()
{
    if (!processor.isLearning())
    {
        processor.startLearning();
        return;
    }
    processor.stopLearning();
}

void AgentEditor::showResult(const AgentProcessor::LearnResult& r)
{
    const juce::String unit = measureBox.getSelectedItemIndex() == 1 ? "LUFS" : "dBFS";
    if (!r.applied)
    {
        status.setText(pl("Nic nie było słychać, trim bez zmian."), juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, colours::quiet);
        return;
    }

    juce::String text = pl("Zmierzone: ") + ma::ui::formatDb(r.measuredDb, unit.toRawUTF8())
                        + ", true peak " + ma::ui::formatDb(r.truePeakDb, "dBTP")
                        + pl(". Ustawiono trim ") + signedDb(r.trimDb) + ".";
    if (r.limitedByPeak)
        text += pl("\nTrim ograniczony, żeby szczyt nie przekroczył limitu. Kanał ma duże szczyty: "
                   "rozważ kompresję albo limiter przed Agentem.");
    status.setText(text, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, r.limitedByPeak ? colours::warning : colours::good);
}

void AgentEditor::timerCallback()
{
    const bool connected = processor.isConnectedToBrain();
    connection.setText(connected ? pl("Połączony z Mózgiem") : pl("Brak Mózgu na masterze"), juce::dontSendNotification);
    connection.setColour(juce::Label::textColourId, connected ? colours::good : colours::quiet);
    if (processor.getUserName().isEmpty())
        nameEditor.setTextToShowWhenEmpty(processor.getDisplayName(), colours::quiet);

    const bool lufs = measureBox.getSelectedItemIndex() == 1;
    const double rms = processor.inputRmsDb();
    inputBar.setLevel(rms);
    inputBar.setTarget(lufs ? -INFINITY : processor.parameters.getRawParameterValue(AgentProcessor::targetId)->load());
    inputValue.setText(ma::ui::formatDb(rms, "dBFS"), juce::dontSendNotification);
    lufsValue.setText(ma::ui::formatDb(processor.inputShortTermLufs(), "LUFS"), juce::dontSendNotification);

    const double out = processor.outputPeakDb();
    outputBar.setLevel(out);
    outputValue.setText(ma::ui::formatDb(out, "dBFS"), juce::dontSendNotification);

    const bool learning = processor.isLearning();
    learnButton.setButtonText(learning ? pl("Zakończ teraz i ustaw") : pl("Naucz poziom"));
    learnButton.setToggleState(learning, juce::dontSendNotification);
    if (learning)
    {
        const double needed = processor.parameters.getRawParameterValue(AgentProcessor::learnTimeId)->load();
        status.setText(pl("Słucham: ") + juce::String(processor.learningSeconds(), 1) + " / " + juce::String(needed, 0)
                           + " s, " + ma::ui::formatDb(processor.learningMeasuredDb(), lufs ? "LUFS" : "dBFS")
                           + pl("\nCisza się nie liczy. Skończę sam."),
                       juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, colours::text);
    }

    const auto result = processor.lastResult();
    if (result.serial != shownResultSerial)
    {
        shownResultSerial = result.serial;
        showResult(result);
    }

    const auto eqSettings = processor.currentEq();
    eqCurve.setEq(eqSettings, processor.eqBypassed());
    juce::StringArray cuts;
    for (const auto& band : eqSettings)
        if (ma::isOn(band))
            cuts.add(juce::String(band.gainDb, 1) + " dB @ " + juce::String(juce::roundToInt(band.hz)) + " Hz");
    if (processor.isComparingBefore())
    {
        eqStatus.setText(pl("Mózg porównuje: słychać wersję sprzed „Zastosuj”."), juce::dontSendNotification);
        eqStatus.setColour(juce::Label::textColourId, colours::warning);
    }
    else
    {
        eqStatus.setText(cuts.isEmpty() ? pl("Brak korekcji. Ustawia je „Zastosuj” w Mózgu.") : cuts.joinIntoString("\n"),
                         juce::dontSendNotification);
        eqStatus.setColour(juce::Label::textColourId, colours::quiet);
    }

    std::array<double, ma::ui::SpectrumView::kMaxBands> levels {}, centres {};
    const bool profile = showProfileButton.getToggleState() && processor.hasProfile();
    if (profile)
        processor.copyProfile(levels.data(), centres.data());
    else
        processor.copyLiveSpectrum(levels.data(), centres.data());
    spectrumView.setBands(levels.data(), centres.data(), AgentProcessor::numSpectrumBands());
    showProfileButton.setEnabled(processor.hasProfile());
}

void AgentEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void AgentEditor::resized()
{
    constexpr int labelWidth = 130;
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(28);
    title.setBounds(top.removeFromLeft(190));
    connection.setBounds(top.removeFromRight(170));
    nameEditor.setBounds(top.reduced(6, 2));
    area.removeFromTop(6);

    auto row = area.removeFromTop(26);
    roleLabel.setBounds(row.removeFromLeft(labelWidth));
    roleBox.setBounds(row.removeFromLeft(170));
    row.removeFromLeft(12);
    priorityLabel.setBounds(row.removeFromLeft(70));
    prioritySlider.setBounds(row);
    area.removeFromTop(10);

    const auto meterRow = [&](juce::Label& label, juce::Label& value, ma::ui::LevelBar* bar) {
        auto r = area.removeFromTop(24);
        label.setBounds(r.removeFromLeft(labelWidth));
        value.setBounds(r.removeFromRight(100));
        if (bar != nullptr)
            bar->setBounds(r.reduced(4, 6));
        area.removeFromTop(2);
    };
    meterRow(inputLabel, inputValue, &inputBar);
    meterRow(lufsLabel, lufsValue, nullptr);
    meterRow(outputLabel, outputValue, &outputBar);
    area.removeFromTop(8);

    row = area.removeFromTop(26);
    measureLabel.setBounds(row.removeFromLeft(labelWidth));
    measureBox.setBounds(row.removeFromLeft(110));
    row.removeFromLeft(8);
    targetSlider.setBounds(row);
    area.removeFromTop(4);

    const auto sliderRow = [&](juce::Label& label, juce::Slider& slider) {
        auto r = area.removeFromTop(26);
        label.setBounds(r.removeFromLeft(labelWidth));
        slider.setBounds(r);
        area.removeFromTop(4);
    };
    sliderRow(trimLabel, trimSlider);
    sliderRow(ceilingLabel, ceilingSlider);
    sliderRow(learnTimeLabel, learnTimeSlider);

    area.removeFromTop(8);
    learnButton.setBounds(area.removeFromTop(32).removeFromLeft(200));
    area.removeFromTop(6);
    status.setBounds(area.removeFromTop(52));

    area.removeFromTop(6);
    row = area.removeFromTop(22);
    eqClearButton.setBounds(row.removeFromRight(80));
    row.removeFromRight(6);
    eqBypassButton.setBounds(row.removeFromRight(80));
    eqLabel.setBounds(row);
    area.removeFromTop(4);
    auto eqRow = area.removeFromTop(90);
    eqStatus.setBounds(eqRow.removeFromRight(170).withTrimmedLeft(10));
    eqCurve.setBounds(eqRow);

    area.removeFromTop(10);
    row = area.removeFromTop(22);
    showProfileButton.setBounds(row.removeFromRight(190));
    spectrumLabel.setBounds(row);
    spectrumView.setBounds(area);
}

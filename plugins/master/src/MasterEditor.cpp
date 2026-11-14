#include "MasterEditor.h"

#include "MaUi.h"

using ma::ui::pl;
namespace colours = ma::ui::colours;

MasterEditor::MasterEditor(MasterProcessor& p) : AudioProcessorEditor(p), processor(p)
{
    const auto style = [this](juce::Label& label, float size, juce::Colour colour) {
        label.setFont(juce::FontOptions(size));
        label.setColour(juce::Label::textColourId, colour);
        label.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(label);
    };
    style(title, 20.0f, colours::text);
    title.setText(pl("Kontrola mastera"), juce::dontSendNotification);
    style(hint, 14.0f, colours::quiet);
    hint.setText(pl("Wstaw tę wtyczkę jako OSTATNI efekt na kanale Master, po Ozone. Niczego nie zmienia w dźwięku: mierzy, co "
                    "wychodzi po masteringu, i wysyła to do Mózgu. Wyniki i propozycje zobaczysz w Mózgu, w raporcie „Po "
                    "masteringu”. Puść drop (albo cały utwór), żeby pomiar się zebrał."),
                 juce::dontSendNotification);
    style(status, 15.0f, colours::quiet);
    style(numbers, 16.0f, colours::text);
    resetButton.setButtonText(pl("Mierz od nowa"));
    resetButton.onClick = [this] { processor.requestReset(); };
    addAndMakeVisible(resetButton);
    setSize(560, 330);
    startTimerHz(5);
    timerCallback();
}

void MasterEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void MasterEditor::resized()
{
    auto area = getLocalBounds().reduced(18);
    auto top = area.removeFromTop(30);
    resetButton.setBounds(top.removeFromRight(150));
    title.setBounds(top);
    area.removeFromTop(6);
    hint.setBounds(area.removeFromTop(92));
    status.setBounds(area.removeFromTop(44));
    numbers.setBounds(area);
}

void MasterEditor::timerCallback()
{
    const bool found = processor.brainFound();
    status.setText(found ? pl("Połączono z Mózgiem tego projektu.")
                         : pl("Nie widzę Mózgu. Wstaw Mózg na kanale Master PRZED Ozone (ta wtyczka idzie po Ozone)."),
                   juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, found ? colours::good : colours::warning);
    const auto v = processor.values();
    juce::String text;
    if (v.seconds < 3.0f || v.integratedLufs < -100.0f)
        text = pl("Czekam na dźwięk: puść utwór.");
    else
    {
        text << pl("Głośność: ") << juce::String(v.integratedLufs, 1) << " LUFS";
        if (v.maxShortTermLufs > -100.0f)
            text << pl(" (najgłośniejsze 3 s: ") << juce::String(v.maxShortTermLufs, 1) << ")";
        text << pl("\nSzczyt: ") << juce::String(v.truePeakDb, 1) << " dBTP" << pl(",  PLR: ") << juce::String(v.truePeakDb - v.integratedLufs, 1) << " dB";
        if (v.sideDb > -100.0f)
            text << pl("\nSzerokość (side do mid od 150 Hz): ") << juce::String(v.sideDb, 1) << " dB";
        if (v.lowSideDb > -100.0f)
            text << pl(",  poniżej 120 Hz: ") << juce::String(v.lowSideDb, 1) << " dB";
        text << pl("\nZmierzono: ") << juce::String(juce::roundToInt(v.seconds)) << " s";
    }
    numbers.setText(text, juce::dontSendNotification);
    numbers.setColour(juce::Label::textColourId, v.truePeakDb > -0.1f && v.seconds >= 3.0f ? colours::warning : colours::text);
}

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

namespace
{
juce::Colour decisionColour(int decision)
{
    switch (static_cast<ma::bus::ToolDecision>(decision))
    {
        case ma::bus::ToolDecision::proposed: return colours::accent;
        case ma::bus::ToolDecision::kept: return colours::good;
        case ma::bus::ToolDecision::notChecked: return colours::warning;
        case ma::bus::ToolDecision::notNeeded:
        case ma::bus::ToolDecision::yours:
        case ma::bus::ToolDecision::none: break;
    }
    return colours::quiet;
}

juce::String ageText(juce::int64 madeMs)
{
    const auto minutes = (juce::Time::currentTimeMillis() - madeMs) / 60000;
    if (minutes < 1)
        return pl("przed chwilą");
    if (minutes < 60)
        return juce::String(minutes) + pl(" min temu");
    if (minutes < 48 * 60)
        return juce::String(minutes / 60) + pl(" godz. temu");
    return juce::Time(madeMs).formatted("%d.%m.%Y %H:%M");
}
} // namespace

void BrainDecisionsView::setState(const AgentProcessor::BrainNotes& newNotes, int brain, int changed, int manual, bool connected)
{
    const auto minute = newNotes.madeMs != 0 ? (juce::Time::currentTimeMillis() - newNotes.madeMs) / 60000 : -1;
    if (newNotes.madeMs == notes.madeMs && brain == brainBits && changed == changedBits && manual == manualBits
        && connected == brainConnected && minute == shownMinute)
        return;
    notes = newNotes;
    brainBits = brain;
    changedBits = changed;
    manualBits = manual;
    brainConnected = connected;
    shownMinute = minute;
    repaint();
}

juce::Rectangle<int> BrainDecisionsView::rowArea(int tool) const
{
    auto area = getLocalBounds().reduced(12, 8);
    area.removeFromTop(54);
    const int rowHeight = area.getHeight() / ma::bus::kToolCount;
    return area.withTrimmedTop(rowHeight * tool).withHeight(rowHeight);
}

void BrainDecisionsView::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().reduced(12, 8);
    auto header = area.removeFromTop(54);
    g.setFont(juce::FontOptions(13.0f));
    juce::String intro;
    if (notes.madeMs == 0)
        intro = brainConnected ? pl("Mózg jeszcze nie analizował tego kanału. Puść drop i kliknij „Analizuj” w Mózgu (na masterze); "
                                    "tu pojawi się, co postanowił przy każdym narzędziu i dlaczego.")
                               : pl("Brak Mózgu na masterze. Wstaw wtyczkę Mózg na master i kliknij „Analizuj”; tu pojawi się, "
                                    "co postanowił przy każdym narzędziu i dlaczego.");
    else
        intro = pl("Ostatnie „Analizuj” w Mózgu: ") + ageText(notes.madeMs)
                + (notes.wholeMix ? pl(" (cały miks). ") : pl(" (tylko korekcje, łańcuch nie był sprawdzany). "))
                + pl("Mózg ma dostęp do wszystkich narzędzi tego kanału; przy każdym jest, co postanowił i dlaczego.");
    g.setColour(colours::quiet);
    g.drawFittedText(intro, header.withTrimmedBottom(14), juce::Justification::topLeft, 2, 1.0f);
    {
        auto columns = header.removeFromBottom(14);
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(pl("NARZĘDZIE"), columns.removeFromLeft(178), juce::Justification::left);
        g.drawText(pl("DECYZJA MÓZGU"), columns.removeFromLeft(136), juce::Justification::left);
        g.drawText(pl("TERAZ"), columns.removeFromLeft(118), juce::Justification::left);
        g.drawText(pl("DLACZEGO"), columns, juce::Justification::left);
    }

    const auto tools = ma::ui::toolNames();
    const auto decisions = ma::ui::decisionNames();
    for (int t = 0; t < ma::bus::kToolCount; ++t)
    {
        auto row = rowArea(t);
        if (t % 2 == 0)
        {
            g.setColour(colours::panel.withAlpha(0.55f));
            g.fillRect(row);
        }
        row.reduce(6, 2);
        const int bit = 1 << t;
        const int decision = notes.decision[static_cast<size_t>(t)];
        g.setFont(juce::FontOptions(13.5f));
        g.setColour(colours::text);
        g.drawText(tools[t], row.removeFromLeft(172), juce::Justification::centredLeft);
        g.setColour(decisionColour(decision));
        g.drawText(decision > 0 ? decisions[decision] : pl("—"), row.removeFromLeft(136), juce::Justification::centredLeft);
        const auto now = (brainBits & bit) != 0 ? pl("od Mózgu") : (changedBits & bit) != 0 ? pl("zmienione ręcznie") : (manualBits & bit) != 0 ? pl("ręcznie") : pl("wyłączone");
        g.setColour((brainBits & bit) != 0 ? colours::good : (changedBits & bit) != 0 ? colours::accent : colours::quiet);
        g.setFont(juce::FontOptions(12.5f));
        g.drawText(now, row.removeFromLeft(118), juce::Justification::centredLeft);
        g.setColour(decision > 0 ? colours::text.withAlpha(0.85f) : colours::quiet);
        g.drawFittedText(decision > 0 ? notes.text[static_cast<size_t>(t)] : juce::String(), row, juce::Justification::centredLeft, 2, 0.92f);
    }
}

juce::String BrainDecisionsView::getTooltip()
{
    const auto mouse = getMouseXYRelative();
    for (int t = 0; t < ma::bus::kToolCount; ++t)
        if (rowArea(t).contains(mouse) && notes.decision[static_cast<size_t>(t)] > 0)
            return ma::ui::toolNames()[t] + ": " + ma::ui::decisionNames()[notes.decision[static_cast<size_t>(t)]] + ", "
                   + notes.text[static_cast<size_t>(t)];
    return {};
}

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

    eqBypassButton.setButtonText(pl("Wyłącz"));
    eqBypassButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    eqBypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, AgentProcessor::eqBypassId, eqBypassButton);

    eqClearButton.setButtonText(pl("Wyzeruj"));
    eqClearButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    eqClearButton.onClick = [this] { processor.clearEq(); };

    connection.setJustificationType(juce::Justification::centredRight);
    connection.setFont(juce::FontOptions(13.0f));

    nameEditor.setText(processor.getUserName(), false);
    nameEditor.setTextToShowWhenEmpty(processor.getDisplayName(), colours::quiet);
    nameEditor.setColour(juce::TextEditor::backgroundColourId, colours::panel);
    nameEditor.setColour(juce::TextEditor::textColourId, colours::text);
    nameEditor.setTooltip(pl("Nazwa w Mózgu. Pusta = nazwa ścieżki z FL."));
    nameEditor.onTextChange = [this] { processor.setUserName(nameEditor.getText()); };

    styleCombo(roleBox, AgentProcessor::roleNames());
    styleCombo(measureBox, { "RMS (dBFS)", "LUFS" });

    for (auto* slider : { &prioritySlider, &targetSlider, &trimSlider, &ceilingSlider, &learnTimeSlider })
        styleSlider(*slider);

    learnButton.setButtonText(pl("Naucz poziom"));
    learnButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    learnButton.setColour(juce::TextButton::buttonOnColourId, colours::accent);
    learnButton.onClick = [this] { toggleLearning(); };
    roleLevelButton.setButtonText(pl("Poziom według roli"));
    roleLevelButton.setTooltip(pl("Nauka ustawia kanał względem celu według roli: stopa na celu, bas 2 dB niżej, wokal 4, "
                                  "lead i perkusja 6, pad 8, efekty 10 dB niżej. Bez tego każdy kanał dostaje ten sam poziom."));
    roleLevelButton.setColour(juce::ToggleButton::textColourId, colours::text);

    showProfileButton.setButtonText(pl("Pokaż nauczony profil"));
    showProfileButton.setColour(juce::ToggleButton::textColourId, colours::quiet);

    styleLabel(duckLabel, pl("Ustępowanie (sidechain)"), colours::quiet);
    styleLabel(duckStatus, {}, colours::quiet, 13.0f);
    duckStatus.setJustificationType(juce::Justification::topLeft);
    styleLabel(unmaskLabel, pl("Ustępowanie bez kabla"), colours::quiet);
    styleLabel(unmaskStatus, {}, colours::quiet, 13.0f);
    unmaskStatus.setJustificationType(juce::Justification::topLeft);
    unmaskOffButton.setButtonText(pl("Wyłącz"));
    unmaskOffButton.setTooltip(pl("Wyłącza ustępowanie innym kanałom. Mózg może je zaproponować znowu przy następnym „Analizuj”."));
    unmaskOffButton.onClick = [this] {
        for (int d = 0; d < ma::kMaxUnmask; ++d)
            if (auto* param = processor.parameters.getParameter(AgentProcessor::unmaskIds[d][0]))
                param->setValueNotifyingHost(0.0f);
    };
    styleLabel(duckFreqLabel, pl("Pasmo"), colours::quiet, 13.0f);
    styleLabel(duckDepthLabel, pl("Głębokość"), colours::quiet, 13.0f);
    styleLabel(duckThresholdLabel, pl("Próg"), colours::quiet, 13.0f);
    styleLabel(duckAttackLabel, pl("Atak"), colours::quiet, 13.0f);
    styleLabel(duckReleaseLabel, pl("Powrót"), colours::quiet, 13.0f);
    duckOnButton.setButtonText(pl("Włącz"));
    polarityButton.setButtonText(pl("Odwróć polaryzację"));
    polarityButton.setTooltip(pl("Odwraca fazę kanału o 180°. Pomaga, gdy stopa i bas się znoszą."));
    for (auto* button : { &duckOnButton, &polarityButton })
        button->setColour(juce::ToggleButton::textColourId, colours::quiet);
    styleCombo(duckModeBox, AgentProcessor::duckModeNames());
    styleCombo(duckSyncBox, AgentProcessor::duckSyncNames());
    duckSyncBox.setTooltip(pl("Czas powrotu dopasowany do tempa utworu."));
    for (auto* slider : { &duckFreqSlider, &duckDepthSlider, &duckThresholdSlider, &duckAttackSlider, &duckReleaseSlider })
    {
        styleSlider(*slider);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }

    styleLabel(harmonicsLabel, pl("Harmoniczne"), colours::quiet);
    harmonicsLabel.setTooltip(pl("Dodaje alikwoty treści poniżej podziału, żeby była słyszalna na telefonie i laptopie."));
    styleLabel(harmonicsFreqLabel, pl("Poniżej"), colours::quiet, 13.0f);
    styleLabel(harmonicsAmountLabel, pl("Ilość"), colours::quiet, 13.0f);
    harmonicsOnButton.setButtonText(pl("Włącz"));
    harmonicsOnButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    for (auto* slider : { &harmonicsFreqSlider, &harmonicsAmountSlider })
    {
        styleSlider(*slider);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }

    styleLabel(attackLabel, pl("Atak"), colours::quiet);
    attackLabel.setTooltip(pl("Podkreśla tylko początek dźwięku (klik stopy, trzask werbla) w paśmie, które słychać na małych głośnikach."));
    styleLabel(attackFreqLabel, pl("Powyżej"), colours::quiet, 13.0f);
    styleLabel(attackAmountLabel, pl("Ilość"), colours::quiet, 13.0f);
    attackOnButton.setButtonText(pl("Włącz"));
    attackOnButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    for (auto* slider : { &attackFreqSlider, &attackAmountSlider })
    {
        styleSlider(*slider);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }

    styleLabel(satLabel, pl("Przester"), colours::quiet);
    satLabel.setTooltip(pl("Saturacja z 4x nadpróbkowaniem. Głośność zostaje, zmienia się barwa."));
    styleLabel(satDriveLabel, "Drive", colours::quiet, 13.0f);
    styleLabel(satMixLabel, pl("Miks"), colours::quiet, 13.0f);
    satOnButton.setButtonText(pl("Włącz"));
    intentionalButton.setButtonText(pl("Celowy przester"));
    intentionalButton.setTooltip(pl("Kanał ma brzmieć brudno: nauka poziomu nie pilnuje szczytów, a Mózg nie ostrzega o przesterowaniu."));
    for (auto* button : { &satOnButton, &intentionalButton })
        button->setColour(juce::ToggleButton::textColourId, colours::quiet);
    styleCombo(satModeBox, AgentProcessor::saturationModeNames());
    for (auto* slider : { &satDriveSlider, &satMixSlider })
    {
        styleSlider(*slider);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    }

    styleLabel(chainTitle, pl("Ustawia je Mózg („Analizuj”, potem „Zastosuj”), patrząc na cały miks. Można też ręcznie. Przy każdej sekcji: "
                                  "„od Mózgu” (tak, jak Mózg ustawił), „zmienione ręcznie” albo „ręcznie”."),
               colours::quiet, 13.0f);
    for (auto& label : origins)
    {
        styleLabel(label, {}, colours::quiet, 12.5f);
        label.setJustificationType(juce::Justification::centredRight);
    }
    styleLabel(cleanLabel, pl("Porządki"), colours::quiet);
    styleLabel(compLabel, pl("Kompresor"), colours::quiet);
    styleLabel(clipLabel, "Clipper", colours::quiet);
    styleLabel(evenLabel, pl("Wyrównanie nut basu"), colours::quiet);
    evenLabel.setTooltip(pl("Niskie nuty ucho słyszy ciszej. Podnosi je, a wyższe obniża, względem typowej nuty linii."));
    styleLabel(hpfLabel, "HPF", colours::quiet, 13.0f);
    hpfLabel.setTooltip(pl("Filtr górnoprzepustowy 24 dB/okt. Wycina śmieci poniżej treści."));
    styleLabel(monoLabel, "Mono <", colours::quiet, 13.0f);
    monoLabel.setTooltip(pl("Poniżej tej częstotliwości kanał gra w mono (stabilny dół w klubie)."));
    styleLabel(mudFreqLabel, pl("Muł Hz"), colours::quiet, 13.0f);
    styleLabel(mudGainLabel, pl("Muł dB"), colours::quiet, 13.0f);
    styleLabel(compThresholdLabel, pl("Próg"), colours::quiet, 13.0f);
    styleLabel(compRatioLabel, "Ratio", colours::quiet, 13.0f);
    styleLabel(compAttackLabel, pl("Atak"), colours::quiet, 13.0f);
    styleLabel(compReleaseLabel, pl("Powrót"), colours::quiet, 13.0f);
    styleLabel(compKneeLabel, "Knee", colours::quiet, 13.0f);
    styleLabel(compMakeupLabel, "Makeup", colours::quiet, 13.0f);
    styleLabel(clipCeilingLabel, pl("Sufit"), colours::quiet, 13.0f);
    styleLabel(clipSoftnessLabel, pl("Miękkość"), colours::quiet, 13.0f);
    styleLabel(evenAmountLabel, pl("Ilość"), colours::quiet, 13.0f);
    styleLabel(evenRefLabel, pl("Nuta"), colours::quiet, 13.0f);
    evenRefLabel.setTooltip(pl("Typowa nuta linii basu: ta zostaje bez zmian."));
    styleLabel(stereoLabel, pl("Faza w pasmach (mid-side)"), colours::quiet);
    stereoLabel.setTooltip(pl("Jak Correlometer: na dole przeciwfaza, która w mono znika. „Analizuj” (cały łańcuch) zdejmuje tam side, "
                              "a ze stemami referencji poszerza kanał do szerokości jego grupy w referencjach (nigdy poniżej korelacji 0,2)."));
    styleLabel(widthLabel, pl("Szerokość"), colours::text, 13.0f);
    widthLabel.setTooltip(pl("Od mono (0%) przez „bez zmian” (100%) do „max” (200%), czyli tak szeroko, jak się da bez przeciwfazy. "
                             "Poszerza od 300 Hz w górę kopią środka opóźnioną o kilka ms i podbiciem side'u. Agent pilnuje każdego pasma "
                             "(korelacja nie spada poniżej 0,2, w razie potrzeby tnie side w tym paśmie). Agenty pilnują też sumy: "
                             "suwaki ustępują, gdy suma zeszłaby poniżej 0,2. W mono nic się nie zmienia."));
    styleLabel(widthFromLabel, pl("Mono poniżej"), colours::quiet, 13.0f);
    widthFromLabel.setTooltip(pl("Gdy suwak szerokości jest w użyciu, poniżej tej częstotliwości kanał gra w mono."));
    styleLabel(widthMonoMark, "mono", colours::quiet, 11.0f);
    styleLabel(widthNeutralMark, pl("bez zmian"), colours::quiet, 11.0f);
    styleLabel(widthMaxMark, "max", colours::quiet, 11.0f);
    widthNeutralMark.setJustificationType(juce::Justification::centred);
    widthMaxMark.setJustificationType(juce::Justification::centredRight);
    for (auto* label : { &cleanStatus, &compStatus, &clipStatus, &evenStatus, &stereoStatus, &widthStatusLabel })
    {
        styleLabel(*label, {}, colours::quiet, 13.0f);
        label->setJustificationType(juce::Justification::topLeft);
    }
    for (auto* button : { &compOnButton, &clipOnButton, &evenOnButton })
    {
        button->setButtonText(pl("Włącz"));
        button->setColour(juce::ToggleButton::textColourId, colours::quiet);
    }

    auto& state = processor.parameters;
    const std::pair<juce::Slider*, const char*> chainSliders[] {
        { &hpfSlider, AgentProcessor::hpfId },
        { &monoSlider, AgentProcessor::monoBelowId },
        { &mudFreqSlider, AgentProcessor::mudFreqId },
        { &mudGainSlider, AgentProcessor::mudGainId },
        { &compThresholdSlider, AgentProcessor::compThresholdId },
        { &compRatioSlider, AgentProcessor::compRatioId },
        { &compAttackSlider, AgentProcessor::compAttackId },
        { &compReleaseSlider, AgentProcessor::compReleaseId },
        { &compKneeSlider, AgentProcessor::compKneeId },
        { &compMakeupSlider, AgentProcessor::compMakeupId },
        { &clipCeilingSlider, AgentProcessor::clipCeilingId },
        { &clipSoftnessSlider, AgentProcessor::clipSoftnessId },
        { &evenAmountSlider, AgentProcessor::evenAmountId },
        { &evenRefSlider, AgentProcessor::evenRefId },
        { &widthSlider, AgentProcessor::widthId },
        { &widthFromSlider, AgentProcessor::widthFromId },
    };
    for (const auto& [slider, id] : chainSliders)
    {
        styleSlider(*slider);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
        chainAttachments.push_back(std::make_unique<SliderAttachment>(state, id, *slider));
    }
    compOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::compOnId, compOnButton);
    clipOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::clipOnId, clipOnButton);
    evenOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::evenOnId, evenOnButton);

    roleAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::roleId, roleBox);
    measureAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::measureId, measureBox);
    priorityAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::priorityId, prioritySlider);
    targetAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::targetId, targetSlider);
    trimAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::trimId, trimSlider);
    ceilingAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::peakCeilingId, ceilingSlider);
    learnTimeAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::learnTimeId, learnTimeSlider);
    duckOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::duckOnId, duckOnButton);
    polarityAttachment
        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::polarityId, polarityButton);
    duckModeAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::duckModeId, duckModeBox);
    duckSyncAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::duckSyncId, duckSyncBox);
    duckFreqAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::duckFreqId, duckFreqSlider);
    duckDepthAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::duckDepthId, duckDepthSlider);
    duckThresholdAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::duckThresholdId, duckThresholdSlider);
    duckAttackAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::duckAttackId, duckAttackSlider);
    duckReleaseAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::duckReleaseId, duckReleaseSlider);
    harmonicsOnAttachment
        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::harmonicsOnId, harmonicsOnButton);
    harmonicsFreqAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::harmonicsFreqId, harmonicsFreqSlider);
    harmonicsAmountAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::harmonicsAmountId, harmonicsAmountSlider);
    attackOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::attackOnId, attackOnButton);
    attackFreqAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::attackFreqId, attackFreqSlider);
    attackAmountAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::attackAmountId, attackAmountSlider);
    satOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::satOnId, satOnButton);
    roleLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state, AgentProcessor::roleLevelId, roleLevelButton);
    intentionalAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        state, AgentProcessor::intentionalDriveId, intentionalButton);
    satModeAttachment = std::make_unique<ComboAttachment>(state, AgentProcessor::satModeId, satModeBox);
    satDriveAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::satDriveId, satDriveSlider);
    satMixAttachment = std::make_unique<SliderAttachment>(state, AgentProcessor::satMixId, satMixSlider);

    // One fixed-size window: the header stays, everything else sits on tabs.
    for (auto* c : std::initializer_list<juce::Component*> { &title, &connection, &nameEditor, &tabs })
        addAndMakeVisible(c);
    const auto page = [this](ma::ui::Page& target, const juce::String& name, std::initializer_list<juce::Component*> children,
                             std::function<void(juce::Rectangle<int>)> layout) {
        for (auto* c : children)
            target.addAndMakeVisible(c);
        target.layout = std::move(layout);
        tabs.add(name, target);
    };
    page(levelPage, pl("Poziom"),
         { &roleLabel, &roleBox, &priorityLabel, &prioritySlider, &inputLabel, &inputValue, &inputBar, &lufsLabel, &lufsValue,
           &outputLabel, &outputValue, &outputBar, &measureLabel, &measureBox, &targetSlider, &trimLabel, &trimSlider,
           &ceilingLabel, &ceilingSlider, &learnTimeLabel, &learnTimeSlider, &learnButton, &roleLevelButton, &status, &spectrumLabel,
           &showProfileButton, &spectrumView },
         [this](auto area) { layoutLevel(area); });
    page(eqPage, pl("EQ i ustępowanie"),
         { &eqLabel, &origins[0], &origins[7], &eqStatus, &eqCurve, &eqBypassButton, &eqClearButton, &duckLabel, &duckOnButton, &duckModeBox,
           &polarityButton, &duckFreqLabel, &duckFreqSlider, &duckDepthLabel, &duckDepthSlider, &duckThresholdLabel,
           &duckThresholdSlider, &duckAttackLabel, &duckAttackSlider, &duckReleaseLabel, &duckReleaseSlider, &duckSyncBox,
           &duckStatus, &unmaskLabel, &origins[10], &unmaskStatus, &unmaskOffButton },
         [this](auto area) { layoutEq(area); });
    page(tonePage, pl("Barwa"),
         { &harmonicsLabel, &origins[8], &origins[9], &origins[6], &harmonicsOnButton, &harmonicsFreqLabel, &harmonicsFreqSlider, &harmonicsAmountLabel,
           &harmonicsAmountSlider, &attackLabel, &attackOnButton, &attackFreqLabel, &attackFreqSlider, &attackAmountLabel,
           &attackAmountSlider, &satLabel, &satOnButton, &satModeBox, &intentionalButton, &satDriveLabel, &satDriveSlider,
           &satMixLabel, &satMixSlider, &toneDecisions },
         [this](auto area) { layoutTone(area); });
    page(chainPage, pl("Inżynier miksu"),
         { &chainTitle, &origins[1], &origins[2], &origins[3], &origins[4], &cleanLabel, &hpfLabel, &hpfSlider, &monoLabel, &monoSlider, &mudFreqLabel, &mudFreqSlider,
           &mudGainLabel, &mudGainSlider, &cleanStatus, &compLabel, &compOnButton, &compThresholdLabel, &compThresholdSlider,
           &compRatioLabel, &compRatioSlider, &compAttackLabel, &compAttackSlider, &compReleaseLabel, &compReleaseSlider,
           &compKneeLabel, &compKneeSlider, &compMakeupLabel, &compMakeupSlider, &compStatus, &clipLabel, &clipOnButton,
           &clipCeilingLabel, &clipCeilingSlider, &clipSoftnessLabel, &clipSoftnessSlider, &clipStatus, &evenLabel,
           &evenOnButton, &evenAmountLabel, &evenAmountSlider, &evenRefLabel, &evenRefSlider, &evenStatus },
         [this](auto area) { layoutChain(area); });
    page(stereoPage, "Stereo",
         { &stereoLabel, &origins[5], &stereoView, &stereoStatus, &widthLabel, &widthSlider, &widthMonoMark, &widthNeutralMark, &widthMaxMark,
           &widthFromLabel, &widthFromSlider, &widthStatusLabel },
         [this](auto area) { layoutStereo(area); });
    toneDecisions.setJustificationType(juce::Justification::topLeft);
    toneDecisions.setFont(juce::FontOptions(13.0f));
    toneDecisions.setColour(juce::Label::textColourId, colours::quiet);
    page(decisionsPage, pl("Decyzje Mózgu"), { &decisionsView }, [this](auto area) { decisionsView.setBounds(area); });
    widthSlider.setDoubleClickReturnValue(true, ma::kWidthNeutral);
    tabs.setCurrentTabIndex(juce::jlimit(0, tabs.getNumTabs() - 1, processor.editorTab), false);
    tabs.onChange = [this](int index) { processor.editorTab = index; };

    shownResultSerial = processor.lastResult().serial;
    processor.setSpectrumVisible(true);
    setSize(kWidth, kHeight);
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
    if (std::abs(r.roleOffsetDb) > 0.05)
        text += pl(" Cel dla roli: ") + ma::ui::formatDb(r.targetDb, unit.toRawUTF8()) + " (" + signedDb(r.roleOffsetDb)
                + pl(" względem stopy).");
    if (r.limitedByPeak)
    {
        // Nauka ustawia tylko trim. Kompresor i clipper z zakładki „Inżynier miksu” ustawia Mózg,
        // bo dobiera je do całego miksu (rola, priorytet, co gra obok).
        int brain = 0, changed = 0, manual = 0;
        processor.toolOrigins(brain, changed, manual);
        const int peakTools = ma::bus::toolCompressor | ma::bus::toolClipper;
        if (((brain | changed | manual) & peakTools) != 0)
            text += pl("\nTrim ograniczony szczytem. Szczyty obrabia już kompresor albo clipper (zakładka „Inżynier miksu”).");
        else
            text += pl("\nTrim ograniczony szczytem. Nauka ustawia tylko trim; kompresor i clipper z zakładki „Inżynier miksu” "
                       "ustawi Mózg: „Analizuj” (cały łańcuch), potem „Zastosuj”.");
    }
    if (r.guess.role != ma::Role::other)
    {
        const auto roles = AgentProcessor::roleNames();
        const auto percent = juce::String(juce::roundToInt(r.guess.confidence * 100.0)) + "%";
        text += pl("\nRozpoznano: ") + roles[static_cast<int>(r.guess.role)] + " (" + percent + ")";
        if (r.roleSet)
            text += r.prioritySet ? pl(", ustawiono rolę i priorytet.") : pl(", ustawiono rolę.");
        else if (r.guess.confidence < 0.5)
            text += pl(", za mało pewne, rola bez zmian.");
        else
            text += ".";
    }
    status.setText(text, juce::dontSendNotification);
    status.setColour(juce::Label::textColourId, r.limitedByPeak ? colours::warning : colours::good);
}

void AgentEditor::timerCallback()
{
    updateOrigins();
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
        status.setText(pl("Słucham: ") + juce::String(processor.learningSeconds(), 1) + " / " + juce::String(juce::roundToInt(needed))
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

    updateDuckStatus();
    updateUnmaskStatus();
    updateChainStatus();

    std::array<double, ma::ui::SpectrumView::kMaxBands> levels {}, centres {};
    const bool profile = showProfileButton.getToggleState() && processor.hasProfile();
    if (profile)
        processor.copyProfile(levels.data(), centres.data());
    else
        processor.copyLiveSpectrum(levels.data(), centres.data());
    spectrumView.setBands(levels.data(), centres.data(), AgentProcessor::numSpectrumBands());
    showProfileButton.setEnabled(processor.hasProfile());
}

void AgentEditor::updateDuckStatus()
{
    const auto dynamics = processor.currentDynamics();
    const bool on = dynamics.duck.on > 0.5f;
    const bool synced = dynamics.sync > 0;
    duckReleaseSlider.setEnabled(!synced || processor.hostTempo() <= 0.0);
    duckFreqSlider.setEnabled(dynamics.duck.wholeSignal < 0.5f);

    juce::String text;
    auto colour = colours::quiet;
    if (!processor.sidechainConnected() && on && processor.duckKeyChannel().isNotEmpty())
    {
        // Phase 30: no cable, the key comes over the bus.
        text = pl("Bez kabla: słucha „") + processor.duckKeyChannel() + pl("” przez wspólną pamięć, ustępuje o ")
               + juce::String(processor.duckReductionDb(), 1) + " dB";
        colour = colours::good;
    }
    else if (!processor.sidechainConnected())
    {
        text = on ? pl("Brak sygnału na wejściu sidechain. W mikserze FL: na kanale stopy kliknij prawym strzałkę "
                       "wysyłki do tego kanału, wybierz „Sidechain to this track”, potem w oknie wtyczki (ikona "
                       "wtyczki, Processing) ustaw wejście sidechain.")
                  : pl("Wyłączone. Mózg może je zaproponować dla basu pod stopą.");
        colour = on ? colours::warning : colours::quiet;
    }
    else
    {
        text = "Sidechain " + ma::ui::formatDb(processor.sidechainLevelDb(), "dBFS");
        if (on)
            text += pl(", ustępuje o ") + juce::String(processor.duckReductionDb(), 1) + " dB";
        if (synced && processor.hostTempo() > 0.0)
            text += pl(", powrót ") + juce::String(juce::roundToInt(processor.effectiveReleaseMs())) + " ms ("
                    + duckSyncBox.getText() + ")";
        else if (synced)
            text += pl(", host nie podał tempa, powrót ") + juce::String(juce::roundToInt(processor.effectiveReleaseMs())) + " ms";
        if (processor.correlationValid())
        {
            const double r = processor.sidechainLowCorrelation();
            text += pl("\nFaza z sidechainem w basie: ") + juce::String(r, 2);
            if (r < -0.2)
            {
                text += pl(" (znoszą się, spróbuj odwrócić polaryzację)");
                colour = colours::warning;
            }
        }
        if (colour != colours::warning)
            colour = on ? colours::good : colours::quiet;
    }
    duckStatus.setText(text, juce::dontSendNotification);
    duckStatus.setColour(juce::Label::textColourId, colour);
}

// Phase 30: what the channel gives way to, and how much it takes off right now.
void AgentEditor::updateUnmaskStatus()
{
    juce::StringArray lines;
    bool any = false, missing = false;
    for (int d = 0; d < ma::kMaxUnmask; ++d)
    {
        const auto dip = processor.unmaskStatus(d);
        if (!dip.on)
            continue;
        any = true;
        const auto& s = dip.settings;
        const auto where = s.hz >= 1000.0f ? juce::String(s.hz / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(s.hz)) + " Hz";
        if (dip.keyName.isEmpty())
        {
            missing = true;
            lines.add(pl("Ustępuje kanałowi, którego Agent teraz nie działa (") + where + pl("): nic nie ścina."));
            continue;
        }
        lines.add(pl("Ustępuje „") + dip.keyName + pl("” wokół ") + where + pl(" do ") + juce::String(s.depthDb, 1)
                  + pl(" dB, gdy tamten gra; teraz ") + juce::String(dip.reductionDb, 1) + " dB.");
    }
    if (!any)
        lines.add(pl("Wyłączone. Mózg włącza je, gdy ten kanał zasłania inny, a EQ i poziom tego nie usuną. Działa bez routingu w FL."));
    unmaskStatus.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
    unmaskStatus.setColour(juce::Label::textColourId, missing ? colours::warning : any ? colours::good : colours::quiet);
    unmaskOffButton.setEnabled(any);
}

void AgentEditor::updateChainStatus()
{
    const auto dynamics = processor.currentDynamics();
    const auto& cleanup = dynamics.cleanup;

    juce::StringArray clean;
    if (cleanup.hpfHz > 0.0f)
        clean.add("HPF " + juce::String(juce::roundToInt(cleanup.hpfHz)) + " Hz");
    if (cleanup.monoBelowHz > 0.0f)
        clean.add(pl("mono poniżej ") + juce::String(juce::roundToInt(cleanup.monoBelowHz)) + " Hz");
    if (cleanup.mudDb < 0.0f)
        clean.add(pl("muł ") + juce::String(cleanup.mudDb, 1) + " dB @ " + juce::String(juce::roundToInt(cleanup.mudHz)) + " Hz");
    for (const auto& notch : cleanup.notches)
        if (notch.db < -0.01f)
            clean.add(pl("rezonans ") + juce::String(notch.db, 1) + " dB @ " + juce::String(juce::roundToInt(notch.hz)) + " Hz");
    cleanStatus.setText(clean.isEmpty() ? pl("Wyłączone. 0 Hz = filtr wyłączony.") : clean.joinIntoString(", "),
                        juce::dontSendNotification);
    cleanStatus.setColour(juce::Label::textColourId, clean.isEmpty() ? colours::quiet : colours::good);

    const bool compOn = dynamics.compressor.on > 0.5f;
    const double reduction = processor.compressorReductionDb();
    compStatus.setText(compOn ? pl("Redukcja ") + juce::String(reduction, 1) + " dB" : pl("Wyłączony."), juce::dontSendNotification);
    compStatus.setColour(juce::Label::textColourId, !compOn ? colours::quiet : reduction > 10.0 ? colours::warning : colours::good);

    const bool clipOn = dynamics.clipper.on > 0.5f;
    const double clipped = processor.clipperDb();
    clipStatus.setText(clipOn ? pl("Ścina szczyty o ") + juce::String(clipped, 1) + " dB" : pl("Wyłączony."), juce::dontSendNotification);
    clipStatus.setColour(juce::Label::textColourId, !clipOn ? colours::quiet : clipped > 4.0 ? colours::warning : colours::good);

    const bool evenOn = dynamics.evener.on > 0.5f;
    juce::String even = pl("Wyłączone.");
    if (evenOn)
    {
        const double note = processor.evenerNoteHz();
        even = note > 0.0 ? pl("Nuta ") + juce::String(note, 1) + " Hz, " + signedDb(processor.evenerGainDb())
                          : pl("Czeka na nutę basu (") + signedDb(processor.evenerGainDb()) + ")";
    }
    evenStatus.setText(even, juce::dontSendNotification);
    evenStatus.setColour(juce::Label::textColourId, evenOn ? colours::good : colours::quiet);

    std::array<double, ma::ui::CoherenceView::kBands> coherence {};
    std::array<bool, ma::ui::CoherenceView::kBands> audible {};
    const bool measured = processor.copyStereo(coherence.data(), audible.data());
    stereoView.setBands(coherence.data(), measured, nullptr, audible.data());
    juce::StringArray bells;
    for (const auto& bell : dynamics.stereo.side)
        if (std::abs(bell.db) > 0.01f)
            bells.add("side " + signedDb(bell.db) + " @ " + juce::String(juce::roundToInt(bell.hz)) + " Hz");
    if (ma::widenOn(dynamics.stereo))
        bells.add(pl("poszerzenie ") + juce::String(dynamics.stereo.widen.amountDb, 1) + pl(" dB od ")
                  + juce::String(juce::roundToInt(dynamics.stereo.widen.lowHz)) + " Hz (" + juce::String(dynamics.stereo.widen.delayMs, 1)
                  + pl(" ms; w mono bez zmian)"));
    stereoStatus.setText(bells.isEmpty() ? pl("Mid-side EQ wyłączony.") : pl("Od Mózgu: ") + bells.joinIntoString(", "),
                         juce::dontSendNotification);
    stereoStatus.setColour(juce::Label::textColourId, bells.isEmpty() ? colours::quiet : colours::good);

    // Phase 26: what the slider does now.
    const auto w = processor.widthStatus();
    juce::String text;
    auto colour = colours::quiet;
    if (!ma::widthRequested(w.request))
        text = pl("Suwak na 100%: szerokość bez zmian. W lewo węziej (0% = mono), w prawo szerzej (200% = max bez przeciwfazy).");
    else
    {
        juce::StringArray parts;
        parts.add(pl("Mono poniżej ") + juce::String(juce::roundToInt(w.settings.fromHz)) + " Hz");
        const float asked = std::min(1.0f, w.request / ma::kWidthNeutral);
        if (w.request < ma::kWidthNeutral || w.settings.scale < asked - 0.01f)
        {
            parts.add(w.settings.scale < 0.005f ? pl("wyżej też mono") : pl("side wyżej ") + signedDb(20.0 * std::log10(w.settings.scale)));
            if (w.settings.scale < asked - 0.01f)
                parts.add(pl("węziej, niż chce suwak: kanał ma przeciwfazę w zbyt wielu pasmach, żeby wyciąć ją punktowo"));
        }
        else
        {
            if (w.settings.scale > 1.005f)
                parts.add(pl("side ") + signedDb(20.0 * std::log10(w.settings.scale)));
            if (w.settings.widenDb > ma::kWidenOffDb)
                parts.add(pl("poszerzenie ") + juce::String(w.settings.widenDb, 1) + pl(" dB (") + juce::String(w.settings.delayMs, 1) + " ms)");
            if (w.planned)
                parts.add(pl("max dla tego kanału: ") + juce::String(juce::roundToInt(100.0 * w.reach)) + pl("% pełnego poszerzenia"));
        }
        for (const auto& g : w.settings.guard)
            if (std::abs(g.db) > 0.01f)
                parts.add(pl("pilnuje ") + juce::String(g.hz >= 1000.0f ? juce::String(g.hz / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(g.hz)) + " Hz")
                          + " (side " + signedDb(g.db) + ")");
        text = parts.joinIntoString(", ") + ".";
        if (w.limitedBySum)
            text += pl(" Suma miksu nie pozwala szerzej (") + juce::String(w.sumLimitHz >= 1000.0 ? juce::String(w.sumLimitHz / 1000.0, 1) + " kHz"
                                                                                                : juce::String(juce::roundToInt(w.sumLimitHz)) + " Hz")
                    + pl(" zeszłoby poniżej 0,2).");
        else if (!w.planned)
            text += pl(" Czeka na pomiar stereo, dopiero wtedy poszerza.");
        colour = w.limitedBySum ? colours::warning : colours::good;
    }
    widthStatusLabel.setText(text, juce::dontSendNotification);
    widthStatusLabel.setColour(juce::Label::textColourId, colour);
}

void AgentEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void AgentEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(28);
    title.setBounds(top.removeFromLeft(190));
    connection.setBounds(top.removeFromRight(170));
    nameEditor.setBounds(top.reduced(6, 2));
    area.removeFromTop(8);
    tabs.setBounds(area);
}

void AgentEditor::layoutLevel(juce::Rectangle<int> area)
{
    constexpr int labelWidth = 130;
    area.reduce(12, 12);
    auto spectrum = area.removeFromRight(area.getWidth() / 2 - 10);
    area.removeFromRight(20);

    auto row = area.removeFromTop(26);
    roleLabel.setBounds(row.removeFromLeft(labelWidth));
    roleBox.setBounds(row);
    area.removeFromTop(4);
    row = area.removeFromTop(26);
    priorityLabel.setBounds(row.removeFromLeft(labelWidth));
    prioritySlider.setBounds(row);
    area.removeFromTop(10);

    const auto meterRow = [&](juce::Label& label, juce::Label& value, ma::ui::LevelBar* bar) {
        auto r = area.removeFromTop(24);
        label.setBounds(r.removeFromLeft(labelWidth));
        value.setBounds(r.removeFromRight(90));
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
    row = area.removeFromTop(32);
    learnButton.setBounds(row.removeFromLeft(200));
    row.removeFromLeft(12);
    roleLevelButton.setBounds(row.removeFromLeft(220));
    area.removeFromTop(6);
    status.setBounds(area);

    row = spectrum.removeFromTop(22);
    showProfileButton.setBounds(row.removeFromRight(180));
    spectrumLabel.setBounds(row);
    spectrum.removeFromTop(4);
    spectrumView.setBounds(spectrum);
}

void AgentEditor::layoutEq(juce::Rectangle<int> area)
{
    area.reduce(12, 12);
    auto row = area.removeFromTop(22);
    eqClearButton.setBounds(row.removeFromRight(80));
    row.removeFromRight(6);
    eqBypassButton.setBounds(row.removeFromRight(80));
    origin(ma::bus::toolEq).setBounds(row.removeFromRight(170));
    eqLabel.setBounds(row);
    area.removeFromTop(4);
    auto eqRow = area.removeFromTop(200);
    eqStatus.setBounds(eqRow.removeFromRight(190).withTrimmedLeft(12));
    eqCurve.setBounds(eqRow);

    area.removeFromTop(18);
    row = area.removeFromTop(24);
    duckLabel.setBounds(row.removeFromLeft(180));
    duckOnButton.setBounds(row.removeFromLeft(72));
    duckModeBox.setBounds(row.removeFromLeft(120).reduced(0, 1));
    row.removeFromLeft(12);
    polarityButton.setBounds(row.removeFromLeft(200));
    origin(ma::bus::toolDuck).setBounds(row);
    area.removeFromTop(6);
    const auto halfRow = [&](juce::Rectangle<int> r, juce::Label& label, juce::Component& control) {
        label.setBounds(r.removeFromLeft(80));
        control.setBounds(r);
    };
    for (auto pair : { std::pair { std::pair { &duckFreqLabel, &duckFreqSlider }, std::pair { &duckDepthLabel, &duckDepthSlider } },
                       std::pair { std::pair { &duckThresholdLabel, &duckThresholdSlider },
                                   std::pair { &duckAttackLabel, &duckAttackSlider } } })
    {
        row = area.removeFromTop(24);
        auto left = row.removeFromLeft(row.getWidth() / 2);
        halfRow(left.withTrimmedRight(16), *pair.first.first, *pair.first.second);
        halfRow(row, *pair.second.first, *pair.second.second);
        area.removeFromTop(4);
    }
    row = area.removeFromTop(24);
    halfRow(row.removeFromLeft(row.getWidth() / 2).withTrimmedRight(16), duckReleaseLabel, duckReleaseSlider);
    duckSyncBox.setBounds(row.removeFromLeft(110).reduced(0, 1));
    area.removeFromTop(8);
    duckStatus.setBounds(area.removeFromTop(48));
    area.removeFromTop(6);
    row = area.removeFromTop(24);
    unmaskLabel.setBounds(row.removeFromLeft(180));
    unmaskOffButton.setBounds(row.removeFromLeft(80).reduced(0, 1));
    row.removeFromLeft(12);
    origin(ma::bus::toolUnmask).setBounds(row.removeFromLeft(170));
    area.removeFromTop(4);
    unmaskStatus.setBounds(area);
}

void AgentEditor::layoutTone(juce::Rectangle<int> area)
{
    area.reduce(12, 12);
    const auto header = [&](juce::Label& label, juce::ToggleButton& button) {
        auto row = area.removeFromTop(24);
        label.setBounds(row.removeFromLeft(180));
        button.setBounds(row.removeFromLeft(72));
        area.removeFromTop(6);
        return row;
    };
    const auto pairRow = [&](juce::Label& leftLabel, juce::Slider& leftSlider, juce::Label& rightLabel, juce::Slider& rightSlider) {
        auto row = area.removeFromTop(24);
        auto left = row.removeFromLeft(row.getWidth() / 2).withTrimmedRight(16);
        leftLabel.setBounds(left.removeFromLeft(80));
        leftSlider.setBounds(left);
        rightLabel.setBounds(row.removeFromLeft(80));
        rightSlider.setBounds(row);
        area.removeFromTop(28);
    };

    origin(ma::bus::toolHarmonics).setBounds(header(harmonicsLabel, harmonicsOnButton).removeFromRight(170));
    pairRow(harmonicsFreqLabel, harmonicsFreqSlider, harmonicsAmountLabel, harmonicsAmountSlider);
    origin(ma::bus::toolTransients).setBounds(header(attackLabel, attackOnButton).removeFromRight(170));
    pairRow(attackFreqLabel, attackFreqSlider, attackAmountLabel, attackAmountSlider);
    auto row = header(satLabel, satOnButton);
    origin(ma::bus::toolSaturation).setBounds(row.removeFromRight(170));
    satModeBox.setBounds(row.removeFromLeft(130).reduced(0, 1));
    row.removeFromLeft(16);
    intentionalButton.setBounds(row.removeFromLeft(200));
    pairRow(satDriveLabel, satDriveSlider, satMixLabel, satMixSlider);
    toneDecisions.setBounds(area);
}

juce::Label& AgentEditor::origin(int bit)
{
    size_t index = 0;
    while (index + 1 < origins.size() && (1 << index) != bit)
        ++index;
    return origins[index];
}

void AgentEditor::updateOrigins()
{
    int brain = 0, changed = 0, manual = 0;
    processor.toolOrigins(brain, changed, manual);
    const auto& notes = processor.brainNotes();
    decisionsView.setState(notes, brain, changed, manual, processor.isConnectedToBrain());
    const auto decisions = ma::ui::decisionNames();
    {
        juce::String text;
        if (notes.madeMs == 0)
            text = pl("Mózg ma dostęp do harmonicznych, ataku i przesteru. Po „Analizuj” w Mózgu zobaczysz tu, co postanowił przy każdym i dlaczego.");
        else
        {
            text = pl("Decyzje Mózgu przy ostatnim „Analizuj” (wszystkie narzędzia: zakładka „Decyzje Mózgu”):");
            for (const int bit : { ma::bus::toolHarmonics, ma::bus::toolTransients, ma::bus::toolSaturation })
            {
                int t = 0;
                while ((1 << t) != bit)
                    ++t;
                const int decision = notes.decision[static_cast<size_t>(t)];
                if (decision > 0)
                    text += "\n\n" + ma::ui::toolNames()[t] + ": " + decisions[decision] + ", " + notes.text[static_cast<size_t>(t)];
            }
        }
        if (toneDecisions.getText() != text)
            toneDecisions.setText(text, juce::dontSendNotification);
    }
    for (size_t i = 0; i < origins.size(); ++i)
    {
        const int bit = 1 << i;
        auto& label = origins[i];
        const int decision = notes.decision[i];
        // Phase 31: a tool that is off still says what the Brain made of it, so "off" never reads as "not looked at".
        auto text = (brain & bit) != 0 ? pl("od Mózgu") : (changed & bit) != 0 ? pl("zmienione ręcznie") : (manual & bit) != 0 ? pl("ręcznie") : juce::String();
        auto colour = (brain & bit) != 0 ? colours::good : (changed & bit) != 0 ? colours::accent : colours::quiet;
        if (text.isEmpty() && decision > 0)
        {
            text = pl("Mózg: ") + decisions[decision];
            colour = decisionColour(decision);
        }
        auto tip = (brain & bit) != 0 ? pl("Tak, jak Mózg ustawił przy ostatnim „Zastosuj”. Nikt tego od tamtej pory nie ruszał.")
                   : (changed & bit) != 0 ? pl("Mózg to ustawił, potem zmieniono ręcznie. Następne „Analizuj” liczy od tego, co jest teraz.")
                   : (manual & bit) != 0  ? pl("Włączone ręcznie, Mózg tego nie ustawiał.")
                                          : juce::String();
        if (decision > 0)
            tip = (tip.isEmpty() ? juce::String() : tip + "\n") + pl("Ostatnie „Analizuj”: ") + decisions[decision] + ", " + notes.text[i]
                  + pl("\nWszystkie decyzje: zakładka „Decyzje Mózgu”.");
        if (label.getText() != text)
            label.setText(text, juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, colour);
        if (label.getTooltip() != tip)
            label.setTooltip(tip);
    }
}

void AgentEditor::layoutChain(juce::Rectangle<int> area)
{
    area.reduce(12, 12);
    chainTitle.setBounds(area.removeFromTop(22));
    area.removeFromTop(10);
    auto right = area.removeFromRight(area.getWidth() / 2 - 12);
    area.removeFromRight(24);

    const auto sliderRow = [](juce::Rectangle<int>& column, juce::Label& label, juce::Slider& slider) {
        auto row = column.removeFromTop(24);
        label.setBounds(row.removeFromLeft(80));
        slider.setBounds(row);
        column.removeFromTop(4);
    };
    const auto header = [](juce::Rectangle<int>& column, juce::Label& label, juce::ToggleButton* button, juce::Label& from) {
        auto row = column.removeFromTop(24);
        label.setBounds(row.removeFromLeft(180));
        if (button != nullptr)
            button->setBounds(row.removeFromLeft(72));
        from.setBounds(row);
        column.removeFromTop(4);
    };
    const auto statusRow = [](juce::Rectangle<int>& column, juce::Label& label) {
        label.setBounds(column.removeFromTop(36));
        column.removeFromTop(16);
    };

    // Left: cleanup and clipper, right: compressor and evener (the chain runs cleanup, clipper, compressor, evener).
    header(area, cleanLabel, nullptr, origin(ma::bus::toolCleanup));
    for (auto [label, slider] : { std::pair { &hpfLabel, &hpfSlider }, std::pair { &monoLabel, &monoSlider },
                                  std::pair { &mudFreqLabel, &mudFreqSlider }, std::pair { &mudGainLabel, &mudGainSlider } })
        sliderRow(area, *label, *slider);
    statusRow(area, cleanStatus);

    header(area, clipLabel, &clipOnButton, origin(ma::bus::toolClipper));
    sliderRow(area, clipCeilingLabel, clipCeilingSlider);
    sliderRow(area, clipSoftnessLabel, clipSoftnessSlider);
    statusRow(area, clipStatus);

    header(right, compLabel, &compOnButton, origin(ma::bus::toolCompressor));
    for (auto [label, slider] : { std::pair { &compThresholdLabel, &compThresholdSlider }, std::pair { &compRatioLabel, &compRatioSlider },
                                  std::pair { &compAttackLabel, &compAttackSlider }, std::pair { &compReleaseLabel, &compReleaseSlider },
                                  std::pair { &compKneeLabel, &compKneeSlider }, std::pair { &compMakeupLabel, &compMakeupSlider } })
        sliderRow(right, *label, *slider);
    statusRow(right, compStatus);

    header(right, evenLabel, &evenOnButton, origin(ma::bus::toolEvener));
    sliderRow(right, evenAmountLabel, evenAmountSlider);
    sliderRow(right, evenRefLabel, evenRefSlider);
    statusRow(right, evenStatus);
}

void AgentEditor::layoutStereo(juce::Rectangle<int> area)
{
    area.reduce(12, 12);
    {
        auto row = area.removeFromTop(24);
        origin(ma::bus::toolStereo).setBounds(row.removeFromRight(170));
        stereoLabel.setBounds(row);
    }
    area.removeFromTop(6);
    {
        auto row = area.removeFromTop(26);
        widthLabel.setBounds(row.removeFromLeft(110));
        widthSlider.setBounds(row);
        auto marks = area.removeFromTop(14).withTrimmedLeft(110).withTrimmedRight(70);
        widthMonoMark.setBounds(marks.removeFromLeft(80));
        widthMaxMark.setBounds(marks.removeFromRight(80));
        widthNeutralMark.setBounds(marks);
    }
    area.removeFromTop(4);
    {
        auto row = area.removeFromTop(24);
        widthFromLabel.setBounds(row.removeFromLeft(110));
        widthFromSlider.setBounds(row.removeFromLeft(360));
    }
    area.removeFromTop(4);
    widthStatusLabel.setBounds(area.removeFromTop(36));
    area.removeFromTop(6);
    stereoStatus.setBounds(area.removeFromBottom(40));
    area.removeFromBottom(6);
    stereoView.setBounds(area);
}

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

juce::Colour correlationColour(double value, double goodFrom)
{
    return value >= goodFrom ? colours::good : value >= 0.0 ? colours::warning : colours::critical;
}
} // namespace

BrainEditor::BrainEditor(BrainProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    styleLabel(title, pl("Mix Assistant Mózg"), colours::text, 18.0f);
    styleLabel(agentsLabel, {}, colours::quiet);
    agentsLabel.setJustificationType(juce::Justification::centredRight);
    styleLabel(integratedLabel, "LUFS integrated", colours::quiet);
    styleLabel(shortTermLabel, "LUFS short-term", colours::quiet);
    styleLabel(truePeakLabel, "True peak (max)", colours::quiet);
    styleLabel(plrLabel, "PLR", colours::quiet);
    styleLabel(lowCorrelationLabel, pl("Korelacja < 120 Hz"), colours::quiet);
    styleLabel(fullCorrelationLabel, pl("Korelacja całości"), colours::quiet);
    styleLabel(ceilingLabel, pl("Limit szczytu"), colours::quiet);
    styleLabel(overlayLabel, pl("Widma kanałów tak, jak słyszy je ucho (szare tło: suma)"), colours::quiet, 13.0f);
    styleLabel(issuesLabel, pl("Kto kogo zasłania. Kliknij wiersz, żeby zaznaczyć kanał."), colours::quiet, 13.0f);
    for (auto* value : { &integratedValue, &shortTermValue, &truePeakValue, &plrValue, &lowCorrelationValue, &fullCorrelationValue })
    {
        styleLabel(*value, {}, colours::text);
        value->setJustificationType(juce::Justification::centredRight);
    }


    ceilingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    ceilingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
    ceilingSlider.setColour(juce::Slider::thumbColourId, colours::accent);
    ceilingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, BrainProcessor::peakCeilingId, ceilingSlider);

    styleLabel(targetLabel, pl("Cel mastera"), colours::quiet);
    targetLabel.setTooltip(pl("Głośność, do której Ozone podciągnie master. Podgląd mastera sprawdza, ile limiter musiałby przy tym ściąć."));
    targetSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    targetSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
    targetSlider.setColour(juce::Slider::thumbColourId, colours::accent);
    targetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, BrainProcessor::masterTargetId, targetSlider);
    masteringReportButton.setButtonText(pl("Raport dla Ozone..."));
    masteringReportButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    masteringReportButton.setTooltip(pl("Zapisuje raport przed masteringiem (HTML): głośność, zapas, podgląd mastera, balans, stereo, "
                                        "referencje i co zostało do zrobienia w Ozone."));
    masteringReportButton.onClick = [this] { saveMasteringReport(); };

    resetButton.setButtonText(pl("Zeruj pomiar"));
    resetButton.setColour(juce::TextButton::buttonColourId, colours::panel);
    resetButton.onClick = [this] { processor.requestReset(); };

    profilesButton.setButtonText(pl("Z nauczonych profili"));
    profilesButton.setTooltip(pl("Kolizje liczone z profili nauczonych w Agentach zamiast z tego, co gra teraz."));
    profilesButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    profilesButton.setToggleState(processor.usesLearnedProfiles(), juce::dontSendNotification);
    profilesButton.onClick = [this] { processor.setUseLearnedProfiles(profilesButton.getToggleState()); };

    styleLabel(proposalStatus, {}, colours::quiet, 13.0f);

    const auto styleButton = [](juce::TextButton& button, const juce::String& text) {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, colours::panel);
        button.setColour(juce::TextButton::buttonOnColourId, colours::warning.darker(0.3f));
    };
    styleButton(proposeButton, pl("Zaproponuj"));
    styleButton(wholeMixButton, pl("Ustaw cały miks"));
    wholeMixButton.setColour(juce::TextButton::buttonColourId, colours::accent.darker(0.5f));
    wholeMixButton.setTooltip(pl("Inżynier miksu: porządki w dole i środku, kompresja, clipper i wyrównanie basu na każdym "
                                 "kanale, ustawione względem całego miksu, plus zwykła propozycja i werdykt dla każdego kanału."));
    styleButton(applyButton, pl("Zastosuj zaznaczone"));
    styleButton(undoButton, pl("Cofnij"));
    styleButton(compareButton, pl("Przed / po"));
    compareButton.setClickingTogglesState(true);
    compareButton.setTooltip(pl("Słuchaj wersji sprzed „Zastosuj”, z wyrównaną głośnością."));

    proposeButton.onClick = [this] {
        processor.propose();
        proposalStatus.setText({}, juce::dontSendNotification);
        tabs.setCurrentTabIndex(static_cast<int>(Tab::proposal));
        timerCallback();
    };
    wholeMixButton.onClick = [this] {
        processor.proposeWholeMix();
        proposalStatus.setText({}, juce::dontSendNotification);
        tabs.setCurrentTabIndex(static_cast<int>(Tab::verdict));
        timerCallback();
    };
    applyButton.onClick = [this] {
        const int sent = processor.applySelected();
        proposalStatus.setText(sent > 0 ? pl("Zastosowano na ") + juce::String(sent) + pl(" kanałach. Pomiar sumy wyzerowany.")
                                        : pl("Nic nie zaznaczono."),
                               juce::dontSendNotification);
        timerCallback();
    };
    undoButton.onClick = [this] {
        const int sent = processor.undoLast();
        proposalStatus.setText(pl("Cofnięto na ") + juce::String(sent) + pl(" kanałach."), juce::dontSendNotification);
        timerCallback();
    };
    compareButton.onClick = [this] { processor.setComparing(compareButton.getToggleState()); };
    proposalView.onToggle = [this](int index) {
        processor.toggleProposalItem(index);
        timerCallback();
    };
    adaptiveButton.setButtonText(pl("Tryb adaptacyjny"));
    adaptiveButton.setTooltip(pl("Mózg dalej słucha i powoli (0,25 dB na sekundę) przesuwa korekcje EQ w Agentach za miksem, "
                                 "w limitach solvera. Wyłączenie zamraża to, co jest ustawione."));
    adaptiveButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    adaptiveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, BrainProcessor::adaptiveId, adaptiveButton);

    proposalViewport.setViewedComponent(&proposalView, false);
    proposalViewport.setScrollBarsShown(true, false);

    stemBox.setTextWhenNothingSelected(pl("wybierz zestaw"));
    stemBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    stemBox.onChange = [this] {
        if (stemBox.getSelectedId() > 0)
            processor.selectStemTarget(stemBox.getText());
    };
    styleButton(newStemsButton, pl("Nowy zestaw"));
    styleButton(loadStemsButton, pl("Wczytaj stemy..."));
    styleButton(removeStemsButton, pl("Usuń ostatnią"));
    loadStemsButton.setTooltip(pl("Zaznacz stemy jednego utworu referencyjnego (drums, bass, vocals, other z FL Studio albo Demucs) "
                                  "i, jeśli masz, cały utwór („mix”) do sprawdzenia separacji."));
    newStemsButton.onClick = [this] { askForNewStemTarget(); };
    loadStemsButton.onClick = [this] { chooseStems(); };
    removeStemsButton.onClick = [this] { processor.removeLastStemProfile(); };
    refreshStemTargets();
    // Items: 1 = detect, then 2.. the 24 keys around the Camelot wheel (1A, 1B, 2A, ...).
    keyBox.addItem(pl("Tonacja: wykryj"), 1);
    for (int number = 1; number <= 12; ++number)
        for (bool minor : { true, false })
        {
            const auto key = ma::keyFromCamelot(number, minor);
            keyBox.addItem(juce::String(ma::camelotCode(key)) + "  " + juce::String(ma::keyName(key)), 2 + ma::keyIndex(key));
        }
    keyBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    keyBox.setTooltip(pl("Tonacja Twojego utworu. Profile referencji (bas, wokal, reszta) przesuwam do niej, "
                         "żeby bas referencji w innej tonacji nie udawał za dużo albo za mało suba. "
                         "Wykrywam ją z mastera; jeśli ją znasz, wybierz ręcznie."));
    keyBox.onChange = [this] {
        if (keyBox.getSelectedId() > 0)
            processor.setKeyOverride(keyBox.getSelectedId() - 2);
    };
    styleButton(redetectKeyButton, pl("Wykryj od nowa"));
    redetectKeyButton.setTooltip(pl("Zapomina tonację usłyszaną do tej pory (np. po zmianie utworu w projekcie) i słucha mastera od nowa."));
    redetectKeyButton.onClick = [this] {
        processor.restartKeyDetection();
        processor.setKeyOverride(-1);
    };
    for (auto* button : { &dropSectionButton, &breakSectionButton })
    {
        button->setClickingTogglesState(true);
        button->setRadioGroupId(4712);
        button->setColour(juce::TextButton::buttonColourId, colours::panel);
        button->setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.6f));
    }
    dropSectionButton.setButtonText("Drop");
    breakSectionButton.setButtonText("Break");
    breakSectionButton.setTooltip(pl("Break: część bez stopy. Porównuje proporcje w breaku i to, o ile jest cichszy od dropu."));
    dropSectionButton.setToggleState(true, juce::dontSendNotification);
    dropSectionButton.onClick = [this] { showBreak = false; };
    breakSectionButton.onClick = [this] { showBreak = true; };
    verdictView.onSelect = [this](std::uint64_t owner) { select(owner == selected ? 0 : owner); };
    verdictViewport.setViewedComponent(&verdictView, false);
    verdictViewport.setScrollBarsShown(true, false);

    styleLabel(previewLabel, pl("Odsłuch"), colours::quiet);
    previewLabel.setJustificationType(juce::Justification::centredRight);
    previewBox.addItemList(BrainProcessor::systemNames(), 1);
    previewBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    previewBox.setSelectedId(processor.previewSystemIndex() + 1, juce::dontSendNotification);
    previewBox.setTooltip(pl("Posłuchaj miksu tak, jak zagra na innym systemie. Pomiary dalej widzą prawdziwy miks."));
    previewBox.onChange = [this] { processor.setPreviewSystem(previewBox.getSelectedId() - 1); };

    targetBox.setTextWhenNothingSelected(pl("wybierz cel"));
    targetBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    targetBox.onChange = [this] {
        if (targetBox.getSelectedId() > 0)
            processor.selectTarget(targetBox.getText());
    };
    styleButton(newTargetButton, pl("Nowy cel"));
    styleButton(captureButton, pl("Nagraj referencję"));
    styleButton(removeReferenceButton, pl("Usuń ostatnią"));
    newTargetButton.onClick = [this] { askForNewTarget(); };
    captureButton.onClick = [this] {
        if (!processor.isCapturing())
            processor.startCapture();
        else if (!processor.finishCapture())
            processor.cancelCapture();
        timerCallback();
    };
    removeReferenceButton.onClick = [this] { processor.removeLastReference(); };
    refreshTargets();

    table.onSelect = [this](std::uint64_t owner) { select(owner); };
    issues.onSelect = [this](std::uint64_t owner) { select(owner == selected ? 0 : owner); };
    reportViewport.setViewedComponent(&report, false);
    reportViewport.setScrollBarsShown(true, false);

    // One fixed-size window: the header, the meters and the proposal's buttons stay, the rest sits on tabs.
    for (auto* c : std::initializer_list<juce::Component*> {
             &title, &previewLabel, &previewBox, &agentsLabel, &masteringReportButton, &resetButton, &integratedLabel,
             &integratedValue, &shortTermLabel, &shortTermValue, &truePeakLabel, &truePeakValue, &plrLabel, &plrValue,
             &lowCorrelationLabel, &lowCorrelationValue, &fullCorrelationLabel, &fullCorrelationValue, &ceilingLabel,
             &ceilingSlider, &targetLabel, &targetSlider, &tabs, &wholeMixButton, &proposeButton, &applyButton, &undoButton,
             &compareButton, &proposalStatus, &adaptiveButton })
        addAndMakeVisible(c);
    const auto page = [this](Tab tab, const juce::String& name, const juce::String& tooltip,
                             std::initializer_list<juce::Component*> children, std::function<void(juce::Rectangle<int>)> layout) {
        auto& target = pages[static_cast<size_t>(tab)];
        for (auto* c : children)
            target.addAndMakeVisible(c);
        target.layout = std::move(layout);
        tabs.add(name, target, tooltip);
    };
    const auto fill = [](juce::Component& c) { return [&c](juce::Rectangle<int> area) { c.setBounds(area.reduced(10)); }; };
    page(Tab::report, pl("Raport"), pl("Raport poprawności: co jest w porządku, a co trzeba poprawić."), { &reportViewport },
         [this](juce::Rectangle<int> area) {
             reportViewport.setBounds(area.reduced(10));
             layoutReport();
         });
    page(Tab::channels, pl("Kanały"), {}, { &table, &overlayLabel, &profilesButton, &overlay },
         [this](auto area) { layoutChannels(area); });
    page(Tab::collisions, pl("Kolizje"), pl("Kto kogo zasłania i co już z tym robi."), { &issuesLabel, &issues },
         [this](juce::Rectangle<int> area) {
             area.reduce(10, 10);
             issuesLabel.setBounds(area.removeFromTop(20));
             area.removeFromTop(4);
             issues.setBounds(area);
         });
    page(Tab::proposal, pl("Propozycja"), pl("Zmiany z „Zaproponuj” i „Ustaw cały miks”. Odznacz to, czego nie chcesz."),
         { &proposalViewport }, [this](juce::Rectangle<int> area) {
             proposalViewport.setBounds(area.reduced(10));
             layoutProposal();
         });
    page(Tab::verdict, pl("Werdykt"), pl("Co „Ustaw cały miks” ustawił na każdym kanale i dlaczego."), { &verdictViewport },
         fill(verdictViewport));
    page(Tab::tonal, pl("Balans tonalny"), pl("Balans tonalny sumy wobec celu z referencji."),
         { &targetBox, &newTargetButton, &captureButton, &removeReferenceButton, &tonalView }, [this](auto area) { layoutTonal(area); });
    page(Tab::stems, pl("Referencje"), pl("Perkusja, bas, wokal i reszta wobec stemów referencji, drop do dropu: poziom, barwa, szerokość."),
         { &stemBox, &newStemsButton, &loadStemsButton, &removeStemsButton, &keyBox, &redetectKeyButton, &dropSectionButton,
           &breakSectionButton, &stemView },
         [this](auto area) { layoutStems(area); });
    page(Tab::translation, pl("Translacja"), pl("Jak miks zagra na telefonie, laptopie, słuchawkach, w aucie i w klubie."),
         { &translationView }, fill(translationView));
    page(Tab::stereo, "Stereo", pl("Faza w pasmach sumy, jak Correlometer: na dole przeciwfaza. Kropki: zaznaczony kanał."),
         { &stereoView }, fill(stereoView));
    tabs.setCurrentTabIndex(juce::jlimit(0, tabs.getNumTabs() - 1, processor.editorTab), false);
    tabs.onChange = [this](int index) { processor.editorTab = index; };

    processor.setEditorOpen(true);
    setSize(kWidth, kHeight);
    startTimerHz(10);
}

BrainEditor::~BrainEditor()
{
    stopTimer();
    if (processor.isComparing())
        processor.setComparing(false);
    processor.setEditorOpen(false);
}

void BrainEditor::refreshTargets()
{
    targetBox.clear(juce::dontSendNotification);
    const auto names = processor.targetNames();
    targetBox.addItemList(names, 1);
    const auto current = juce::String::fromUTF8(processor.target().name.c_str());
    shownTarget = current;
    const int index = names.indexOf(current);
    if (index >= 0)
        targetBox.setSelectedId(index + 1, juce::dontSendNotification);
}

void BrainEditor::refreshStemTargets()
{
    stemBox.clear(juce::dontSendNotification);
    const auto names = processor.stemTargetNames();
    stemBox.addItemList(names, 1);
    const auto current = juce::String::fromUTF8(processor.stemTarget().name.c_str());
    shownStems = current;
    const int index = names.indexOf(current);
    if (index >= 0)
        stemBox.setSelectedId(index + 1, juce::dontSendNotification);
}

void BrainEditor::askForNewStemTarget()
{
    nameDialog = std::make_unique<juce::AlertWindow>(pl("Nowy zestaw referencji"),
                                                     pl("Nazwa zestawu, np. „Tech house”. Potem wczytaj do niego stemy kilku utworów."),
                                                     juce::MessageBoxIconType::NoIcon, this);
    nameDialog->addTextEditor("name", {}, pl("Nazwa"));
    nameDialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    nameDialog->addButton(pl("Anuluj"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    nameDialog->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
        if (result == 1 && nameDialog != nullptr && processor.createStemTarget(nameDialog->getTextEditorContents("name")))
            refreshStemTargets();
        nameDialog.reset();
    }));
}

void BrainEditor::chooseStems()
{
    stemChooser = std::make_unique<juce::FileChooser>(pl("Stemy jednego utworu referencyjnego"), juce::File(),
                                                      "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    const auto mode = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::canSelectMultipleItems;
    stemChooser->launchAsync(mode, [this](const juce::FileChooser& chooser) {
        const auto files = chooser.getResults();
        if (!files.isEmpty())
            processor.loadStems(files);
        timerCallback();
    });
}

void BrainEditor::askForNewTarget()
{
    nameDialog = std::make_unique<juce::AlertWindow>(pl("Nowy cel"), pl("Nazwa celu, np. „House 124”. Potem nagraj do niego referencje."),
                                                     juce::MessageBoxIconType::NoIcon, this);
    nameDialog->addTextEditor("name", {}, pl("Nazwa"));
    nameDialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    nameDialog->addButton(pl("Anuluj"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    nameDialog->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
        if (result == 1 && nameDialog != nullptr && processor.createTarget(nameDialog->getTextEditorContents("name")))
            refreshTargets();
        nameDialog.reset();
    }));
}

void BrainEditor::select(std::uint64_t owner)
{
    selected = owner;
    timerCallback();
}

void BrainEditor::timerCallback()
{
    const double integrated = processor.integratedLufs();
    const double peak = processor.truePeakDb();
    integratedValue.setText(ma::ui::formatDb(integrated, "LUFS"), juce::dontSendNotification);
    shortTermValue.setText(ma::ui::formatDb(processor.shortTermLufs(), "LUFS"), juce::dontSendNotification);
    truePeakValue.setText(ma::ui::formatDb(peak, "dBTP"), juce::dontSendNotification);
    truePeakValue.setColour(juce::Label::textColourId,
                            std::isfinite(peak) && peak > processor.peakCeilingDb() ? colours::critical : colours::text);
    plrValue.setText(std::isfinite(integrated) && std::isfinite(peak) ? juce::String(peak - integrated, 1) + " dB" : "-",
                     juce::dontSendNotification);

    const double low = processor.lowCorrelation();
    const double full = processor.fullCorrelation();
    lowCorrelationValue.setText(juce::String(low, 2), juce::dontSendNotification);
    lowCorrelationValue.setColour(juce::Label::textColourId, correlationColour(low, 0.7));
    fullCorrelationValue.setText(juce::String(full, 2), juce::dontSendNotification);
    fullCorrelationValue.setColour(juce::Label::textColourId, correlationColour(full, 0.2));

    const auto& mix = processor.snapshot();
    const bool selectionAlive = std::any_of(mix.channels.begin(), mix.channels.end(),
                                            [this](const auto& c) { return c.owner == selected; });
    if (!selectionAlive)
        selected = 0;

    agentsLabel.setText(pl("Agenci: ") + juce::String(static_cast<int>(mix.channels.size())), juce::dontSendNotification);
    report.setItems(mix.report);
    layoutReport();
    table.setChannels(mix.channels, selected);

    std::array<double, ma::SpectrumAnalyzer::kMaxBands> bands {}, centres {};
    const int masterBands = BrainProcessor::numMasterBands();
    for (int b = 0; b < masterBands; ++b)
    {
        bands[static_cast<size_t>(b)] = processor.bandDb(b);
        centres[static_cast<size_t>(b)] = ma::SpectrumAnalyzer::bandCentreHz(ma::kTonalScale, b);
    }
    overlay.setData(mix, selected, bands.data(), centres.data(), masterBands);

    const auto& target = processor.target();
    const auto targetName = juce::String::fromUTF8(target.name.c_str());
    if (shownTarget != targetName)
        refreshTargets();
    juce::String tonalStatus;
    if (processor.isCapturing())
        tonalStatus = pl("Nagrywam referencję do „") + targetName + pl("”: ") + juce::String(juce::roundToInt(processor.secondsAnalysed()))
                      + pl(" s. Puść cały utwór (albo drop) na masterze i kliknij „Zakończ nagrywanie”.");
    else if (targetName.isEmpty())
        tonalStatus = pl("Utwórz cel („Nowy cel”) i nagraj do niego referencje, jak Capture w Tonal Balance.");
    else if (!target.valid())
        tonalStatus = pl("Cel „") + targetName + pl("” nie ma referencji. Kliknij „Nagraj referencję”.");
    else if (!mix.tonal.valid)
        tonalStatus = pl("Cel „") + targetName + "\" (" + juce::String(static_cast<int>(target.references.size()))
                      + pl(" ref.). Słucham miksu: ") + juce::String(juce::roundToInt(processor.secondsAnalysed())) + " / "
                      + juce::String(juce::roundToInt(BrainProcessor::kMinimumSeconds)) + " s";
    else
        tonalStatus = pl("Cel „") + targetName + "\" (" + juce::String(static_cast<int>(target.references.size()))
                      + pl(" ref.), miks z ") + juce::String(juce::roundToInt(processor.secondsAnalysed())) + " s";
    tonalView.setData(target, mix.tonal, tonalStatus);

    const auto& stems = processor.stemTarget();
    const auto stemsName = juce::String::fromUTF8(stems.name.c_str());
    if (shownStems != stemsName)
        refreshStemTargets();
    juce::String stemStatus;
    bool stemWarning = false;
    if (processor.isAnalysingStems())
        stemStatus = pl("Analizuję stemy: ") + juce::String(juce::roundToInt(processor.stemProgress() * 100.0)) + "%";
    else if (stemsName.isEmpty())
        stemStatus = pl("Utwórz zestaw („Nowy zestaw”) i wczytaj stemy utworów referencyjnych (drums, bass, vocals, other).");
    else if (processor.stemMessage().isNotEmpty())
    {
        stemStatus = processor.stemMessage();
        stemWarning = processor.stemMessageIsWarning();
    }
    else if (!stems.valid())
        stemStatus = pl("Zestaw „") + stemsName + pl("” jest pusty. Kliknij „Wczytaj stemy...”.");
    else if (!mix.stems.valid)
        stemStatus = pl("Zestaw „") + stemsName + "\" (" + juce::String(static_cast<int>(stems.profiles.size()))
                     + pl(" ref.). Puść drop swojego utworu: ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " / "
                     + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds)) + " s";
    else
        stemStatus = pl("Zestaw „") + stemsName + "\" (" + juce::String(static_cast<int>(stems.profiles.size()))
                     + pl(" ref.), drop miksu: ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " s";
    juce::String keyStatus;
    bool keyWarning = false;
    updateKeyControls(keyStatus, keyWarning);
    if (showBreak)
    {
        const auto& breaks = processor.stemBreakTarget();
        if (!stems.valid())
        {
            // the drop's status already says what is missing
        }
        else if (!breaks.valid())
            stemStatus = pl("Żadna referencja w zestawie nie ma breaku (części bez perkusji, co najmniej 8 s).");
        else if (!mix.breakStems.valid)
            stemStatus = pl("Break: puść część bez stopy po dropie: ") + juce::String(juce::roundToInt(mix.breakSeconds)) + " / "
                         + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds)) + " s";
        else
        {
            const auto& contrast = breaks.breakContrast;
            stemStatus = pl("Break miksu: ") + juce::String(juce::roundToInt(mix.breakSeconds)) + pl(" s, ") + juce::String(mix.breakBelowDropDb, 1)
                         + pl(" dB ciszej niż drop") + (contrast.valid ? pl(" (referencje ") + juce::String(contrast.low, 1) + pl("–")
                                                                              + juce::String(contrast.high, 1) + " dB)"
                                                                        : juce::String())
                         + ".";
            stemWarning = contrast.valid && std::abs(contrast.outside(mix.breakBelowDropDb)) >= 1.0;
        }
        stemView.setData(breaks, mix.breakStems, stemStatus, stemWarning, keyStatus, keyWarning);
    }
    else
        stemView.setData(stems, mix.stems, stemStatus, stemWarning, keyStatus, keyWarning);
    loadStemsButton.setEnabled(stemsName.isNotEmpty() && !processor.isAnalysingStems());
    removeStemsButton.setEnabled(stems.valid() && !processor.isAnalysingStems());
    translationView.setData(mix);
    {
        std::array<double, ma::ui::CoherenceView::kBands> sum {}, channel {};
        for (size_t b = 0; b < sum.size(); ++b)
            sum[b] = ma::coherence(mix.master.midDb[b], mix.master.sideDb[b]);
        const auto chosen = std::find_if(mix.channels.begin(), mix.channels.end(),
                                         [this](const auto& c) { return c.owner == selected && c.stereoValid; });
        if (chosen != mix.channels.end())
            for (size_t b = 0; b < channel.size(); ++b)
                channel[b] = ma::coherence(chosen->midDb[b], chosen->sideDb[b]
                                           + ma::sideResponseDb(chosen->data.stereo,
                                                                ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, static_cast<int>(b))));
        stereoView.setBands(sum.data(), mix.master.valid, chosen != mix.channels.end() ? channel.data() : nullptr);
    }
    {
        const auto& made = processor.proposal();
        verdictView.setVerdicts(made.verdicts, made.wholeMix, selected);
        const int width = verdictViewport.getMaximumVisibleWidth();
        verdictView.setSize(width, 10);
        verdictView.setSize(width, std::max(verdictViewport.getHeight(), verdictView.contentHeight()));
    }
    if (previewBox.getSelectedId() != processor.previewSystemIndex() + 1)
        previewBox.setSelectedId(processor.previewSystemIndex() + 1, juce::dontSendNotification);
    previewBox.setColour(juce::ComboBox::backgroundColourId,
                         processor.previewSystemIndex() != 0 ? colours::warning.darker(0.5f) : colours::panel);
    captureButton.setButtonText(processor.isCapturing() ? pl("Zakończ nagrywanie") : pl("Nagraj referencję"));
    captureButton.setToggleState(processor.isCapturing(), juce::dontSendNotification);
    captureButton.setEnabled(targetName.isNotEmpty());
    removeReferenceButton.setEnabled(!target.references.empty() && !processor.isCapturing());

    std::vector<brain::IssuesView::Line> lines;
    for (const auto& issue : mix.masking.issues)
    {
        brain::IssuesView::Line line;
        line.text = processor.describeIssue(issue);
        line.score = static_cast<float>(issue.score);
        const auto handling = processor.issueHandling(issue);
        line.ducked = handling == BrainProcessor::IssueHandling::ducked;
        line.cutAtLimit = handling == BrainProcessor::IssueHandling::cutAtLimit;
        line.masker = mix.channels[static_cast<size_t>(issue.masker)].owner;
        line.involvesSelected = selected != 0
                                && (line.masker == selected || mix.channels[static_cast<size_t>(issue.masked)].owner == selected);
        lines.push_back(line);
    }
    issues.setLines(std::move(lines));

    const auto& proposal = processor.proposal();
    proposalView.setProposal(proposal);
    layoutProposal();
    const bool anySelected = std::any_of(proposal.items.begin(), proposal.items.end(), [](const auto& i) { return i.selected; });
    applyButton.setEnabled(anySelected);
    const bool undoable = processor.canUndo();
    undoButton.setEnabled(undoable);
    compareButton.setEnabled(undoable || processor.isComparing());
    compareButton.setToggleState(processor.isComparing(), juce::dontSendNotification);
    if (processor.isComparing())
    {
        const double gain = processor.compareGainDb();
        proposalStatus.setText(std::isfinite(gain) ? pl("Słychać wersję PRZED, głośność wyrównana o ") + juce::String(gain, 1) + " dB."
                                                   : pl("Słychać wersję PRZED. Mierzę głośność, wyrównanie za kilka sekund..."),
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::warning);
    }
    else if (proposal.made && !proposal.items.empty() && proposalStatus.getText().isEmpty())
    {
        proposalStatus.setText(pl("Kolizje: ") + juce::String(proposal.crowdingBefore, 2) + pl(" → ")
                                   + juce::String(proposal.crowdingAfter, 2) + pl(". Odznacz to, czego nie chcesz."),
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::quiet);
    }
    else if (!processor.isComparing() && proposalStatus.getText().startsWith(pl("Słychać")))
    {
        proposalStatus.setText({}, juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::quiet);
    }
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
    header.removeFromRight(8);
    masteringReportButton.setBounds(header.removeFromRight(150));
    header.removeFromRight(12);
    agentsLabel.setBounds(header.removeFromRight(90));
    header.removeFromRight(12);
    previewBox.setBounds(header.removeFromRight(160));
    header.removeFromRight(6);
    previewLabel.setBounds(header.removeFromRight(70));
    title.setBounds(header);
    area.removeFromTop(8);

    // Meters in two rows of four columns; the two right columns are wider for the sliders.
    const int narrow = 210, wide = (area.getWidth() - 2 * narrow - 3 * 24) / 2;
    using Cell = std::pair<juce::Component*, juce::Component*>;
    const std::array<std::array<Cell, 4>, 2> cells { {
        { { { &integratedLabel, &integratedValue }, { &shortTermLabel, &shortTermValue }, { &truePeakLabel, &truePeakValue },
            { &plrLabel, &plrValue } } },
        { { { &lowCorrelationLabel, &lowCorrelationValue }, { &fullCorrelationLabel, &fullCorrelationValue },
            { &ceilingLabel, &ceilingSlider }, { &targetLabel, &targetSlider } } },
    } };
    for (const auto& line : cells)
    {
        auto row = area.removeFromTop(26);
        for (size_t i = 0; i < line.size(); ++i)
        {
            auto cell = row.removeFromLeft(i < 2 ? narrow : wide);
            row.removeFromLeft(24);
            line[i].first->setBounds(cell.removeFromLeft(i < 2 ? 140 : 110));
            line[i].second->setBounds(cell);
        }
    }
    area.removeFromTop(8);

    auto actions = area.removeFromBottom(28);
    for (auto* button : { &wholeMixButton, &proposeButton, &applyButton, &undoButton, &compareButton })
    {
        button->setBounds(actions.removeFromLeft(button == &applyButton || button == &wholeMixButton ? 150 : 100));
        actions.removeFromLeft(6);
    }
    adaptiveButton.setBounds(actions.removeFromRight(150));
    proposalStatus.setBounds(actions.withTrimmedLeft(6));
    area.removeFromBottom(8);
    tabs.setBounds(area);
}

void BrainEditor::layoutChannels(juce::Rectangle<int> area)
{
    area.reduce(10, 10);
    table.setBounds(area.removeFromTop(190));
    area.removeFromTop(8);
    auto row = area.removeFromTop(22);
    profilesButton.setBounds(row.removeFromRight(170));
    overlayLabel.setBounds(row);
    area.removeFromTop(2);
    overlay.setBounds(area);
}

void BrainEditor::layoutTonal(juce::Rectangle<int> area)
{
    area.reduce(10, 10);
    auto controls = area.removeFromTop(26);
    targetBox.setBounds(controls.removeFromLeft(200));
    controls.removeFromLeft(6);
    newTargetButton.setBounds(controls.removeFromLeft(100));
    controls.removeFromLeft(6);
    captureButton.setBounds(controls.removeFromLeft(160));
    controls.removeFromLeft(6);
    removeReferenceButton.setBounds(controls.removeFromLeft(120));
    area.removeFromTop(6);
    tonalView.setBounds(area);
}

void BrainEditor::layoutStems(juce::Rectangle<int> area)
{
    area.reduce(10, 10);
    auto controls = area.removeFromTop(26);
    stemBox.setBounds(controls.removeFromLeft(200));
    controls.removeFromLeft(6);
    newStemsButton.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(6);
    loadStemsButton.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(6);
    removeStemsButton.setBounds(controls.removeFromLeft(110));
    area.removeFromTop(4);
    auto keyRow = area.removeFromTop(26);
    keyBox.setBounds(keyRow.removeFromLeft(200));
    keyRow.removeFromLeft(6);
    redetectKeyButton.setBounds(keyRow.removeFromLeft(130));
    breakSectionButton.setBounds(keyRow.removeFromRight(70));
    dropSectionButton.setBounds(keyRow.removeFromRight(70));
    area.removeFromTop(6);
    stemView.setBounds(area);
}

void BrainEditor::layoutReport()
{
    const int width = reportViewport.getMaximumVisibleWidth();
    report.setSize(width, 10);
    report.setSize(width, std::max(reportViewport.getHeight(), report.contentHeight()));
}

void BrainEditor::layoutProposal()
{
    const int width = proposalViewport.getMaximumVisibleWidth();
    proposalView.setSize(width, 10);
    proposalView.setSize(width, std::max(proposalViewport.getHeight(), proposalView.contentHeight()));
}

void BrainEditor::updateKeyControls(juce::String& status, bool& warning)
{
    const auto& heard = processor.detectedKey();
    const double seconds = processor.keySecondsHeard();
    const int manual = processor.keyOverrideIndex();
    const bool sure = seconds >= BrainProcessor::kMinimumKeySeconds && heard.confident;
    keyBox.changeItemText(1, sure ? pl("Wykryta: ") + BrainProcessor::keyText(heard.key) : pl("Tonacja: wykryj"));
    if (keyBox.getSelectedId() != manual + 2)
        keyBox.setSelectedId(manual + 2, juce::dontSendNotification);

    const auto key = processor.trackKey();
    warning = false;
    if (!key.valid())
    {
        warning = seconds >= BrainProcessor::kMinimumKeySeconds;
        status = seconds < BrainProcessor::kMinimumKeySeconds
                     ? pl("Tonacja utworu: słucham mastera (") + juce::String(juce::roundToInt(seconds)) + " / "
                           + juce::String(juce::roundToInt(BrainProcessor::kMinimumKeySeconds)) + pl(" s). Do tego czasu referencji nie przesuwam.")
                     : pl("Tonacja utworu niepewna (najbliżej ") + BrainProcessor::keyText(heard.key)
                           + pl("). Wybierz ją ręcznie, wtedy przesunę referencje.");
        return;
    }
    status = pl("Tonacja utworu: ") + BrainProcessor::keyText(key) + (manual >= 0 ? pl(" (ręcznie)") : pl(" (wykryta)")) + ". ";
    const auto& stems = processor.stemTarget();
    if (stems.profiles.empty())
        return;
    juce::StringArray moved;
    int unknown = 0;
    for (const auto& p : stems.profiles)
    {
        const auto name = juce::String::fromUTF8(p.name.c_str());
        if (!p.key.valid() || !p.hasBands)
            ++unknown;
        else if (p.shiftSemitones != 0)
            moved.add(name + " " + BrainProcessor::keyText(p.key) + ": " + (p.shiftSemitones > 0 ? "+" : "")
                      + juce::String(p.shiftSemitones) + pl(" półt."));
    }
    status += moved.isEmpty() ? pl("Referencje są w tej samej tonacji (albo równoległej), nic nie przesuwam.")
                              : pl("Przesunięte: ") + moved.joinIntoString(", ") + ".";
    if (unknown > 0)
    {
        status += " " + juce::String(unknown) + pl(" bez przesunięcia (tonacja niepewna albo zestaw sprzed tej wersji).");
        warning = true;
    }
}

void BrainEditor::saveMasteringReport()
{
    const auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto name = pl("Raport przed masteringiem ") + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M") + ".html";
    reportChooser = std::make_unique<juce::FileChooser>(pl("Zapisz raport dla Ozone"), folder.getChildFile(name), "*.html");
    const auto mode = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;
    reportChooser->launchAsync(mode, [this](const juce::FileChooser& chooser) {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        if (!file.hasFileExtension("html;htm"))
            file = file.withFileExtension("html");
        if (processor.saveMasteringReport(file))
            file.startAsProcess();
        else
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, pl("Raport dla Ozone"),
                                                   pl("Nie mogę zapisać ") + file.getFullPathName());
    });
}

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
    styleLabel(issuesLabel, pl("Kto kogo zasłania. Zielone: rozwiązane. Niebieskie: miks zrobił, ile mógł (albo mały krok czeka w propozycji). "
                                     "Pomarańczowe i czerwone: jest coś do zrobienia. Kliknij wiersz, żeby zaznaczyć kanał."), colours::quiet, 13.0f);
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

    afterLimiterButton.setButtonText(pl("Jak po limiterze (master)"));
    afterLimiterButton.setTooltip(pl("Referencje to gotowe mastery, więc Mózg porównuje miks tak, jak zostawiłby go limiter do celu "
                                     "głośności z zakładki pre-mastera (szacunek; limiter zjada głównie dół). Cienka linia: miks tak, "
                                     "jak gra teraz, czyli to, co pokazuje Tonal Balance przed Ozone."));
    afterLimiterButton.setColour(juce::ToggleButton::textColourId, colours::text);
    afterLimiterButton.setToggleState(processor.tonalAfterLimiter(), juce::dontSendNotification);
    afterLimiterButton.onClick = [this] { processor.setTonalAfterLimiter(afterLimiterButton.getToggleState()); };

    sketchButton.setButtonText(pl("Tryb szkicu: brakujące partie jako duchy"));
    sketchButton.setTooltip(pl("Gdy dropowi brakuje partii, które mają referencje (albo typowy drop house'u), Mózg wstawia za nie "
                               "partie-duchy: balans tonalny, proporcje i kolizje liczy z nimi, a „Analizuj” (cały łańcuch) zostawia im miejsce "
                               "i nie łata EQ-em dziur aranżu. Wyłączony: miks oceniany tak, jak gra."));
    sketchButton.setColour(juce::ToggleButton::textColourId, colours::text);
    sketchButton.setToggleState(processor.sketchMode(), juce::dontSendNotification);
    sketchButton.onClick = [this] { processor.setSketchMode(sketchButton.getToggleState()); };
    listenGhostsButton.setButtonText(pl("Słuchaj duchów (szum w ich kształcie, tylko w dropie)"));
    listenGhostsButton.setTooltip(pl("W odsłuchu Mózgu grają brakujące partie jako szum o ich widmie, poziomie i szerokości "
                                     "(perkusja pulsuje na bit). Słychać, jak drop zabrzmi z nimi. Pomiary go nie słyszą; "
                                     "wyłącz przed eksportem."));
    listenGhostsButton.setColour(juce::ToggleButton::textColourId, colours::text);
    listenGhostsButton.setToggleState(processor.listenGhosts(), juce::dontSendNotification);
    listenGhostsButton.onClick = [this] { processor.setListenGhosts(listenGhostsButton.getToggleState()); };

    styleLabel(proposalStatus, {}, colours::quiet, 13.0f);

    const auto styleButton = [](juce::TextButton& button, const juce::String& text) {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, colours::panel);
        button.setColour(juce::TextButton::buttonOnColourId, colours::warning.darker(0.3f));
    };
    scopeBox.addItem(pl("Cały łańcuch"), 2);
    scopeBox.addItem(pl("Tylko korekty"), 1);
    scopeBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    scopeBox.setSelectedId(processor.analysisScope + 1, juce::dontSendNotification);
    scopeBox.setTooltip(pl("Cały łańcuch: inżynier miksu ustawia każdy kanał (porządki w dole i środku, kompresja, clipper, "
                           "bas, stereo, poziomy grup) względem całego miksu, plus korekty i werdykt dla każdego kanału.\n"
                           "Tylko korekty: cięcia EQ, ustępowanie, polaryzacja i translacja, bez ruszania łańcucha."));
    scopeBox.onChange = [this] { processor.analysisScope = scopeBox.getSelectedId() - 1; };
    styleButton(analyseButton, pl("Analizuj"));
    analyseButton.setTooltip(pl("Liczy propozycję na tym, co gra (najlepiej drop). Niczego nie zmienia, dopóki nie klikniesz „Zastosuj”."));
    styleButton(applyButton, pl("Zastosuj"));
    applyButton.setTooltip(pl("Wysyła zaznaczone zmiany do Agentów. Potem puść drop jeszcze raz: Mózg zmierzy efekt."));
    styleButton(undoButton, pl("Cofnij"));
    styleButton(compareButton, pl("Przed / po"));
    compareButton.setClickingTogglesState(true);
    compareButton.setTooltip(pl("Słuchaj wersji sprzed „Zastosuj”, z wyrównaną głośnością."));

    analyseButton.onClick = [this] {
        processor.stopRefining();
        const bool chain = processor.analysisScope == 1;
        processor.analyse(processor.analysisScope);
        measuringEffect = false;
        proposalStatus.setText({}, juce::dontSendNotification);
        tabs.setCurrentTabIndex(static_cast<int>(chain ? Tab::verdict : Tab::proposal));
        timerCallback();
    };
    applyButton.onClick = [this] {
        const int sent = processor.applyByUser();
        if (sent > 0)
        {
            measuringEffect = true;
            effectReset = false;
            effectNote = pl("Zastosowano na ") + juce::String(sent) + pl(" kanałach.");
        }
        proposalStatus.setText(sent > 0 ? effectNote : pl("Nic nie zaznaczono."), juce::dontSendNotification);
        timerCallback();
    };
    undoButton.onClick = [this] {
        const int sent = processor.undoLast();
        measuringEffect = sent > 0;
        effectReset = false;
        effectNote = pl("Cofnięto na ") + juce::String(sent) + pl(" kanałach.");
        proposalStatus.setText(effectNote, juce::dontSendNotification);
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
    autoRefineButton.setButtonText(pl("Dopracuj sam"));
    autoRefineButton.setTooltip(pl("Po „Zastosuj” Mózg mierzy drop jeszcze raz i, jeśli coś zostało (np. clipper pod głośny master, "
                                   "kolizja), analizuje i stosuje sam, najwyżej 2 rundy. Jedno „Cofnij” cofa wszystko."));
    autoRefineButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    autoRefineAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, BrainProcessor::autoRefineId, autoRefineButton);

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
    for (auto* button : { &introSectionButton, &buildSectionButton, &dropSectionButton, &breakSectionButton })
    {
        button->setClickingTogglesState(true);
        button->setRadioGroupId(4712);
        button->setColour(juce::TextButton::buttonColourId, colours::panel);
        button->setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.6f));
    }
    introSectionButton.setButtonText("Intro");
    buildSectionButton.setButtonText("Build");
    dropSectionButton.setButtonText("Drop");
    breakSectionButton.setButtonText("Break");
    introSectionButton.setTooltip(pl("Intro: to, co gra przed pierwszym dropem (bez buildu). Mózg zna je z pozycji w utworze."));
    buildSectionButton.setTooltip(pl("Build: ostatnie 8 s przed dropem. Porównuje proporcje i to, o ile build jest cichszy od dropu."));
    breakSectionButton.setTooltip(pl("Break: część bez stopy. Porównuje proporcje w breaku i to, o ile jest cichszy od dropu."));
    dropSectionButton.setToggleState(true, juce::dontSendNotification);
    introSectionButton.onClick = [this] { shownSection = static_cast<int>(ma::StemSection::intro); };
    buildSectionButton.onClick = [this] { shownSection = static_cast<int>(ma::StemSection::build); };
    dropSectionButton.onClick = [this] { shownSection = -1; };
    breakSectionButton.onClick = [this] { shownSection = static_cast<int>(ma::StemSection::breakdown); };
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
    report.onResize = [this] { layoutReport(); };
    reportViewport.setScrollBarsShown(true, false);

    // One fixed-size window: the header, the meters and the proposal's buttons stay, the rest sits on tabs.
    for (auto* c : std::initializer_list<juce::Component*> {
             &title, &previewLabel, &previewBox, &agentsLabel, &masteringReportButton, &resetButton, &integratedLabel,
             &integratedValue, &shortTermLabel, &shortTermValue, &truePeakLabel, &truePeakValue, &plrLabel, &plrValue,
             &lowCorrelationLabel, &lowCorrelationValue, &fullCorrelationLabel, &fullCorrelationValue, &ceilingLabel,
             &ceilingSlider, &targetLabel, &targetSlider, &tabs, &scopeBox, &analyseButton, &applyButton, &undoButton,
             &compareButton, &proposalStatus, &adaptiveButton, &autoRefineButton, &stepPath })
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
    page(Tab::guide, pl("Jak zacząć"), pl("Co zrobić po kolei, żeby wtyczka dała najlepszy wynik. Kroki odhaczają się same."), { &guideView },
         fill(guideView));
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
    page(Tab::proposal, pl("Propozycja"), pl("Zmiany z „Analizuj”. Niczego jeszcze nie zmieniają: odznacz to, czego nie chcesz, i kliknij „Zastosuj”."),
         { &proposalViewport }, [this](juce::Rectangle<int> area) {
             proposalViewport.setBounds(area.reduced(10));
             layoutProposal();
         });
    page(Tab::verdict, pl("Werdykt"), pl("Co Mózg zaplanował na każdym kanale i dlaczego. Przy każdym narzędziu: działa, do zastosowania, zmienione ręcznie albo nie działa."), { &verdictViewport },
         fill(verdictViewport));
    page(Tab::tonal, pl("Balans tonalny"), pl("Balans tonalny sumy wobec celu z referencji."),
         { &targetBox, &newTargetButton, &captureButton, &removeReferenceButton, &afterLimiterButton, &tonalView }, [this](auto area) { layoutTonal(area); });
    page(Tab::stems, pl("Referencje"), pl("Perkusja, bas, wokal i reszta wobec stemów referencji, drop do dropu: poziom, barwa, szerokość."),
         { &stemBox, &newStemsButton, &loadStemsButton, &removeStemsButton, &keyBox, &redetectKeyButton, &introSectionButton,
           &buildSectionButton, &dropSectionButton, &breakSectionButton, &stemView },
         [this](auto area) { layoutStems(area); });
    page(Tab::arrangement, pl("Aranż"), pl("Tryb szkicu: czego dropowi brakuje względem referencji i co dograć."),
         { &sketchButton, &listenGhostsButton, &arrangementView }, [this](juce::Rectangle<int> area) {
             area.reduce(10, 10);
             auto row = area.removeFromTop(24);
             sketchButton.setBounds(row.removeFromLeft(420));
             row.removeFromLeft(12);
             listenGhostsButton.setBounds(row.removeFromLeft(440));
             area.removeFromTop(6);
             arrangementView.setBounds(area);
         });
    page(Tab::energy, pl("Energia"), pl("Mapa energii: głośność utworu i grup sekunda po sekundzie wobec referencji, od pierwszego dropu."),
         { &energyView }, fill(energyView));
    page(Tab::translation, pl("Translacja"), pl("Jak miks zagra na telefonie, laptopie, słuchawkach, w aucie i w klubie."),
         { &translationView }, fill(translationView));
    page(Tab::stereo, "Stereo", pl("Faza w pasmach sumy, jak Correlometer: na dole przeciwfaza. Z referencjami Mózg poszerza do ich szerokości, nigdy poniżej 0,2. Kropki: zaznaczony kanał."),
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

namespace
{
juce::String stemsNameFor(const BrainProcessor& processor)
{
    return juce::String::fromUTF8(processor.stemTarget().name.c_str());
}
} // namespace

void BrainEditor::select(std::uint64_t owner)
{
    selected = owner;
    timerCallback();
}

// Phase 31: the guide. Each step reads what the Brain sees, so it ticks itself off.
void BrainEditor::updateGuide()
{
    using State = brain::GuideView::State;
    const auto& mix = processor.snapshot();
    const auto roles = ma::ui::roleNames();
    const auto list = [](const juce::StringArray& names) {
        juce::StringArray quotedNames;
        for (int i = 0; i < names.size() && i < 6; ++i)
            quotedNames.add(pl("„") + names[i] + pl("”"));
        return quotedNames.joinIntoString(", ") + (names.size() > 6 ? pl(" i ") + juce::String(names.size() - 6) + pl(" innych") : juce::String());
    };

    std::vector<brain::GuideView::Section> sections(3);
    auto& setup = sections[0];
    setup.title = pl("Przygotowanie (raz na projekt)");
    setup.subtitle = pl("Bez tego Mózg zgaduje. Każdy krok odhacza się sam.");
    juce::StringArray all, noRole, notLearned;
    for (const auto& channel : mix.channels)
    {
        all.add(channel.name);
        if (channel.data.role == 0)
            noRole.add(channel.name);
        if (channel.data.statsValid == 0 && channel.data.role != static_cast<int>(ma::Role::send))
            notLearned.add(channel.name);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("Agent na każdym kanale miksera");
        step.state = all.size() >= 2 ? State::done : State::now;
        step.detail = all.isEmpty() ? pl("Wstaw wtyczkę Agent jako pierwszy efekt na każdym kanale miksera FL (stopa, bas, wokal, synthy, busy). "
                                         "Kanał bez Agenta jest dla Mózgu niewidoczny.")
                                    : pl("Mózg widzi ") + juce::String(all.size()) + pl(" kan.: ") + list(all)
                                          + pl(". Jeśli któregoś brakuje, wstaw na nim Agenta.");
        setup.steps.push_back(step);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("„Naucz poziom” na dropie w każdym Agencie");
        step.state = all.isEmpty() ? State::todo : notLearned.isEmpty() ? State::done : State::now;
        step.detail = notLearned.isEmpty() && !all.isEmpty()
                          ? pl("Wszystkie kanały nauczone: trim, rola i dynamika są znane.")
                          : pl("Zapętl najgęstszy fragment i kliknij „Naucz poziom” (można w kilku Agentach naraz). Ustawia trim, "
                               "rozpoznaje rolę, zapamiętuje dynamikę i szczyty.")
                                + (notLearned.isEmpty() ? juce::String() : pl(" Jeszcze bez nauki: ") + list(notLearned) + ".");
        setup.steps.push_back(step);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("Sprawdź rolę i priorytet kanałów");
        step.state = all.isEmpty() || !notLearned.isEmpty() ? State::todo : noRole.isEmpty() ? State::done : State::now;
        step.detail = noRole.isEmpty() ? pl("Od roli zależy prawie wszystko: kompresja, saturacja, kto komu ustępuje. W Agencie: „Poziom” → „Rola”.")
                                       : pl("Rola „") + roles[0] + pl("” na: ") + list(noRole)
                                             + pl(". Ustaw właściwą w Agencie („Poziom” → „Rola”), a priorytet 1 dla najważniejszych.");
        setup.steps.push_back(step);
    }
    if (mix.kickAndBass)
    {
        brain::GuideView::Step step;
        step.title = pl("Kabel sidechain: stopa do Agenta basu");
        step.state = mix.bassWithoutCable.isEmpty() ? State::done : State::now;
        step.detail = mix.bassWithoutCable.isEmpty()
                          ? pl("Bas dostaje stopę kablem: ustępuje w samo uderzenie, a Mózg mierzy fazę i przesunięcie w czasie.")
                          : pl("Brakuje na: ") + list(mix.bassWithoutCable)
                                + pl(". W mikserze FL zaznacz kanał stopy, kliknij prawym strzałkę na dole kanału basu i wybierz „Sidechain to "
                                     "this track”; potem w oknie Agenta basu: koło zębate → „Processing” → „Connections”, wejście sidechain 1. "
                                     "Bez kabla ustępowanie działa awaryjnie, o jeden bufor za późno.");
        setup.steps.push_back(step);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("Cel brzmienia: referencje");
        const bool tonal = processor.target().valid();
        const bool stems = !processor.stemTarget().profiles.empty();
        step.state = tonal || stems ? State::done : State::optional;
        step.detail = tonal || stems
                          ? (tonal ? pl("Cel tonalny „") + juce::String::fromUTF8(processor.target().name.c_str()) + pl("” wybrany. ") : juce::String())
                                + (stems ? pl("Stemy referencji wczytane.") : pl("Stemy referencji (zakładka „Referencje”) dadzą jeszcze proporcje grup i szerokość."))
                          : pl("Raz na gatunek: „Balans tonalny” → „Nowy cel” → nagraj 3–10 utworów (jak Capture w TBC). Bez tego Mózg "
                               "trzyma się typowych proporcji house'u i wbudowanego celu „Klubowy” (z 13 klubowych masterów).");
        setup.steps.push_back(step);
    }

    {
        brain::GuideView::Step step;
        step.title = pl("Kontrola po masteringu (wtyczka po Ozone)");
        step.state = mix.masterCheckPresent ? State::done : State::optional;
        step.detail = mix.masterCheckPresent
                          ? pl("„Kontrola mastera” działa: raport ma pozycję „Po masteringu”, a balans tonalny i gęstość liczą się po Ozone.")
                          : pl("Wstaw „Mix Assistant Master Check” jako ostatni efekt na kanale Master, po Ozone. Mózg zobaczy wtedy, co "
                               "naprawdę wychodzi po masteringu, i zaproponuje poprawki w miksie (zatwierdzasz je sam przez „Zastosuj”).");
        setup.steps.push_back(step);
    }

    // The loop the user comes back to.
    auto& loop = sections[1];
    loop.title = pl("Miks (powtarzaj po każdej większej zmianie)");
    loop.subtitle = pl("Mózg porównuje tylko głośny fragment, więc zawsze graj drop.");
    const bool changedSinceAnalysis = processor.lastChangeTime() > processor.lastAnalysisTime();
    const auto& proposal = processor.proposal();
    const auto toApply = std::count_if(proposal.items.begin(), proposal.items.end(), [](const auto& i) { return !i.advisory && i.selected != i.inPlace; });
    const bool dropHeard = mix.dropSeconds >= BrainProcessor::kMinimumDropSeconds;
    const bool refining = processor.isRefining();
    const bool autoRefine = processor.autoRefineOn();
    int problems = 0;
    for (const auto& item : mix.report)
        problems += !item.gate && item.status == MixSnapshot::Status::problem ? 1 : 0;
    // 0 drop, 1 analyse, 2 apply, 3 listen again / refine, 4 report
    int current = 4;
    if (refining || measuringEffect)
        current = 3;
    else if (!dropHeard)
        current = 0;
    else if (proposal.made && toApply > 0)
        current = 2;
    else if (processor.lastAnalysisTime() == 0 || changedSinceAnalysis)
        current = 1;
    const auto stateOf = [&](int index) { return index < current ? State::done : index == current ? State::now : State::todo; };
    const auto add = [&](int index, const juce::String& heading, const juce::String& detail) {
        brain::GuideView::Step step;
        step.state = stateOf(index);
        step.title = heading;
        step.detail = detail;
        loop.steps.push_back(step);
    };
    add(0, pl("Zapętl drop i graj"),
        dropHeard ? pl("Drop zmierzony (") + juce::String(juce::roundToInt(mix.dropSeconds)) + pl(" s).")
                  : pl("Najgęstszy fragment, w pętli. Mózg potrzebuje ") + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds))
                        + pl(" s dropu, teraz ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " s.");
    add(1, changedSinceAnalysis ? pl("„Analizuj” jeszcze raz") : pl("„Analizuj” (zakres „Cały miks”)"),
        changedSinceAnalysis ? pl("Po zmianach sprawdza, czy trafiły; zostaje to, czego jeszcze brakuje. Nic nie zmienia sam z siebie.")
                             : pl("Nic nie zmienia. Pokazuje propozycję i werdykt na każdy kanał; w Agentach zakładka „Decyzje Mózgu” mówi, "
                                  "co postanowił przy każdym narzędziu i dlaczego."));
    add(2, pl("Przejrzyj propozycję i „Zastosuj”"),
        pl("Odznacz to, czego nie chcesz. „Przed / po” porównuje z wyrównaną głośnością, „Cofnij” zdejmuje wszystko naraz."));
    add(3, autoRefine ? pl("Graj dalej: Mózg dopracowuje sam") : pl("Puść drop jeszcze raz"),
        autoRefine ? (refining ? processor.refineStatus()
                               : pl("Po „Zastosuj” mierzy drop na nowo i sam poprawia, co zostało (do ")
                                     + juce::String(BrainProcessor::kRefineRounds) + pl(" rund). Graj drop, aż skończy."))
                   : pl("Zmiany słychać dopiero na nowym pomiarze: graj drop ") + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds))
                         + pl(" s, potem znowu „Analizuj”. Albo włącz „Dopracuj sam”."));
    add(4, pl("Sprawdź „Raport”"),
        problems > 0 ? pl("Czerwonych pozycji: ") + juce::String(problems)
                           + pl(". Część to aranż albo brzmienie (do zrobienia w FL), nie miks; każda mówi, co dalej.")
                     : pl("Nic czerwonego. Żółte to rzeczy do decyzji, nie błędy."));
    if (current == 4 && problems == 0)
        loop.steps.back().state = State::done;

    auto& extra = sections[2];
    extra.title = pl("Dodatkowo");
    extra.subtitle = pl("Gdy miks już siedzi.");
    {
        brain::GuideView::Step step;
        step.title = pl("Cały utwór od początku w trybie Song");
        // Long enough to hold an intro, a build and a drop: not just the loop.
        step.state = mix.energy.ours.valid && mix.energy.ours.levelDb.size() >= 90 && mix.energy.comparison.valid ? State::done : State::optional;
        step.detail = pl("Raz, od początku do końca: mapa energii, intro, build i break wobec referencji (zakładki „Energia” i „Referencje”).");
        extra.steps.push_back(step);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("Przed masteringiem: „Raport dla Ozone”");
        step.state = State::info;
        step.detail = pl("W zakładce „Raport”: poziom, szczyty i uwagi do masteringu w jednym pliku.");
        extra.steps.push_back(step);
    }
    {
        brain::GuideView::Step step;
        step.title = pl("Nowy kanał albo inna barwa");
        step.state = State::info;
        step.detail = pl("„Naucz poziom” na tym kanale, potem znowu od „Zapętl drop”.");
        extra.steps.push_back(step);
    }
    guideView.setSections(std::move(sections));
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
    else if (!target.valid())
        tonalStatus = (targetName.isEmpty() ? pl("Bez własnego celu: wbudowany „Klubowy” (z 13 klubowych masterów). Własny: „Nowy cel” i referencje")
                                            : pl("Cel „") + targetName + pl("” nie ma referencji (do tego czasu wbudowany „Klubowy”). Kliknij „Nagraj referencję”"))
                      + (mix.tonal.valid ? juce::String()
                                         : pl(". Słucham miksu: ") + juce::String(juce::roundToInt(processor.secondsAnalysed())) + " / "
                                               + juce::String(juce::roundToInt(BrainProcessor::kMinimumSeconds)) + " s");
    else if (!mix.tonal.valid)
        tonalStatus = pl("Cel „") + targetName + "\" (" + juce::String(static_cast<int>(target.references.size()))
                      + pl(" ref.). Słucham miksu: ") + juce::String(juce::roundToInt(processor.secondsAnalysed())) + " / "
                      + juce::String(juce::roundToInt(BrainProcessor::kMinimumSeconds)) + " s";
    else
        tonalStatus = pl("Cel „") + targetName + "\" (" + juce::String(static_cast<int>(target.references.size()))
                      + pl(" ref.), miks z ") + juce::String(juce::roundToInt(processor.secondsAnalysed())) + " s";
    const bool afterLimiter = processor.tonalAfterLimiter() && processor.limiterTiltKnown();
    if (mix.tonal.valid && afterLimiter)
        tonalStatus += pl(". Gruba: jak po limiterze do ") + juce::String(processor.masterTargetLufs(), 1) + pl(" LUFS, cienka: jak gra");
    else if (mix.tonal.valid && processor.tonalAfterLimiter())
        tonalStatus += pl(". Podgląd limitera jeszcze słucha, krzywa bez niego");
    if (mix.sketch.active() && mix.sketch.tonal.valid)
        tonalStatus += pl(". Przerywana: z partiami-duchami, szare: dziury aranżu (nie EQ)");
    tonalView.setData(processor.activeTarget(), mix.tonal, tonalStatus, mix.sketch.active() ? mix.sketch.tonal : ma::TonalComparison {},
                      mix.sketch.arrangementRegion, afterLimiter ? processor.tonalBeforeLimiter() : ma::TonalComparison {});
    afterLimiterButton.setToggleState(processor.tonalAfterLimiter(), juce::dontSendNotification);
    {
        juce::String status;
        juce::StringArray briefs;
        const auto source = mix.sketch.arrangement.fromReferences ? pl("drop referencji „") + stemsNameFor(processor) + pl("”")
                                                                   : pl("typowy drop house'u (wczytaj stemy referencji, żeby porównać z nimi)");
        if (!processor.sketchMode())
            status = pl("Tryb szkicu wyłączony: miks oceniany tak, jak gra.");
        else if (mix.dropSeconds < BrainProcessor::kMinimumDropSeconds)
            status = pl("Puść drop: sprawdzę, czy ma wszystkie partie. ") + juce::String(juce::roundToInt(mix.dropSeconds)) + " / "
                     + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds)) + " s";
        else if (!mix.sketch.active())
            status = pl("Drop ma wszystkie partie, które ma ") + source + ".";
        else
        {
            status = pl("Szkic. Porównanie: ") + source + pl(". Duchy liczą się w balansie, proporcjach i kolizjach, nie dostają zmian.");
            for (const auto& ghost : mix.sketch.analysis.ghosts)
                briefs.add(pl("• ") + processor.ghostBrief(ghost));
            for (const auto& collision : mix.sketch.collisions)
                if (collision.ghost < mix.sketch.analysis.ghosts.size())
                    briefs.add(pl("Zasłoni: ") + mix.channels[collision.masker].name + pl(" → ")
                               + BrainProcessor::ghostName(mix.sketch.analysis.ghosts[collision.ghost]) + " ("
                               + juce::String(juce::roundToInt(collision.score * 100.0)) + pl("%). „Analizuj” (cały łańcuch) zrobi miejsce."));
        }
        arrangementView.setData(mix.sketch, status, briefs);
        sketchButton.setToggleState(processor.sketchMode(), juce::dontSendNotification);
        listenGhostsButton.setToggleState(processor.listenGhosts(), juce::dontSendNotification);
        listenGhostsButton.setEnabled(processor.sketchMode() && mix.sketch.active());
        listenGhostsButton.setColour(juce::ToggleButton::textColourId,
                                     processor.ghostsAudibleNow() ? colours::warning : colours::text);
    }
    {
        // Phase 20: the energy map.
        const auto item = processor.energyItem();
        std::array<std::vector<std::pair<double, double>>, ma::kStemSections> sections;
        for (int s = 0; s < ma::kStemSections; ++s)
            sections[static_cast<size_t>(s)] = mix.sections[static_cast<size_t>(s)].ranges;
        energyView.setData(mix.energy, sections, item.detail, item.status == MixSnapshot::Status::warning, processor.tempoBpm());
    }

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
    if (shownSection >= 0)
    {
        const auto section = static_cast<ma::StemSection>(shownSection);
        const auto& target = processor.stemSectionTarget(section);
        const auto& ours = mix.section(section);
        const auto text = BrainProcessor::sectionText(section);
        const auto name = text.name.substring(0, 1).toUpperCase() + text.name.substring(1);
        static constexpr const char* missing[] = { "Żadna referencja w zestawie nie ma intro (co najmniej 8 s przed pierwszym dropem).",
                                                   "Żadna referencja w zestawie nie ma buildu (co najmniej 4 s przed dropem).",
                                                   "Żadna referencja w zestawie nie ma breaku (części bez perkusji, co najmniej 8 s)." };
        if (!stems.valid())
        {
            // the drop's status already says what is missing
        }
        else if (!target.valid())
            stemStatus = pl(missing[shownSection]);
        else if (!ours.stems.valid)
            stemStatus = text.waiting + " " + juce::String(juce::roundToInt(ours.seconds)) + " / "
                         + juce::String(juce::roundToInt(section == ma::StemSection::build ? 4.0 : BrainProcessor::kMinimumDropSeconds)) + " s";
        else
        {
            const auto& contrast = target.contrast(section);
            stemStatus = name + pl(" miksu: ") + juce::String(juce::roundToInt(ours.seconds)) + pl(" s, ") + juce::String(ours.belowDropDb, 1)
                         + pl(" dB ciszej niż drop") + (contrast.valid ? pl(" (referencje ") + juce::String(contrast.low, 1) + pl("–")
                                                                              + juce::String(contrast.high, 1) + " dB)"
                                                                        : juce::String())
                         + ".";
            stemWarning = contrast.valid && std::abs(contrast.outside(ours.belowDropDb)) >= 1.0;
        }
        if (ours.stems.valid && ours.automation.valid)
        {
            std::array<juce::String, ma::kStemGroups> steps;
            for (int g = 0; g < ma::kStemGroups; ++g)
                steps[static_cast<size_t>(g)] = BrainProcessor::automationStep(ours.automation, g);
            const auto advice = processor.automationText(section);
            stemView.setData(target, ours.stems, stemStatus, stemWarning, keyStatus, keyWarning, &steps,
                             advice.isNotEmpty() ? advice : pl("Automatyka niepotrzebna: proporcje i głośność względem dropu jak w referencjach."));
        }
        else
            stemView.setData(target, ours.stems, stemStatus, stemWarning, keyStatus, keyWarning);
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
            {
                // What the channel sends on: its side bells, the widener's delayed mid (phase 25) and its width slider (phase 26).
                const double hz = ma::SpectrumAnalyzer::bandCentreHz(ma::BandScale::erb, static_cast<int>(b));
                const double side = ma::processedSidePower(chosen->data.stereo, chosen->data.width, hz, std::pow(10.0, chosen->midDb[b] / 10.0),
                                                           std::pow(10.0, chosen->sideDb[b] / 10.0));
                channel[b] = ma::coherence(chosen->midDb[b], 10.0 * std::log10(side + 1e-30));
            }
        stereoView.setBands(sum.data(), mix.master.valid, chosen != mix.channels.end() ? channel.data() : nullptr);
    }
    {
        const auto& made = processor.proposal();
        const auto verdicts = processor.verdictsNow();
        verdictView.setVerdicts(verdicts, made.wholeMix || !verdicts.empty(), selected);
        fitToViewport(verdictViewport, verdictView, [this] { return verdictView.contentHeight(); });
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
        line.ducked = handling == BrainProcessor::IssueHandling::ducked || handling == BrainProcessor::IssueHandling::unmasked;
        line.cutAtLimit = handling == BrainProcessor::IssueHandling::cutAtLimit;
        line.settled = handling == BrainProcessor::IssueHandling::levelStep || handling == BrainProcessor::IssueHandling::settled;
        line.masker = mix.channels[static_cast<size_t>(issue.masker)].owner;
        line.involvesSelected = selected != 0
                                && (line.masker == selected || mix.channels[static_cast<size_t>(issue.masked)].owner == selected);
        lines.push_back(line);
    }
    issues.setLines(std::move(lines));

    const auto& proposal = processor.proposal();
    proposalView.setProposal(proposal);
    layoutProposal();
    // Phase 23: what already runs in the Agents is no change; unticking it is (it goes away).
    const auto toApply = std::count_if(proposal.items.begin(), proposal.items.end(),
                                       [](const auto& i) { return !i.advisory && i.selected != i.inPlace; });
    const auto running = std::count_if(proposal.items.begin(), proposal.items.end(), [](const auto& i) { return i.inPlace && i.selected; });
    applyButton.setButtonText(toApply > 0 ? pl("Zastosuj (") + juce::String(static_cast<int>(toApply))
                                                + (toApply == 1 ? pl(" zmiana)") : toApply < 5 ? pl(" zmiany)") : pl(" zmian)"))
                                          : pl("Zastosuj"));
    applyButton.setEnabled(toApply > 0);
    const bool undoable = processor.canUndo();
    undoButton.setEnabled(undoable);
    compareButton.setEnabled(undoable || processor.isComparing());
    compareButton.setToggleState(processor.isComparing(), juce::dontSendNotification);
    if (processor.analysisScope + 1 != scopeBox.getSelectedId())
        scopeBox.setSelectedId(processor.analysisScope + 1, juce::dontSendNotification);

    // Phase 22: which step is next. After apply or undo, the drop has to play again before the
    // numbers mean anything; then the next step is a fresh analysis.
    if (measuringEffect && mix.dropSeconds < 1.0)
        effectReset = true;
    const double measured = effectReset ? std::min(mix.dropSeconds, BrainProcessor::kMinimumDropSeconds) : 0.0;
    if (measuringEffect && effectReset && mix.dropSeconds >= BrainProcessor::kMinimumDropSeconds)
    {
        measuringEffect = false;
        proposalStatus.setText(effectNote + pl(" Efekt zmierzony: zajrzyj do raportu albo kliknij „Analizuj” jeszcze raz."),
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::good);
    }
    int step = 0;
    juce::String stepNote;
    if (measuringEffect)
    {
        step = 3;
        stepNote = pl("mierzę efekt ") + juce::String(juce::roundToInt(measured)) + " / "
                   + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds)) + " s";
    }
    else if (proposal.made && toApply > 0)
        step = 1;
    stepPath.setState(step, stepNote);
    const int next = step == 1 ? 1 : step == 3 ? -1 : 0; // 0 = Analizuj, 1 = Zastosuj
    if (next != highlighted)
    {
        highlighted = next;
        analyseButton.setColour(juce::TextButton::buttonColourId, next == 0 ? colours::accent.darker(0.5f) : colours::panel);
        applyButton.setColour(juce::TextButton::buttonColourId, next == 1 ? colours::accent.darker(0.5f) : colours::panel);
    }

    updateGuide();

    // Phase 30: while the Brain refines by itself, it tells where it is.
    const auto refineNote = processor.refineStatus();
    if (refineNote.isNotEmpty())
    {
        measuringEffect = false;
        if (processor.isRefining())
            stepPath.setState(3, pl("dopracowuję sam"));
    }
    if (processor.isComparing())
    {
        const double gain = processor.compareGainDb();
        proposalStatus.setText(std::isfinite(gain) ? pl("Słychać wersję PRZED, głośność wyrównana o ") + juce::String(gain, 1) + " dB."
                                                   : pl("Słychać wersję PRZED. Mierzę głośność, wyrównanie za kilka sekund..."),
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::warning);
    }
    else if (refineNote.isNotEmpty())
    {
        proposalStatus.setText(refineNote, juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, processor.isRefining() ? colours::accent : colours::good);
    }
    else if (measuringEffect)
    {
        proposalStatus.setText(effectNote + pl(" Puść drop, mierzę efekt: ") + juce::String(juce::roundToInt(measured)) + " / "
                                   + juce::String(juce::roundToInt(BrainProcessor::kMinimumDropSeconds)) + " s.",
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::accent);
    }
    else if (proposal.made && toApply > 0 && proposalStatus.getText().isEmpty())
    {
        proposalStatus.setText(pl("Kolizje: ") + juce::String(proposal.crowdingBefore, 2) + pl(" → ")
                                   + juce::String(proposal.crowdingAfter, 2)
                                   + (running > 0 ? pl(". Już działa: ") + juce::String(static_cast<int>(running)) + pl(", nowe: ")
                                                        + juce::String(static_cast<int>(toApply))
                                                  : juce::String())
                                   + pl(". Odznacz to, czego nie chcesz, i kliknij „Zastosuj”."),
                               juce::dontSendNotification);
        proposalStatus.setColour(juce::Label::textColourId, colours::quiet);
    }
    else if (proposal.made && proposal.items.size() > 0 && toApply == 0 && proposalStatus.getText().isEmpty())
    {
        proposalStatus.setText(running > 0 ? pl("Nic nowego: ") + juce::String(static_cast<int>(running))
                                                 + pl(" zmian Mózgu już działa w Agentach. Zostały tylko rady.")
                                           : pl("Nic do zastosowania: zostały tylko rady (do zrobienia w aranżu albo w FL)."),
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
    auto steps = area.removeFromBottom(20);
    autoRefineButton.setBounds(steps.removeFromRight(150));
    stepPath.setBounds(steps);
    scopeBox.setBounds(actions.removeFromLeft(130));
    actions.removeFromLeft(4);
    analyseButton.setBounds(actions.removeFromLeft(100));
    actions.removeFromLeft(12);
    applyButton.setBounds(actions.removeFromLeft(160));
    actions.removeFromLeft(6);
    for (auto* button : { &undoButton, &compareButton })
    {
        button->setBounds(actions.removeFromLeft(90));
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
    afterLimiterButton.setBounds(controls.removeFromRight(230));
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
    breakSectionButton.setBounds(keyRow.removeFromRight(64));
    dropSectionButton.setBounds(keyRow.removeFromRight(64));
    buildSectionButton.setBounds(keyRow.removeFromRight(64));
    introSectionButton.setBounds(keyRow.removeFromRight(64));
    area.removeFromTop(6);
    stemView.setBounds(area);
}

// Phase 28: the editor re-lays these every tick. Shrinking the content first (to measure it at the
// new width) made the viewport clamp its scroll to the top each time, so a long list jumped back up
// while being read; now only a width change re-lays it, and the height goes straight to what it needs.
void BrainEditor::fitToViewport(juce::Viewport& viewport, juce::Component& content, const std::function<int()>& contentHeight)
{
    const int width = viewport.getMaximumVisibleWidth();
    if (content.getWidth() != width)
        content.setSize(width, std::max(1, content.getHeight()));
    const int height = std::max(viewport.getHeight(), contentHeight());
    if (content.getHeight() != height)
        content.setSize(width, height);
}

void BrainEditor::layoutReport()
{
    fitToViewport(reportViewport, report, [this] { return report.contentHeight(); });
}

void BrainEditor::layoutProposal()
{
    fitToViewport(proposalViewport, proposalView, [this] { return proposalView.contentHeight(); });
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

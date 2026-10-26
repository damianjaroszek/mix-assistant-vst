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
    styleLabel(reportLabel, pl("Raport poprawności"), colours::text, 15.0f);
    styleLabel(channelsLabel, pl("Kanały"), colours::text, 15.0f);
    styleLabel(overlayLabel, pl("Widma kanałów tak, jak słyszy je ucho (szare tło: suma)"), colours::text, 15.0f);
    styleLabel(issuesLabel, pl("Kolizje: kto kogo zasłania"), colours::text, 15.0f);
    for (auto* value : { &integratedValue, &shortTermValue, &truePeakValue, &plrValue, &lowCorrelationValue, &fullCorrelationValue })
    {
        styleLabel(*value, {}, colours::text);
        value->setJustificationType(juce::Justification::centredRight);
    }

    for (auto* label : { &title, &agentsLabel, &integratedLabel, &integratedValue, &shortTermLabel, &shortTermValue,
                         &truePeakLabel, &truePeakValue, &plrLabel, &plrValue, &lowCorrelationLabel, &lowCorrelationValue,
                         &fullCorrelationLabel, &fullCorrelationValue, &ceilingLabel, &reportLabel, &channelsLabel,
                         &overlayLabel, &issuesLabel })
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

    profilesButton.setButtonText(pl("Z nauczonych profili"));
    profilesButton.setTooltip(pl("Kolizje liczone z profili nauczonych w Agentach zamiast z tego, co gra teraz."));
    profilesButton.setColour(juce::ToggleButton::textColourId, colours::quiet);
    profilesButton.setToggleState(processor.usesLearnedProfiles(), juce::dontSendNotification);
    profilesButton.onClick = [this] { processor.setUseLearnedProfiles(profilesButton.getToggleState()); };
    addAndMakeVisible(profilesButton);

    styleLabel(proposalLabel, pl("Propozycja"), colours::text, 15.0f);
    styleLabel(proposalStatus, {}, colours::quiet, 13.0f);
    addAndMakeVisible(proposalLabel);
    addAndMakeVisible(proposalStatus);

    const auto styleButton = [this](juce::TextButton& button, const juce::String& text) {
        button.setButtonText(text);
        button.setColour(juce::TextButton::buttonColourId, colours::panel);
        button.setColour(juce::TextButton::buttonOnColourId, colours::warning.darker(0.3f));
        addAndMakeVisible(button);
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
        timerCallback();
    };
    wholeMixButton.onClick = [this] {
        processor.proposeWholeMix();
        proposalStatus.setText({}, juce::dontSendNotification);
        verdictViewButton.setToggleState(true, juce::dontSendNotification);
        showView(View::verdict);
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
    addAndMakeVisible(adaptiveButton);
    adaptiveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.parameters, BrainProcessor::adaptiveId, adaptiveButton);

    proposalViewport.setViewedComponent(&proposalView, false);
    proposalViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(proposalViewport);

    for (auto* button : { &channelsViewButton, &tonalViewButton, &translationViewButton, &stereoViewButton, &verdictViewButton })
    {
        button->setClickingTogglesState(true);
        button->setRadioGroupId(1);
        button->setColour(juce::TextButton::buttonColourId, colours::panel);
        button->setColour(juce::TextButton::buttonOnColourId, colours::accent.darker(0.4f));
        addAndMakeVisible(button);
    }
    channelsViewButton.setButtonText(pl("Kanały"));
    tonalViewButton.setButtonText(pl("Balans tonalny"));
    translationViewButton.setButtonText(pl("Translacja"));
    channelsViewButton.onClick = [this] { showView(View::channels); };
    tonalViewButton.onClick = [this] { showView(View::tonal); };
    translationViewButton.onClick = [this] { showView(View::translation); };
    stereoViewButton.setButtonText("Stereo");
    stereoViewButton.setTooltip(pl("Faza w pasmach sumy, jak Correlometer: na dole przeciwfaza. Kropki: zaznaczony kanał."));
    stereoViewButton.onClick = [this] { showView(View::stereo); };
    addChildComponent(stereoView);
    verdictViewButton.setButtonText(pl("Werdykt"));
    verdictViewButton.onClick = [this] { showView(View::verdict); };
    verdictView.onSelect = [this](std::uint64_t owner) { select(owner == selected ? 0 : owner); };
    verdictViewport.setViewedComponent(&verdictView, false);
    verdictViewport.setScrollBarsShown(true, false);
    addChildComponent(verdictViewport);
    addChildComponent(translationView);

    styleLabel(previewLabel, pl("Odsłuch"), colours::quiet);
    previewLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(previewLabel);
    previewBox.addItemList(BrainProcessor::systemNames(), 1);
    previewBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    previewBox.setSelectedId(processor.previewSystemIndex() + 1, juce::dontSendNotification);
    previewBox.setTooltip(pl("Posłuchaj miksu tak, jak zagra na innym systemie. Pomiary dalej widzą prawdziwy miks."));
    previewBox.onChange = [this] { processor.setPreviewSystem(previewBox.getSelectedId() - 1); };
    addAndMakeVisible(previewBox);

    targetBox.setTextWhenNothingSelected(pl("wybierz cel"));
    targetBox.setColour(juce::ComboBox::backgroundColourId, colours::panel);
    targetBox.onChange = [this] {
        if (targetBox.getSelectedId() > 0)
            processor.selectTarget(targetBox.getText());
    };
    addChildComponent(targetBox);
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
    addChildComponent(tonalView);
    refreshTargets();
    channelsViewButton.setToggleState(true, juce::dontSendNotification);
    showView(View::channels);

    table.onSelect = [this](std::uint64_t owner) { select(owner); };
    issues.onSelect = [this](std::uint64_t owner) { select(owner == selected ? 0 : owner); };
    for (auto* view : std::initializer_list<juce::Component*> { &report, &table, &overlay, &issues })
        addAndMakeVisible(view);

    processor.setEditorOpen(true);
    setSize(1120, 920);
    startTimerHz(10);
}

BrainEditor::~BrainEditor()
{
    stopTimer();
    if (processor.isComparing())
        processor.setComparing(false);
    processor.setEditorOpen(false);
}

void BrainEditor::showView(View view)
{
    currentView = view;
    const bool tonal = view == View::tonal;
    overlay.setVisible(view == View::channels);
    profilesButton.setVisible(view == View::channels);
    translationView.setVisible(view == View::translation);
    verdictViewport.setVisible(view == View::verdict);
    stereoView.setVisible(view == View::stereo);
    overlayLabel.setVisible(false);
    for (auto* c : std::initializer_list<juce::Component*> { &tonalView, &targetBox, &newTargetButton, &captureButton, &removeReferenceButton })
        c->setVisible(tonal);
    resized();
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
    header.removeFromRight(12);
    agentsLabel.setBounds(header.removeFromRight(90));
    header.removeFromRight(12);
    previewBox.setBounds(header.removeFromRight(160));
    header.removeFromRight(6);
    previewLabel.setBounds(header.removeFromRight(70));
    title.setBounds(header);
    area.removeFromTop(10);

    auto left = area.removeFromLeft(310);
    area.removeFromLeft(14);

    const auto row = [&left](juce::Label& label, juce::Label& value) {
        auto r = left.removeFromTop(22);
        label.setBounds(r.removeFromLeft(170));
        value.setBounds(r);
    };
    row(integratedLabel, integratedValue);
    row(shortTermLabel, shortTermValue);
    row(truePeakLabel, truePeakValue);
    row(plrLabel, plrValue);
    row(lowCorrelationLabel, lowCorrelationValue);
    row(fullCorrelationLabel, fullCorrelationValue);
    left.removeFromTop(4);
    auto ceilingRow = left.removeFromTop(26);
    ceilingLabel.setBounds(ceilingRow.removeFromLeft(100));
    ceilingSlider.setBounds(ceilingRow);
    left.removeFromTop(10);
    reportLabel.setBounds(left.removeFromTop(22));
    left.removeFromTop(4);
    report.setBounds(left);

    channelsLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);
    table.setBounds(area.removeFromTop(190));
    area.removeFromTop(10);
    auto overlayHeader = area.removeFromTop(24);
    channelsViewButton.setBounds(overlayHeader.removeFromLeft(90));
    overlayHeader.removeFromLeft(4);
    tonalViewButton.setBounds(overlayHeader.removeFromLeft(120));
    overlayHeader.removeFromLeft(4);
    translationViewButton.setBounds(overlayHeader.removeFromLeft(100));
    overlayHeader.removeFromLeft(4);
    stereoViewButton.setBounds(overlayHeader.removeFromLeft(80));
    overlayHeader.removeFromLeft(4);
    verdictViewButton.setBounds(overlayHeader.removeFromLeft(90));
    profilesButton.setBounds(overlayHeader.removeFromRight(170));
    overlayLabel.setBounds({});
    area.removeFromTop(4);
    auto viewArea = area.removeFromTop(250);
    overlay.setBounds(viewArea);
    translationView.setBounds(viewArea);
    verdictViewport.setBounds(viewArea);
    stereoView.setBounds(viewArea);
    if (tonalView.isVisible())
    {
        auto controls = viewArea.removeFromTop(26);
        targetBox.setBounds(controls.removeFromLeft(170));
        controls.removeFromLeft(6);
        newTargetButton.setBounds(controls.removeFromLeft(90));
        controls.removeFromLeft(6);
        captureButton.setBounds(controls.removeFromLeft(150));
        controls.removeFromLeft(6);
        removeReferenceButton.setBounds(controls.removeFromLeft(110));
        viewArea.removeFromTop(4);
        tonalView.setBounds(viewArea);
    }
    area.removeFromTop(10);
    issuesLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);
    issues.setBounds(area.removeFromTop(110));

    area.removeFromTop(10);
    auto proposalHeader = area.removeFromTop(26);
    proposalLabel.setBounds(proposalHeader.removeFromLeft(140));
    for (auto* button : { &wholeMixButton, &proposeButton, &applyButton, &undoButton, &compareButton })
    {
        button->setBounds(proposalHeader.removeFromLeft(button == &applyButton || button == &wholeMixButton ? 140 : 95));
        proposalHeader.removeFromLeft(6);
    }
    area.removeFromTop(4);
    auto statusRow = area.removeFromTop(22);
    adaptiveButton.setBounds(statusRow.removeFromRight(150));
    proposalStatus.setBounds(statusRow);
    area.removeFromTop(2);
    proposalViewport.setBounds(area);
    layoutProposal();
}

void BrainEditor::layoutProposal()
{
    const int width = proposalViewport.getMaximumVisibleWidth();
    proposalView.setSize(width, 10);
    proposalView.setSize(width, std::max(proposalViewport.getHeight(), proposalView.contentHeight()));
}

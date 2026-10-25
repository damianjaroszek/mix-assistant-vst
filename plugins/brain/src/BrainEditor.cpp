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

    styleLabel(proposalLabel, pl("Propozycja poprawek"), colours::text, 15.0f);
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
    addAndMakeVisible(proposalView);

    table.onSelect = [this](std::uint64_t owner) { select(owner); };
    issues.onSelect = [this](std::uint64_t owner) { select(owner == selected ? 0 : owner); };
    for (auto* view : std::initializer_list<juce::Component*> { &report, &table, &overlay, &issues })
        addAndMakeVisible(view);

    processor.setEditorOpen(true);
    setSize(1000, 920);
    startTimerHz(10);
}

BrainEditor::~BrainEditor()
{
    stopTimer();
    if (processor.isComparing())
        processor.setComparing(false);
    processor.setEditorOpen(false);
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

    std::array<double, ma::SpectrumAnalyzer::kNumBands> bands {}, centres {};
    for (int b = 0; b < ma::SpectrumAnalyzer::kNumBands; ++b)
    {
        bands[static_cast<size_t>(b)] = processor.bandDb(b);
        centres[static_cast<size_t>(b)] = ma::SpectrumAnalyzer::bandCentreHz(b);
    }
    overlay.setData(mix, selected, bands.data(), centres.data(), ma::SpectrumAnalyzer::kNumBands);

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
    agentsLabel.setBounds(header.removeFromRight(120));
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
    auto overlayHeader = area.removeFromTop(22);
    profilesButton.setBounds(overlayHeader.removeFromRight(170));
    overlayLabel.setBounds(overlayHeader);
    area.removeFromTop(4);
    overlay.setBounds(area.removeFromTop(250));
    area.removeFromTop(10);
    issuesLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);
    issues.setBounds(area.removeFromTop(110));

    area.removeFromTop(10);
    auto proposalHeader = area.removeFromTop(26);
    proposalLabel.setBounds(proposalHeader.removeFromLeft(170));
    for (auto* button : { &proposeButton, &applyButton, &undoButton, &compareButton })
    {
        button->setBounds(proposalHeader.removeFromLeft(button == &applyButton ? 150 : 100));
        proposalHeader.removeFromLeft(6);
    }
    area.removeFromTop(4);
    proposalStatus.setBounds(area.removeFromTop(20));
    area.removeFromTop(2);
    proposalView.setBounds(area);
}

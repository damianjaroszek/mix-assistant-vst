#pragma once

#include "BrainProcessor.h"
#include "BrainViews.h"
#include "MaUi.h"

#include <juce_audio_processors/juce_audio_processors.h>

class BrainEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BrainEditor(BrainProcessor&);
    ~BrainEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kWidth = 1120, kHeight = 680;

private:
    void timerCallback() override;
    void select(std::uint64_t owner);

    BrainProcessor& processor;
    std::uint64_t selected = 0;

    juce::Label title, agentsLabel;
    juce::Label integratedLabel, integratedValue, shortTermLabel, shortTermValue;
    juce::Label truePeakLabel, truePeakValue, plrLabel, plrValue;
    juce::Label lowCorrelationLabel, lowCorrelationValue, fullCorrelationLabel, fullCorrelationValue;
    juce::Label ceilingLabel, overlayLabel, issuesLabel;
    juce::Slider ceilingSlider;
    juce::TextButton resetButton;
    juce::ToggleButton profilesButton;
    ma::ui::CoherenceView stereoView;
    brain::VerdictView verdictView;
    juce::Viewport verdictViewport;
    brain::TranslationView translationView;
    juce::Label previewLabel;
    juce::ComboBox previewBox;
    juce::ComboBox targetBox;
    juce::TextButton newTargetButton, captureButton, removeReferenceButton;
    brain::TonalView tonalView;
    // Phase 9: stem references.
    brain::StemView stemView;
    juce::ComboBox stemBox;
    juce::TextButton newStemsButton, loadStemsButton, removeStemsButton;
    std::unique_ptr<juce::FileChooser> stemChooser;
    juce::String shownStems;
    // Phase 10: the track's key (detected or set by hand).
    juce::ComboBox keyBox;
    juce::TextButton redetectKeyButton;
    // Phase 12: which section the view compares.
    juce::TextButton introSectionButton, buildSectionButton, dropSectionButton, breakSectionButton;
    int shownSection = -1; // the drop, else a ma::StemSection
    // Phase 17: sketch mode.
    brain::ArrangementView arrangementView;
    juce::ToggleButton sketchButton;
    void updateKeyControls(juce::String& status, bool& warning);
    void refreshStemTargets();
    void askForNewStemTarget();
    void chooseStems();
    void refreshTargets();
    void askForNewTarget();
    std::unique_ptr<juce::AlertWindow> nameDialog;
    juce::String shownTarget;

    juce::Label proposalStatus;
    juce::TextButton proposeButton, wholeMixButton, applyButton, undoButton, compareButton;
    juce::ToggleButton adaptiveButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> adaptiveAttachment;
    brain::ProposalView proposalView;
    juce::Viewport proposalViewport;
    void layoutProposal();

    brain::ReportView report;
    juce::Viewport reportViewport;
    void layoutReport();
    brain::ChannelTable table;
    brain::OverlayView overlay;
    brain::IssuesView issues;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ceilingAttachment;
    // Phase 11: the master's loudness target and the report for mastering.
    juce::Label targetLabel;
    juce::Slider targetSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> targetAttachment;
    juce::TextButton masteringReportButton;
    std::unique_ptr<juce::FileChooser> reportChooser;
    void saveMasteringReport();

    // Fixed window: the meters and the proposal's buttons stay, everything else is one tab at a time.
    enum class Tab { report, channels, collisions, proposal, verdict, tonal, stems, arrangement, translation, stereo, count };
    std::array<ma::ui::Page, static_cast<size_t>(Tab::count)> pages;
    ma::ui::Tabs tabs;
    void layoutChannels(juce::Rectangle<int> area);
    void layoutTonal(juce::Rectangle<int> area);
    void layoutStems(juce::Rectangle<int> area);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainEditor)
};

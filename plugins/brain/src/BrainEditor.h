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

private:
    void timerCallback() override;
    void select(std::uint64_t owner);

    BrainProcessor& processor;
    std::uint64_t selected = 0;

    juce::Label title, agentsLabel;
    juce::Label integratedLabel, integratedValue, shortTermLabel, shortTermValue;
    juce::Label truePeakLabel, truePeakValue, plrLabel, plrValue;
    juce::Label lowCorrelationLabel, lowCorrelationValue, fullCorrelationLabel, fullCorrelationValue;
    juce::Label ceilingLabel, reportLabel, channelsLabel, overlayLabel, issuesLabel;
    juce::Slider ceilingSlider;
    juce::TextButton resetButton;
    juce::ToggleButton profilesButton;
    juce::TextButton channelsViewButton, tonalViewButton, translationViewButton, stereoViewButton, stemsViewButton, verdictViewButton;
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
    juce::TextButton dropSectionButton, breakSectionButton;
    bool showBreak = false;
    void updateKeyControls(juce::String& status, bool& warning);
    void refreshStemTargets();
    void askForNewStemTarget();
    void chooseStems();
    enum class View { channels, tonal, translation, stereo, stems, verdict };
    void showView(View view);
    View currentView = View::channels;
    void refreshTargets();
    void askForNewTarget();
    std::unique_ptr<juce::AlertWindow> nameDialog;
    juce::String shownTarget;

    juce::Label proposalLabel, proposalStatus;
    juce::TextButton proposeButton, wholeMixButton, applyButton, undoButton, compareButton;
    juce::ToggleButton adaptiveButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> adaptiveAttachment;
    brain::ProposalView proposalView;
    juce::Viewport proposalViewport;
    void layoutProposal();

    brain::ReportView report;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BrainEditor)
};

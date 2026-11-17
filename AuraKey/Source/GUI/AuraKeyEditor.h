// AuraKey — NextDSP · AuraSuite
// AuraKeyEditor.h : interface NextDSP d'AuraKey (1000 × 640 par défaut, redimensionnable).
//
//   ┌ en-tête : logo · fréquence (DXD) · précision ─────────────────────────────────────────────────────┐
//   │ LIVE KEY : clé du flux en grand, confiance,   │ FILE ANALYSIS : zone de dépôt / IMPORT, puis clé,  │
//   │ Camelot, relative, accord, classes de hauteur │ tempo, Camelot, accord, caractéristiques du fichier │
//   │ + cercle des quintes interactif ; MEMORY, RESET│                                                    │
//   ├ SECTIONS : carte majeur / mineur du fichier ───────────────────────────────────────────────────────┤
//
// Glisser un fichier audio n'importe où sur le plug-in lance son analyse (fil de travail du processeur).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../../../Source/GUI/Controls.h"
#include "../../../Source/GUI/NextDSPLookAndFeel.h"
#include "../PluginProcessor.h"
#include "CircleOfFifths.h"
#include "SectionMap.h"

/** Zone de dépôt et résultat de l'analyse du fichier. */
class FilePanelView final : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void setDragHover (bool h) { if (h != dragHover) { dragHover = h; repaint(); } }
    bool isDragHover() const noexcept { return dragHover; }

    AuraKeyProcessor::FileState state = AuraKeyProcessor::FileState::Idle;
    float progress = 0.0f;
    juce::String pendingName, message;
    std::shared_ptr<const FileAnalysis> analysis;

private:
    bool dragHover = false;
};

class AuraKeyEditor final : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer
{
public:
    explicit AuraKeyEditor (AuraKeyProcessor&);
    ~AuraKeyEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

    /** Rafraîchit l'affichage depuis le processeur (appelé par le timer à 30 Hz ; public pour le banc d'essai). */
    void refresh();

    nextdsp::CircleOfFifths& getCircle() noexcept { return circle; }
    nextdsp::SectionMap& getSectionMap() noexcept { return sectionMap; }
    FilePanelView& getFilePanel() noexcept { return filePanel; }
    juce::TextButton& getResetButton() noexcept { return resetButton; }
    juce::TextButton& getImportButton() noexcept { return importButton; }

private:
    void timerCallback() override { refresh(); }
    void chooseFile();
    void paintLive (juce::Graphics&);

    AuraKeyProcessor& keyProcessor;
    nextdsp::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    nextdsp::CircleOfFifths circle;
    nextdsp::SectionMap sectionMap;
    FilePanelView filePanel;
    nextdsp::SegmentedControl memory;
    juce::TextButton resetButton { "RESET" }, importButton { "IMPORT" }, cancelButton { "CANCEL" };
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> headerArea, livePanel, liveText, chromaArea, filePanelArea, sectionPanel;
    aura::key::LiveKeyState live;
    int pinned = -1;
    juce::String rateText, precisionText;
    bool dxd = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraKeyEditor)
};

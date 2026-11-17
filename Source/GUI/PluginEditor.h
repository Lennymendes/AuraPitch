// AuraPitch — NextDSP · AuraSuite
// PluginEditor.h : interface NextDSP.
//
// Lot 2
//   * Mise à l'échelle : l'interface est dessinée à 960 × 640 (kBaseWidth × kBaseHeight) puis agrandie d'un seul
//     facteur (75 … 250 %), proportions verrouillées — coin inférieur droit, ou onglet SETTINGS. Textes, traits et
//     zones cliquables suivent ; l'échelle est sauvegardée avec le projet.
//   * Workflow : sélecteur LIVE (prise, 2 ms) / STUDIO DXD (mixage) avec la latence réelle de chaque mode.
//   * Onglets MONITOR | GRAPH | SETTINGS ; bouton ULTRA dans la barre d'onglets (ambre pendant un export hors ligne,
//     où le mode Ultra est engagé d'office).
//   * Clavier : bandeau d'activité entrée / sortie.
//   * Vue MONITOR : AURA CORE (couronne des 12 notes, halo, note visée ; remplace l'accordeur) + historique de hauteur ;
//     colonne CORRECTION à droite, toujours visible : RETUNE et MIX en grand, NOTE HOLD (Flex en un clic :
//     OFF · SHORT · MID · LONG). Les réglages fins (Flex, Humanize, formants, hauteur, Doubler) sont dans la bande du bas.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../PluginProcessor.h"
#include "Controls.h"
#include "GraphEditor.h"
#include "AuraCore.h"
#include "GraphWindow.h"
#include "NextDSPLookAndFeel.h"
#include "PitchDisplay.h"
#include "ScaleKeyboard.h"
#include "SettingsPanel.h"

class AuraPitchEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit AuraPitchEditor (AuraPitchProcessor&);
    ~AuraPitchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pour le banc d'essai : une image de l'interface (vide la FIFO et avance l'animation d'un pas). */
    void pullFramesForTest (double dtSeconds) { update (dtSeconds); }

    /** Pour le banc d'essai : accès aux commandes. */
    juce::TextButton& getAutoKeyButton() noexcept { return autoKeyButton; }
    juce::TextButton& getMidiButton() noexcept    { return midiButton; }
    nextdsp::PitchGraph& getGraph() noexcept      { return graph; }
    nextdsp::GraphEditor& getGraphEditor() noexcept { return graphEditor; }
    /** Vue du panneau de hauteur : MONITOR (historique) ou GRAPH (éditeur de notes). */
    void setGraphView (bool showGraph);
    bool isGraphView() const noexcept { return graphView; }
    /** Onglets (lot 2). */
    enum class View { Monitor, Graph, Settings };
    void setView (View);
    View getView() const noexcept { return view; }

    /** Mise à l'échelle (lot 2) : taille de dessin et facteur courant. */
    static constexpr int kBaseWidth = 960, kBaseHeight = 640;
    static constexpr double kMinScale = 0.75, kMaxScale = 2.5;
    double getUiScale() const noexcept { return uiScale; }
    void setUiScale (double scale);

    nextdsp::SettingsPanel& getSettingsPanel() noexcept { return settings; }
    nextdsp::WorkflowSwitch& getWorkflowSwitch() noexcept { return trackingMode; }
    nextdsp::ScaleKeyboard& getKeyboard() noexcept { return keyboard; }
    juce::TextButton& getUltraButton() noexcept { return ultraButton; }
    juce::TextButton& getSettingsTab() noexcept { return viewSettings; }
    /** Étape 6 : GRAPH EDITOR dans sa grande fenêtre (true) ou dans l'éditeur (false). */
    void setGraphExpanded (bool expanded);
    bool isGraphExpanded() const noexcept { return graphWindow != nullptr; }
    nextdsp::GraphWindow* getGraphWindow() noexcept { return graphWindow.get(); }
    nextdsp::GraphDetachedPanel& getDetachedPanel() noexcept { return detachedPanel; }
    nextdsp::Knob& getKnob (const juce::String& paramId);
    nextdsp::AuraCore& getCore() noexcept { return core; }
    nextdsp::QuickValues& getNoteHold() noexcept { return noteHold; }

    /** Auto-Key : clic sur le bouton (démarre l'écoute, ou l'arrête et applique le meilleur résultat). */
    void autoKeyClicked();

private:
    void timerCallback() override;
    void update (double dtSeconds);
    void refreshStatus();
    void updateAutoKey (double dt);
    void finishAutoKey (bool apply);

    AuraPitchProcessor& pitchProcessor;
    nextdsp::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    nextdsp::PitchGraph graph;
    nextdsp::GraphEditor graphEditor;
    nextdsp::GraphDetachedPanel detachedPanel;
    nextdsp::SettingsPanel settings;
    juce::TextButton viewMonitor { "MONITOR" }, viewGraph { "GRAPH" }, viewSettings { "SETTINGS" }, ultraButton { "ULTRA" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ultraAttachment;
    bool graphView = false;
    View view = View::Monitor;
    double uiScale = 1.0;
    bool offlineShown = false, ultraShown = false, constructed = false;
    nextdsp::AuraCore core;
    nextdsp::ScaleKeyboard keyboard;

    juce::ComboBox presetBox, keyBox, scaleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> keyAttachment, scaleAttachment;

    nextdsp::SegmentedControl inputType, channelMode;
    nextdsp::WorkflowSwitch trackingMode;
    nextdsp::QuickValues noteHold;
    nextdsp::Knob retune, humanize, formant, transpose, fine, a4, mix, flex, throat, doubler;
    juce::TextButton autoKeyButton { "AUTO-KEY" }, midiButton { "MIDI" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> midiAttachment;
    enum class KeyState { Idle, Listening, Showing };
    KeyState keyState = KeyState::Idle;
    double keyTimer = 0.0;
    juce::ToggleButton preserve { "PRESERVE" };
    juce::TextButton bypass { "BYPASS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> preserveAttachment, bypassAttachment;

    juce::Rectangle<int> headerArea, tabArea, scalePanel, controlPanel, statusArea, macroPanel, stageArea, pullArea;
    float pullShown = 0.0f, pullActivity = 0.0f;
    juce::String rateText, precisionText, latencyText;
    bool dxd = false, studio = false;
    double lastTick = 0.0;

    // Déclarée en dernier : détruite avant le GraphEditor et le LookAndFeel qu'elle utilise.
    std::unique_ptr<nextdsp::GraphWindow> graphWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraPitchEditor)
};

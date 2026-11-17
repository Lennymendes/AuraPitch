// AuraPitch — NextDSP · AuraSuite
// SettingsPanel.h : onglet SETTINGS (lot 2).
//
//   ENGINE     interrupteur ULTRA MODE (paramètre ultra_mode) et état réel du moteur : STANDARD, ULTRA, ou
//              ULTRA · OFFLINE BOUNCE quand l'hôte exporte hors temps réel (engagé d'office, sans toucher au réglage) ;
//   WORKFLOW   rappel des deux modes et de leur latence à la fréquence de la session ;
//   INTERFACE  taille de la fenêtre : 75 … 200 % (l'interface entière est mise à l'échelle, coin inférieur droit ou
//              boutons), sauvegardée avec le projet ;
//   SESSION    fréquence, précision, latence annoncée à l'hôte.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../PluginProcessor.h"
#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (AuraPitchProcessor&);

    /** Rafraîchit l'état affiché (appelé par le timer de l'éditeur). */
    void refresh();
    /** Échelle affichée par les boutons (1 = 100 %). */
    void setCurrentScale (double scale);
    std::function<void (double)> onScaleChosen;

    juce::ToggleButton& getUltraToggle() noexcept { return ultraToggle; }
    juce::TextButton& getScaleButton (int index) noexcept { return scaleButtons[(size_t) index]; }
    static constexpr int kNumScales = 5;
    static constexpr double kScales[kNumScales] = { 0.75, 1.0, 1.25, 1.5, 2.0 };
    /** Texte d'état du moteur (« STANDARD », « ULTRA », « ULTRA · OFFLINE BOUNCE »). */
    juce::String engineStateText() const { return engineText; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AuraPitchProcessor& processor;
    juce::ToggleButton ultraToggle { "ULTRA MODE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ultraAttachment;
    std::array<juce::TextButton, kNumScales> scaleButtons;

    juce::Rectangle<int> engineArea, workflowArea, interfaceArea, sessionArea;
    juce::String engineText, sessionText, liveText, studioText;
    bool ultra = false, offline = false, studio = false;
    double shownScale = 1.0;
};
}

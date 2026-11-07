// AuraPitch — NextDSP · AuraSuite
// ScaleKeyboard.h : clavier d'une octave (C … B), synchronisé avec Key / Scale et les paramètres note_0 … note_11.
//   * clic           : note active <-> retirée ;
//   * Alt-clic / clic droit : bypass (la voix passe sans correction près de cette note) ;
//   * sur une gamme prédéfinie, le premier clic recopie la gamme affichée dans le clavier puis passe en Custom.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
class ScaleKeyboard final : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    explicit ScaleKeyboard (juce::AudioProcessorValueTreeState& state);

    /** État affiché : 0 Active, 1 Bypass, 2 Removed, pour chaque classe de hauteur (C = 0). */
    std::array<int, 12> displayedStates() const;
    std::uint16_t activeMask() const;
    int tonic() const;

    /** Appelé par le timer de l'éditeur : redessine si la gamme a changé (automation, preset, hôte). */
    void refresh();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> keyBounds (int pc) const;
    int keyAt (juce::Point<float>) const;
    void setNote (int pc, int stateIndex);

    juce::AudioProcessorValueTreeState& state;
    std::array<int, 12> lastShown {};
    int lastTonic = -1, hovered = -1;
};
}

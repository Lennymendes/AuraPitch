// AuraPitch — NextDSP · AuraSuite
// ScaleKeyboard.h : clavier d'une octave (C … B), synchronisé avec Key / Scale et les paramètres note_0 … note_11.
//   * clic           : note active <-> retirée ;
//   * Alt-clic / clic droit : bypass (la voix passe sans correction près de cette note) ;
//   * sur une gamme prédéfinie, le premier clic recopie la gamme affichée dans le clavier puis passe en Custom.
// Lot 2 : bandeau d'activité au-dessus des touches — rangée du haut (ambre) les notes chantées, rangée du bas (cyan)
// les notes sorties après correction, sur les 3 dernières secondes. On voit d'un coup d'œil quelle note fausse est
// ramenée où, et quelles notes de la gamme servent réellement.
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

    /** Classes de hauteur des notes MIDI tenues (mode MIDI Target) : touches éclairées en ambre. */
    void setMidiPitchClasses (std::uint16_t mask) { if (mask != midiMask) { midiMask = mask; repaint(); } }

    /** Bandeau d'activité : une trame voisée (hauteurs MIDI chantée et corrigée), puis tick() à chaque image. */
    void pushActivity (double inputMidi, double outputMidi) noexcept;
    void tick (double dtSeconds);
    float inputActivity (int pc) const noexcept  { return heatIn[(size_t) (pc % 12)]; }
    float outputActivity (int pc) const noexcept { return heatOut[(size_t) (pc % 12)]; }
    static constexpr float kHeatStrip = 11.0f;          // hauteur du bandeau

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
    std::uint16_t midiMask = 0;
    std::array<float, 12> heatIn {}, heatOut {};
    bool heatDirty = false;
};
}

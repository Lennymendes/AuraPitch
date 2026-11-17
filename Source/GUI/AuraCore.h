// AuraPitch — NextDSP · AuraSuite
// AuraCore.h : pièce maîtresse de la vue MONITOR (lot 2). Remplace l'accordeur en arc.
//
//   * Couronne des 12 classes de hauteur (C en haut, sens horaire, un demi-ton par case) : notes de la gamme en cyan
//     sourd, notes retirées éteintes, note visée lumineuse. Deux repères tournent sur la couronne à la hauteur réelle,
//     continue : la voix entrante (ambre) et la voix corrigée (cyan). On VOIT la correction : l'ambre flotte autour de
//     la case, le cyan s'y verrouille.
//   * Halo : un anneau de particules qui respire avec le niveau de la voix et s'agite avec l'écart à la note ;
//     au repos, il s'éteint lentement.
//   * Centre : note visée en grand, écart d'entrée (ambre, en cents), état de la sortie (LOCKED à ±3 c, sinon écart).
//   * Pied : fréquence d'entrée et écart de sortie (comme l'ancien accordeur).
//
// Données : PitchFrame de la LockFreePitchFifo, poussées par l'éditeur (timer 60 Hz). Rien ici ne touche au thread
// audio ; aucune allocation par image.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>

#include "../DSP/LockFreePitchFifo.h"
#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
class AuraCore final : public juce::Component
{
public:
    AuraCore();

    void push (const aura::pitch::PitchFrame& f, double referenceA4);
    void setScale (std::uint16_t activeMask, int tonic);
    void tick (double dtSeconds);
    void paint (juce::Graphics&) override;

    //--- banc d'essai ----------------------------------------------------------------------------------------------
    int   getTargetPitchClass() const noexcept { return target >= 0 ? target % 12 : -1; }
    float getShownInputCents() const noexcept  { return shownIn; }
    float getShownOutputCents() const noexcept { return shownOut; }
    float getActivity() const noexcept         { return (float) activity; }
    bool  isLocked() const noexcept            { return activity > 0.3 && std::abs (shownOut) < 3.0f; }

private:
    /** Angle (radians, 0 en haut, sens horaire) d'une hauteur MIDI continue. */
    static float angleFor (float midi) noexcept;

    std::uint16_t mask = 0x0fff;
    int key = 0;
    int target = -1;
    float inputMidi = 0.0f, outputMidi = 0.0f;           // hauteurs affichées (lissées sur le cercle)
    float wantedInMidi = 0.0f, wantedOutMidi = 0.0f;
    float shownIn = 0.0f, shownOut = 0.0f, wantedIn = 0.0f, wantedOut = 0.0f;   // cents
    double inputHz = 0.0, silence = 1.0, activity = 0.0, level = 0.0, wantedLevel = 0.0, phase = 0.0;
    std::array<float, 12> glow {};                         // éclat par case (la cible s'allume, puis s'éteint)
};
}

// AuraKey — NextDSP · AuraSuite
// CircleOfFifths.h : cercle des quintes interactif.
//
//   * Anneau extérieur : les 12 tonalités majeures (C en haut, une quinte par case dans le sens horaire) ; anneau
//     intérieur : leurs relatives mineures. L'intensité de chaque case suit la probabilité de la clé dans le flux live.
//   * Clé live : case lumineuse (cyan en majeur, violet en mineur) ; clé du fichier importé : contour ambre.
//   * Survol : la case s'éclaire et le centre affiche la clé (nom, code Camelot, notes de la gamme, probabilité live) ;
//     clic : épingle la clé (nouveau clic ou clic au centre : désépingle). onPinChanged reçoit l'index (−1 : aucune).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace nextdsp
{
class CircleOfFifths final : public juce::Component
{
public:
    CircleOfFifths();

    void setLive (const std::array<double, 24>& posteriors, int tonic, bool minor, bool hasKey);
    void setFileKey (int tonic, bool minor);                  // tonic < 0 : aucun fichier
    void setPinned (int keyIndex);                            // −1 : aucune
    int getPinned() const noexcept { return pinned; }
    int getHovered() const noexcept { return hovered; }
    std::function<void (int)> onPinChanged;

    /** Index de clé sous un point (−1 : hors des anneaux). Public pour le banc d'essai. */
    int keyAt (juce::Point<float>) const;
    /** Centre de la case d'une clé (banc d'essai). */
    juce::Point<float> centreOfKey (int keyIndex) const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Geometry { juce::Point<float> c; float rOut, rMid, rIn; };
    Geometry geometry() const;

    std::array<double, 24> post {};
    int liveKey = -1, fileKey = -1, pinned = -1, hovered = -1;
};
}

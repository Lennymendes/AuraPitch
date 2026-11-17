// AuraKey — NextDSP · AuraSuite
// SectionMap.h : cartographie des sections majeures / mineures du fichier importé.
//
// Une bande par section sur la durée du morceau (cyan : majeur, violet : mineur ; opacité = confiance), nom de la clé et
// code Camelot dans la bande, règle en minutes:secondes. Survol : détails dans l'en-tête ; clic : la clé de la section
// est épinglée sur le cercle des quintes (onSectionClicked reçoit l'index de clé).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DSP/KeyTypes.h"

#include <functional>

namespace nextdsp
{
class SectionMap final : public juce::Component
{
public:
    void setSections (const std::vector<aura::key::Section>& sections, double durationSec);
    void setHighlightKey (int keyIndex) { if (keyIndex != highlight) { highlight = keyIndex; repaint(); } }
    std::function<void (int keyIndex)> onSectionClicked;
    /** Texte du survol (« 0:36 – 1:12 · F Minor · 64 % »), vide sans survol. */
    juce::String getHoverText() const;
    int sectionAt (juce::Point<float>) const;
    juce::Rectangle<float> barArea() const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    float xForTime (double t) const;
    std::vector<aura::key::Section> sections;
    double duration = 0.0;
    int hovered = -1, highlight = -1;
};

juce::String formatTime (double seconds);
}

// AuraPitch — NextDSP · AuraSuite
// Controls.h : bouton rotatif NextDSP et sélecteur segmenté, reliés aux paramètres de l'APVTS.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
/** Curseur rotatif NextDSP :
      * glisser vers le haut / la droite : 220 px pour toute la course ;
      * Maj, Cmd (macOS) ou Ctrl (Windows / Linux) pendant le glissé : sensibilité ÷ 5 (réglage au dixième) — on peut
        appuyer ou relâcher la touche en cours de geste, sans saut de valeur ;
      * double-clic (ou Alt-clic) : valeur par défaut ;
      * clic sur la valeur affichée, ou clic droit sur le bouton : saisie au clavier (« 3.5 », « -2.5 »…). */
class KnobSlider final : public juce::Slider
{
public:
    static constexpr double kDragPixels = 220.0;
    static constexpr double kFineFactor = 5.0;

    static bool isFineDrag (const juce::ModifierKeys& m) noexcept
    {
        return m.isShiftDown() || m.isCommandDown() || m.isCtrlDown();
    }
    /** Variation de position normalisée (0 … 1) pour un déplacement de souris (px). */
    static double dragDelta (juce::Point<float> delta, const juce::ModifierKeys& m) noexcept
    {
        return (double) (delta.x - delta.y) / (isFineDrag (m) ? kDragPixels * kFineFactor : kDragPixels);
    }

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    juce::Point<float> lastDragPos;
    double dragProportion = 0.0;
};

/** Bouton rotatif : titre au-dessus, anneau de valeur lumineux, valeur avec unité en dessous. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, juce::String title,
          juce::Colour accent = colours::cyan, bool bipolar = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    KnobSlider slider;

private:
    juce::String title;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** Sélecteur segmenté pour un paramètre Choice ou Bool (index = valeur). */
class SegmentedControl final : public juce::Component,
                               public juce::SettableTooltipClient
{
public:
    SegmentedControl (juce::RangedAudioParameter& param, juce::StringArray labels, juce::String title = {},
                      juce::Colour accent = colours::cyan);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    int getSelectedIndex() const noexcept { return selected; }

private:
    juce::Rectangle<float> segmentArea() const;
    int segmentAt (juce::Point<float>) const;

    juce::StringArray labels;
    juce::String title;
    juce::Colour accent;
    juce::ParameterAttachment attachment;
    int selected = 0, hovered = -1;
};

/** Sélecteur de workflow (lot 2) : deux grandes cellules pour le paramètre tracking_mode.
      LIVE        — enregistrement : monitoring à 2 ms, hauteur suivie à l'instant lu ;
      STUDIO DXD  — mixage : commande lue dans le futur, formants replacés, latence compensée par l'hôte.
    Chaque cellule affiche sa latence réelle à la fréquence de la session. */
class WorkflowSwitch final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    explicit WorkflowSwitch (juce::RangedAudioParameter& trackingMode);

    void setLatencies (double liveMs, double studioMs);
    int getSelectedIndex() const noexcept { return selected; }
    /** Cellule (0 = Live, 1 = Studio) : zone cliquable, pour le banc d'essai. */
    juce::Rectangle<float> cellBounds (int index) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::ParameterAttachment attachment;
    int selected = 0, hovered = -1;
    double liveMs = 2.0, studioMs = 56.0;
};

/** Valeurs rapides (lot 2) : raccourcis vers des valeurs fixes d'un paramètre continu (ex. NOTE HOLD -> Flex
    0 / 40 / 90 / 160 ms). Le segment s'allume quand le paramètre vaut exactement l'une des valeurs ; réglé autrement
    (bouton rotatif, automation), aucun segment n'est allumé. */
class QuickValues final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    QuickValues (juce::RangedAudioParameter& param, juce::StringArray labels, std::vector<float> values,
                 juce::String title = {}, juce::Colour accent = colours::cyan);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    int getSelectedIndex() const noexcept { return selected; }
    /** Clic sur le segment i (banc d'essai). */
    void choose (int i);

private:
    juce::Rectangle<float> segmentArea() const;
    int segmentAt (juce::Point<float>) const;

    juce::StringArray labels;
    std::vector<float> values;
    juce::String title;
    juce::Colour accent;
    juce::ParameterAttachment attachment;
    int selected = -1, hovered = -1;
};
}

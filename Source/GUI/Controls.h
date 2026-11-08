// AuraPitch — NextDSP · AuraSuite
// Controls.h : bouton rotatif NextDSP et sélecteur segmenté, reliés aux paramètres de l'APVTS.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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
}

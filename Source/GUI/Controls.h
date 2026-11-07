// AuraPitch — NextDSP · AuraSuite
// Controls.h : bouton rotatif NextDSP et sélecteur segmenté, reliés aux paramètres de l'APVTS.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
/** Bouton rotatif : titre au-dessus, anneau de valeur lumineux, valeur avec unité en dessous (éditable au double-clic
    sur la valeur ; double-clic sur le bouton = valeur par défaut). */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, juce::String title,
          juce::Colour accent = colours::cyan, bool bipolar = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    juce::Slider slider;

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

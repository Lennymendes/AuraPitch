// AuraPitch — NextDSP · AuraSuite
// NextDSPLookAndFeel.h : design system NextDSP (sombre, matériel, chirurgical).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace nextdsp
{
namespace colours
{
    inline const juce::Colour background { 0xff0b0c10 };
    inline const juce::Colour panel      { 0xff12151e };
    inline const juce::Colour panelHi    { 0xff171b26 };
    inline const juce::Colour border     { 0xff1e2433 };
    inline const juce::Colour grid       { 0xff1a1f2c };
    inline const juce::Colour text       { 0xffe6e9f0 };
    inline const juce::Colour textDim    { 0xff7c8496 };
    inline const juce::Colour textFaint  { 0xff4a5163 };
    inline const juce::Colour cyan       { 0xff00f0ff };   // hauteur corrigée / cible
    inline const juce::Colour violet     { 0xffa855f7 };   // hauteur entrante (ancienne)
    inline const juce::Colour amber      { 0xfff59e0b };   // hauteur entrante (récente)
    inline const juce::Colour red        { 0xffff3b5c };
}

juce::Font font (float height, bool bold = false);
juce::Font monoFont (float height);

/** Halo lumineux : plusieurs traits superposés de plus en plus larges et transparents. */
void strokeGlow (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float width, float glow = 1.0f);

/** Panneau NextDSP : fond #12151E, bordure #1E2433, coins de 8 px. */
void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, float radius = 8.0f);

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getLabelFont (juce::Label&) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    juce::Font getPopupMenuFont() override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};
}

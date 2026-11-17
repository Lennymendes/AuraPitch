// AuraPitch — NextDSP · AuraSuite
#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
using namespace juce;

Font font (float height, bool bold)
{
    auto o = FontOptions (height).withKerningFactor (0.02f);
    return Font (bold ? o.withStyle ("Bold") : o);
}

Font monoFont (float height)
{
    return Font (FontOptions (Font::getDefaultMonospacedFontName(), height, Font::plain));
}

void strokeGlow (Graphics& g, const Path& path, Colour colour, float width, float glow)
{
    if (glow > 0.0f)
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (colour.withAlpha (0.06f * glow * (float) (4 - i)));
            g.strokePath (path, PathStrokeType (width + 3.0f * (float) i, PathStrokeType::curved, PathStrokeType::rounded));
        }
    g.setColour (colour);
    g.strokePath (path, PathStrokeType (width, PathStrokeType::curved, PathStrokeType::rounded));
}

void drawPanel (Graphics& g, Rectangle<float> r, float radius)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (colours::border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

//==============================================================================================================
LookAndFeel::LookAndFeel()
{
    setColour (ResizableWindow::backgroundColourId, colours::background);
    setColour (Label::textColourId, colours::text);
    setColour (Slider::textBoxTextColourId, colours::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Slider::textBoxHighlightColourId, colours::cyan.withAlpha (0.3f));
    setColour (TextEditor::textColourId, colours::text);
    setColour (TextEditor::backgroundColourId, colours::panelHi);
    setColour (TextEditor::highlightColourId, colours::cyan.withAlpha (0.3f));
    setColour (TextEditor::outlineColourId, colours::border);
    setColour (TextEditor::focusedOutlineColourId, colours::cyan.withAlpha (0.6f));
    setColour (CaretComponent::caretColourId, colours::cyan);
    setColour (ComboBox::textColourId, colours::text);
    setColour (ComboBox::arrowColourId, colours::textDim);
    setColour (ComboBox::backgroundColourId, colours::panelHi);
    setColour (ComboBox::outlineColourId, colours::border);
    setColour (PopupMenu::backgroundColourId, colours::panel);
    setColour (PopupMenu::textColourId, colours::text);
    setColour (PopupMenu::highlightedBackgroundColourId, colours::cyan.withAlpha (0.15f));
    setColour (PopupMenu::highlightedTextColourId, colours::cyan);
    setColour (TextButton::textColourOffId, colours::textDim);
    setColour (TextButton::textColourOnId, colours::cyan);
    setColour (TooltipWindow::backgroundColourId, colours::panelHi);
    setColour (TooltipWindow::textColourId, colours::text);
    setColour (TooltipWindow::outlineColourId, colours::border);
}

void LookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const auto bounds = Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float size   = jmin (bounds.getWidth(), bounds.getHeight());
    const auto  centre = bounds.getCentre();
    const float radius = size * 0.5f;
    const float ringW  = jmax (2.5f, radius * 0.11f);
    const float arcR   = radius - ringW;
    const bool  bipolar = s.getProperties().getWithDefault ("bipolar", false);
    const auto  accent  = Colour ((uint32) (int64) s.getProperties().getWithDefault ("accent", (int64) colours::cyan.getARGB()));
    const float angle  = start + pos * (end - start);

    // Rail
    Path rail;
    rail.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour (colours::border);
    g.strokePath (rail, PathStrokeType (ringW, PathStrokeType::curved, PathStrokeType::rounded));

    // Valeur
    const float from = bipolar ? (start + end) * 0.5f : start;
    if (std::abs (angle - from) > 0.001f)
    {
        Path value;
        value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, jmin (from, angle), jmax (from, angle), true);
        strokeGlow (g, value, accent.withAlpha (s.isEnabled() ? 1.0f : 0.35f), ringW, 1.0f);
    }

    // Corps du bouton
    const float bodyR = arcR - ringW * 1.6f;
    const auto body = Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
    g.setGradientFill (ColourGradient (Colour (0xff1d2230), body.getX(), body.getY(), Colour (0xff0f1219), body.getX(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (colours::border.brighter (0.15f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    // Index
    const Point<float> p1 = centre.getPointOnCircumference (bodyR * 0.35f, angle);
    const Point<float> p2 = centre.getPointOnCircumference (bodyR * 0.85f, angle);
    Path tick;
    tick.startNewSubPath (p1);
    tick.lineTo (p2);
    strokeGlow (g, tick, colours::text, jmax (1.5f, ringW * 0.45f), 0.4f);
}

Label* LookAndFeel::createSliderTextBox (Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (monoFont (12.0f));
    l->setJustificationType (Justification::centred);
    l->setColour (Label::textColourId, colours::text);
    l->setColour (Label::outlineColourId, Colours::transparentBlack);
    l->setColour (Label::backgroundColourId, Colours::transparentBlack);
    return l;
}

Font LookAndFeel::getLabelFont (Label& l)
{
    return font (jlimit (10.0f, 15.0f, (float) l.getHeight() * 0.7f));
}

void LookAndFeel::drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box)
{
    const auto r = Rectangle<int> (0, 0, w, h).toFloat().reduced (0.5f);
    g.setColour (box.isMouseOver (true) ? colours::panelHi.brighter (0.06f) : colours::panelHi);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (box.hasKeyboardFocus (true) ? colours::cyan.withAlpha (0.5f) : colours::border);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    Path arrow;
    const float ax = (float) w - 16.0f, ay = (float) h * 0.5f;
    arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
    arrow.lineTo (ax, ay + 2.0f);
    arrow.lineTo (ax + 4.0f, ay - 2.0f);
    g.setColour (colours::textDim);
    g.strokePath (arrow, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
}

Font LookAndFeel::getComboBoxFont (ComboBox& box) { return font (jlimit (11.0f, 15.0f, (float) box.getHeight() * 0.48f), true); }

void LookAndFeel::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (10, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void LookAndFeel::drawPopupMenuBackground (Graphics& g, int w, int h)
{
    g.fillAll (colours::panel);
    g.setColour (colours::border);
    g.drawRect (0, 0, w, h);
}

Font LookAndFeel::getPopupMenuFont() { return font (14.0f); }

void LookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState() || (bool) b.getProperties().getWithDefault ("forceLit", false);
    const auto accent = Colour ((uint32) (int64) b.getProperties().getWithDefault ("accent", (int64) colours::cyan.getARGB()));
    g.setColour (on ? accent.withAlpha (0.14f) : (highlighted ? colours::panelHi.brighter (0.05f) : colours::panelHi));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (on ? accent.withAlpha (0.8f) : colours::border.brighter (down ? 0.3f : 0.0f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

void LookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool, bool)
{
    const auto accent = Colour ((uint32) (int64) b.getProperties().getWithDefault ("accent", (int64) colours::cyan.getARGB()));
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.getToggleState() || (bool) b.getProperties().getWithDefault ("forceLit", false) ? accent : colours::textDim);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), Justification::centred, 1);
}

Font LookAndFeel::getTextButtonFont (TextButton&, int h) { return font (jlimit (10.0f, 14.0f, (float) h * 0.42f), true); }

void LookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool highlighted, bool)
{
    // Interrupteur « pilule » + libellé
    const auto r = b.getLocalBounds().toFloat();
    const float h = jmin (20.0f, r.getHeight() - 4.0f);
    const auto pill = Rectangle<float> (h * 1.9f, h).withCentre ({ r.getCentreX(), r.getY() + h * 0.5f + 2.0f });
    const bool on = b.getToggleState();
    g.setColour (on ? colours::cyan.withAlpha (0.18f) : colours::panelHi.brighter (highlighted ? 0.05f : 0.0f));
    g.fillRoundedRectangle (pill, h * 0.5f);
    g.setColour (on ? colours::cyan.withAlpha (0.9f) : colours::border.brighter (0.2f));
    g.drawRoundedRectangle (pill.reduced (0.5f), h * 0.5f, 1.0f);
    const float d = h - 6.0f;
    const auto knob = Rectangle<float> (d, d).withCentre ({ on ? pill.getRight() - h * 0.5f : pill.getX() + h * 0.5f, pill.getCentreY() });
    if (on)
    {
        g.setColour (colours::cyan.withAlpha (0.25f));
        g.fillEllipse (knob.expanded (3.0f));
    }
    g.setColour (on ? colours::cyan : colours::textDim);
    g.fillEllipse (knob);

    g.setFont (font (12.0f, true));
    g.setColour (on ? colours::text : colours::textDim);
    g.drawFittedText (b.getButtonText(), r.withTop (pill.getBottom() + 4.0f).toNearestInt(), Justification::centredTop, 1);
}
}

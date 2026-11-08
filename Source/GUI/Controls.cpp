// AuraPitch — NextDSP · AuraSuite
#include "Controls.h"

namespace nextdsp
{
using namespace juce;

void KnobSlider::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (isEnabled()) showTextBox();                 // clic droit : saisie au clavier
        return;
    }
    Slider::mouseDown (e);                              // geste (begin/endChangeGesture), Alt-clic = défaut
    lastDragPos = e.position;
    dragProportion = valueToProportionOfLength (getValue());
}

void KnobSlider::mouseDrag (const MouseEvent& e)
{
    if (! isEnabled() || e.mods.isPopupMenu())
        return;
    // Déplacement relatif depuis le dernier évènement : changer de modificateur en cours de geste ne fait pas sauter
    // la valeur, seule la sensibilité change.
    dragProportion = jlimit (0.0, 1.0, dragProportion + dragDelta (e.position - lastDragPos, e.mods));
    lastDragPos = e.position;
    setValue (proportionOfLengthToValue (dragProportion), sendNotificationSync);
}

//==============================================================================================================
Knob::Knob (AudioProcessorValueTreeState& state, const String& paramId, String t, Colour accent, bool bipolar)
    : title (std::move (t))
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, 90, 18);
    slider.setVelocityBasedMode (false);
    slider.setTextBoxIsEditable (true);              // un clic sur la valeur ouvre la saisie
    slider.getProperties().set ("accent", (int64) accent.getARGB());
    slider.getProperties().set ("bipolar", bipolar);
    addAndMakeVisible (slider);

    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
    if (auto* p = state.getParameter (paramId))
    {
        slider.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
        slider.setTooltip (p->getName (64) + String (CharPointer_UTF8 (
            "  \xc2\xb7  drag (Shift / Cmd-Ctrl: fine) \xc2\xb7 double-click: default \xc2\xb7 click value or right-click: type a value")));
    }
}

void Knob::paint (Graphics& g)
{
    g.setFont (font (11.5f, true));
    g.setColour (colours::textDim);
    g.drawText (title.toUpperCase(), getLocalBounds().removeFromTop (16), Justification::centred);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (16);
    slider.setTextBoxStyle (Slider::TextBoxBelow, false, r.getWidth(), 18);
    slider.setBounds (r);
}

//==============================================================================================================
SegmentedControl::SegmentedControl (RangedAudioParameter& param, StringArray l, String t, Colour a)
    : labels (std::move (l)), title (std::move (t)), accent (a),
      attachment (param, [this] (float v) { selected = roundToInt (v); repaint(); }, nullptr)
{
    attachment.sendInitialUpdate();
    setRepaintsOnMouseActivity (true);
    setTooltip (param.getName (64));
}

Rectangle<float> SegmentedControl::segmentArea() const
{
    auto r = getLocalBounds().toFloat();
    if (title.isNotEmpty()) r.removeFromTop (16.0f);
    return r.reduced (0.5f);
}

int SegmentedControl::segmentAt (Point<float> p) const
{
    const auto area = segmentArea();
    if (! area.contains (p) || labels.isEmpty()) return -1;
    return jlimit (0, labels.size() - 1, (int) ((p.x - area.getX()) / area.getWidth() * (float) labels.size()));
}

void SegmentedControl::paint (Graphics& g)
{
    if (title.isNotEmpty())
    {
        g.setFont (font (11.5f, true));
        g.setColour (colours::textDim);
        g.drawText (title.toUpperCase(), getLocalBounds().removeFromTop (16), Justification::centredLeft);
    }

    const auto area = segmentArea();
    g.setColour (colours::panelHi);
    g.fillRoundedRectangle (area, 6.0f);
    g.setColour (colours::border);
    g.drawRoundedRectangle (area, 6.0f, 1.0f);

    const float w = area.getWidth() / (float) labels.size();
    for (int i = 0; i < labels.size(); ++i)
    {
        auto seg = Rectangle<float> (area.getX() + w * (float) i, area.getY(), w, area.getHeight()).reduced (2.5f);
        if (i == selected)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (seg, 4.5f);
            g.setColour (accent.withAlpha (0.85f));
            g.drawRoundedRectangle (seg, 4.5f, 1.0f);
        }
        else if (i == hovered)
        {
            g.setColour (Colours::white.withAlpha (0.03f));
            g.fillRoundedRectangle (seg, 4.5f);
        }
        if (i > 0 && i != selected && i - 1 != selected)
        {
            g.setColour (colours::border);
            g.drawVerticalLine ((int) (area.getX() + w * (float) i), area.getY() + 6.0f, area.getBottom() - 6.0f);
        }
        g.setFont (font (jlimit (10.0f, 12.5f, jmin (area.getHeight() * 0.36f, seg.getWidth() * 0.16f)), true));
        g.setColour (i == selected ? accent : colours::textDim);
        g.drawFittedText (labels[i].toUpperCase(), seg.toNearestInt(), Justification::centred, 1, 0.7f);
    }
}

void SegmentedControl::mouseDown (const MouseEvent& e)
{
    const int i = segmentAt (e.position);
    if (i >= 0 && i != selected)
        attachment.setValueAsCompleteGesture ((float) i);
}

void SegmentedControl::mouseMove (const MouseEvent& e)
{
    const int h = segmentAt (e.position);
    if (h != hovered) { hovered = h; repaint(); }
}

void SegmentedControl::mouseExit (const MouseEvent&)
{
    hovered = -1;
    repaint();
}
}

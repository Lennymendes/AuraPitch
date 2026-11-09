// AuraKey — NextDSP · AuraSuite
#include "CircleOfFifths.h"

#include "../../../Source/GUI/NextDSPLookAndFeel.h"
#include "../DSP/KeyTypes.h"

#include <cmath>
#include <tuple>

namespace nextdsp
{
using namespace juce;
using namespace aura::key;

namespace
{
    constexpr float kTwoPi = MathConstants<float>::twoPi;
    constexpr float kStep = kTwoPi / 12.0f;

    String scaleNotes (int index)
    {
        const int tonic = index % 12;
        const bool minor = index >= 12;
        static constexpr int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[] = { 0, 2, 3, 5, 7, 8, 10 };
        StringArray s;
        for (int i = 0; i < 7; ++i)
            s.add (pitchName (tonic + (minor ? minorSteps[i] : majorSteps[i])));
        return s.joinIntoString (" ");
    }
}

CircleOfFifths::CircleOfFifths()
{
    post.fill (1.0 / 24.0);
    setRepaintsOnMouseActivity (false);
}

void CircleOfFifths::setLive (const std::array<double, 24>& p, int tonic, bool minor, bool hasKey)
{
    const int k = hasKey ? keyIndex (tonic, minor) : -1;
    if (p != post || k != liveKey)
    {
        post = p;
        liveKey = k;
        repaint();
    }
}

void CircleOfFifths::setFileKey (int tonic, bool minor)
{
    const int k = tonic >= 0 ? keyIndex (tonic, minor) : -1;
    if (k != fileKey) { fileKey = k; repaint(); }
}

void CircleOfFifths::setPinned (int index)
{
    if (index != pinned) { pinned = index; repaint(); }
}

CircleOfFifths::Geometry CircleOfFifths::geometry() const
{
    const auto b = getLocalBounds().toFloat();
    const float size = jmin (b.getWidth(), b.getHeight());
    Geometry g;
    g.c = b.getCentre();
    g.rOut = size * 0.5f - 6.0f;
    g.rMid = g.rOut * 0.68f;
    g.rIn = g.rOut * 0.38f;
    return g;
}

int CircleOfFifths::keyAt (Point<float> p) const
{
    const auto g = geometry();
    const auto d = p - g.c;
    const float r = d.getDistanceFromOrigin();
    if (r < g.rIn || r > g.rOut) return -1;
    float a = std::atan2 (d.x, -d.y);                          // 0 en haut, sens horaire
    if (a < 0.0f) a += kTwoPi;
    const int pos = (int) std::floor ((a + 0.5f * kStep) / kStep) % 12;
    const bool minor = r < g.rMid;
    return keyIndex (tonicAtFifthsPosition (pos, minor), minor);
}

Point<float> CircleOfFifths::centreOfKey (int index) const
{
    const auto g = geometry();
    const bool minor = index >= 12;
    const float a = (float) fifthsPosition (index % 12, minor) * kStep;
    const float r = minor ? 0.5f * (g.rIn + g.rMid) : 0.5f * (g.rMid + g.rOut);
    return { g.c.x + r * std::sin (a), g.c.y - r * std::cos (a) };
}

void CircleOfFifths::paint (Graphics& g)
{
    const auto geo = geometry();
    double maxP = 1e-12;
    for (double v : post) maxP = jmax (maxP, v);
    // contraste : rien d'allumé tant que les 24 clés sont équiprobables (silence, début d'écoute)
    const float contrast = jlimit (0.0f, 1.0f, (float) (maxP * 24.0 - 1.0) / 3.0f);

    auto segment = [&] (int pos, bool minor, float inset = 0.0f)
    {
        Path p;
        const float r0 = (minor ? geo.rIn : geo.rMid) + inset, r1 = (minor ? geo.rMid : geo.rOut) - inset;
        const float a0 = ((float) pos - 0.5f) * kStep, a1 = ((float) pos + 0.5f) * kStep;
        p.addPieSegment (geo.c.x - r1, geo.c.y - r1, 2.0f * r1, 2.0f * r1, a0, a1, r0 / r1);
        return p;
    };

    // fond des anneaux
    g.setColour (colours::panelHi);
    g.fillEllipse (Rectangle<float> (2.0f * geo.rOut, 2.0f * geo.rOut).withCentre (geo.c));

    for (int ring = 0; ring < 2; ++ring)
        for (int pos = 0; pos < 12; ++pos)
        {
            const bool minor = ring == 1;
            const int k = keyIndex (tonicAtFifthsPosition (pos, minor), minor);
            const auto accent = minor ? colours::violet : colours::cyan;
            const float level = contrast * (float) std::sqrt (post[(size_t) k] / maxP);
            const auto seg = segment (pos, minor);
            g.setColour (accent.withAlpha (0.05f + 0.30f * level));
            g.fillPath (seg);
            if (k == liveKey)
            {
                g.setColour (accent.withAlpha (0.55f));
                g.fillPath (seg);
                strokeGlow (g, segment (pos, minor, 1.5f), accent, 1.6f, 1.0f);
            }
            if (k == hovered && k != pinned)
            {
                g.setColour (Colours::white.withAlpha (0.08f));
                g.fillPath (seg);
            }
        }

    // séparateurs
    g.setColour (colours::background);
    for (int pos = 0; pos < 12; ++pos)
    {
        const float a = ((float) pos - 0.5f) * kStep;
        g.drawLine (geo.c.x + geo.rIn * std::sin (a), geo.c.y - geo.rIn * std::cos (a),
                    geo.c.x + geo.rOut * std::sin (a), geo.c.y - geo.rOut * std::cos (a), 2.0f);
    }
    g.drawEllipse (Rectangle<float> (2.0f * geo.rMid, 2.0f * geo.rMid).withCentre (geo.c), 2.0f);

    // clé du fichier (ambre), clé épinglée (blanc)
    for (auto [k, colour, width] : { std::tuple<int, Colour, float> { fileKey, colours::amber, 2.0f },
                                     std::tuple<int, Colour, float> { pinned, colours::text, 1.6f } })
        if (k >= 0)
        {
            const bool minor = k >= 12;
            g.setColour (colour);
            g.strokePath (segment (fifthsPosition (k % 12, minor), minor, 2.5f), PathStrokeType (width));
        }

    // étiquettes
    const float scale = jlimit (0.75f, 1.4f, geo.rOut / 150.0f);
    for (int ring = 0; ring < 2; ++ring)
        for (int pos = 0; pos < 12; ++pos)
        {
            const bool minor = ring == 1;
            const int tonic = tonicAtFifthsPosition (pos, minor);
            const int k = keyIndex (tonic, minor);
            const auto c = centreOfKey (k);
            const bool lit = k == liveKey || k == pinned || k == hovered;
            g.setColour (lit ? colours::text : colours::text.withAlpha (0.72f));
            g.setFont (font ((minor ? 11.5f : 14.0f) * scale, true));
            g.drawText (keyShortName (tonic, minor), Rectangle<float> (60.0f, 18.0f * scale).withCentre (c.translated (0.0f, -5.0f * scale)),
                        Justification::centred);
            g.setFont (monoFont (8.5f * scale));
            g.setColour (lit ? colours::textDim.brighter (0.3f) : colours::textFaint);
            g.drawText (camelot (tonic, minor), Rectangle<float> (40.0f, 12.0f * scale).withCentre (c.translated (0.0f, 8.0f * scale)),
                        Justification::centred);
        }

    // centre : clé épinglée, survolée ou live
    const int shown = pinned >= 0 ? pinned : hovered >= 0 ? hovered : liveKey;
    auto inner = Rectangle<float> (2.0f * geo.rIn, 2.0f * geo.rIn).withCentre (geo.c).reduced (4.0f);
    g.setColour (colours::background);
    g.fillEllipse (inner);
    g.setColour (colours::border);
    g.drawEllipse (inner, 1.0f);
    auto t = inner.reduced (inner.getWidth() * 0.12f, inner.getHeight() * 0.2f);
    if (shown < 0)
    {
        g.setFont (font (10.5f * scale, true));
        g.setColour (colours::textDim);
        g.drawText ("LISTENING", t, Justification::centred);
        return;
    }
    const int tonic = shown % 12;
    const bool minor = shown >= 12;
    const auto accent = minor ? colours::violet : colours::cyan;
    g.setFont (font (9.0f * scale, true));
    g.setColour (shown == pinned ? colours::text.withAlpha (0.7f) : shown == hovered ? colours::textDim : accent);
    g.drawText (shown == pinned ? "PINNED" : shown == hovered ? "KEY" : "LIVE", t.removeFromTop (t.getHeight() * 0.2f), Justification::centred);
    g.setFont (font (20.0f * scale, true));
    g.setColour (colours::text);
    g.drawText (keyShortName (tonic, minor), t.removeFromTop (t.getHeight() * 0.42f), Justification::centred);
    g.setFont (monoFont (10.0f * scale));
    g.setColour (accent);
    g.drawText (String (camelot (tonic, minor)) + "  " + String (roundToInt (100.0 * post[(size_t) shown])) + " %", t.removeFromTop (t.getHeight() * 0.45f),
                Justification::centred);
    g.setFont (font (8.5f * scale));
    g.setColour (colours::textDim);
    g.drawFittedText (scaleNotes (shown), t.toNearestInt(), Justification::centred, 1, 0.8f);
}

void CircleOfFifths::mouseMove (const MouseEvent& e)
{
    const int k = keyAt (e.position);
    if (k != hovered)
    {
        hovered = k;
        setMouseCursor (k >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
        repaint();
    }
}

void CircleOfFifths::mouseExit (const MouseEvent&)
{
    if (hovered >= 0) { hovered = -1; repaint(); }
}

void CircleOfFifths::mouseDown (const MouseEvent& e)
{
    const int k = keyAt (e.position);
    const int next = (k < 0 || k == pinned) ? -1 : k;
    setPinned (next);
    if (onPinChanged) onPinChanged (next);
}
}

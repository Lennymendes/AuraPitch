// AuraPitch — NextDSP · AuraSuite
#include "AuraCore.h"

#include <cmath>

namespace nextdsp
{
using namespace juce;

namespace
{
    constexpr float kTwoPi = MathConstants<float>::twoPi;
    const char* const kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    /** Position sur la couronne, 0 … 12 (C = 0), d'une hauteur MIDI. */
    float ringPosition (float midi) noexcept
    {
        float p = std::fmod (midi, 12.0f);
        return p < 0.0f ? p + 12.0f : p;
    }

    /** Lissage circulaire sur la couronne (pas de tour complet quand la voix saute d'une octave). */
    float approachOnRing (float shown, float wanted, float k) noexcept
    {
        float d = wanted - shown;
        d -= 12.0f * std::round (d / 12.0f);
        return ringPosition (shown + k * d);
    }
}

AuraCore::AuraCore()
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
}

float AuraCore::angleFor (float midi) noexcept { return ringPosition (midi) / 12.0f * kTwoPi; }

void AuraCore::setScale (std::uint16_t activeMask, int tonic)
{
    if (activeMask != mask || tonic != key)
    {
        mask = activeMask;
        key = tonic;
        repaint();
    }
}

void AuraCore::push (const aura::pitch::PitchFrame& f, double referenceA4)
{
    if (! f.voiced) return;
    silence = 0.0;
    target = roundToInt (f.targetMidi);
    wantedIn  = jlimit (-50.0f, 50.0f, (f.inputMidi - (float) target) * 100.0f);
    wantedOut = jlimit (-50.0f, 50.0f, (f.outputMidi - (float) target) * 100.0f);
    wantedInMidi  = ringPosition (f.inputMidi);
    wantedOutMidi = ringPosition (f.outputMidi);
    inputHz = referenceA4 * std::exp2 ((f.inputMidi - 69.0) / 12.0);
    // −60 dBFS -> 0, −12 dBFS -> 1
    wantedLevel = jlimit (0.0, 1.0, ((double) f.levelDb + 60.0) / 48.0);
}

void AuraCore::tick (double dt)
{
    silence += dt;
    const float k = (float) (1.0 - std::exp (-dt / 0.05));
    shownIn  += k * (wantedIn - shownIn);
    shownOut += k * (wantedOut - shownOut);
    const float kr = (float) (1.0 - std::exp (-dt / 0.035));
    if (activity < 0.02)
    {
        inputMidi = wantedInMidi; outputMidi = wantedOutMidi;     // apparition : pas de glissement depuis l'ancienne note
    }
    inputMidi  = approachOnRing (inputMidi, wantedInMidi, kr);
    outputMidi = approachOnRing (outputMidi, wantedOutMidi, kr);
    activity += (silence < 0.15 ? 1.0 - activity : -activity) * (1.0 - std::exp (-dt / 0.12));
    level += ((silence < 0.15 ? wantedLevel : 0.0) - level) * (1.0 - std::exp (-dt / 0.08));
    phase = std::fmod (phase + dt * (0.35 + 0.9 * level), 1000.0);
    for (int pc = 0; pc < 12; ++pc)
    {
        const float want = (target >= 0 && target % 12 == pc) ? (float) activity : 0.0f;
        glow[(size_t) pc] += (want - glow[(size_t) pc]) * (float) (1.0 - std::exp (-dt / (want > glow[(size_t) pc] ? 0.04 : 0.35)));
    }
    repaint();
}

void AuraCore::paint (Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float size = jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float rOut = size * 0.5f - 4.0f;              // bord de la couronne
    const float rRing = rOut * 0.80f;                    // intérieur de la couronne
    const float rHalo = rOut * 0.66f;                    // anneau de particules
    const float alive = (float) activity;
    const bool hasTarget = target >= 0 && alive > 0.02f;

    // Fond : disque sombre, lueur centrale qui suit le niveau
    {
        ColourGradient bg (colours::panelHi.brighter (0.06f), c.x, c.y - rOut * 0.3f, colours::background, c.x, c.y + rOut, true);
        g.setGradientFill (bg);
        g.fillEllipse (Rectangle<float> (2.0f * rOut, 2.0f * rOut).withCentre (c));
        const float glowR = rHalo * (0.9f + 0.35f * (float) level);
        g.setGradientFill (ColourGradient (colours::cyan.withAlpha (0.10f + 0.16f * (float) level * alive), c.x, c.y,
                                           Colours::transparentBlack, c.x + glowR, c.y, true));
        g.fillEllipse (Rectangle<float> (2.0f * glowR, 2.0f * glowR).withCentre (c));
        g.setColour (colours::border);
        g.drawEllipse (Rectangle<float> (2.0f * rOut, 2.0f * rOut).withCentre (c), 1.2f);
    }

    // Couronne des 12 notes
    const float step = kTwoPi / 12.0f;
    for (int pc = 0; pc < 12; ++pc)
    {
        const bool inScale = ((mask >> pc) & 1u) != 0;
        const float a0 = ((float) pc - 0.5f) * step + 0.025f, a1 = ((float) pc + 0.5f) * step - 0.025f;
        Path seg;
        seg.addPieSegment (c.x - rOut + 3.0f, c.y - rOut + 3.0f, 2.0f * (rOut - 3.0f), 2.0f * (rOut - 3.0f), a0, a1, rRing / (rOut - 3.0f));
        const float lit = glow[(size_t) pc];
        g.setColour (inScale ? colours::cyan.withAlpha (0.10f + 0.45f * lit) : colours::panel.withAlpha (0.9f));
        g.fillPath (seg);
        if (inScale)
        {
            g.setColour (colours::cyan.withAlpha (0.22f + 0.6f * lit));
            g.strokePath (seg, PathStrokeType (1.0f));
        }
        if (lit > 0.05f)
        {
            Path edge;
            edge.addCentredArc (c.x, c.y, rOut - 4.0f, rOut - 4.0f, 0.0f, a0 + 0.02f, a1 - 0.02f, true);
            strokeGlow (g, edge, colours::cyan.withAlpha (lit), 2.2f, lit);
        }
        // nom de la note dans la case
        const auto p = c.getPointOnCircumference (0.5f * (rRing + rOut - 3.0f), (float) pc * step);
        g.setFont (font (jlimit (9.0f, 15.0f, size * 0.042f), true));
        g.setColour (lit > 0.3f ? colours::text : inScale ? colours::text.withAlpha (0.78f) : colours::textFaint);
        g.drawText (kNames[pc], Rectangle<float> (34.0f, 18.0f).withCentre (p), Justification::centred);
        // tonique : petit point
        if (pc == key % 12 && inScale)
        {
            const auto d = c.getPointOnCircumference (rRing + 5.0f, (float) pc * step);
            g.setColour (colours::cyan.withAlpha (0.8f));
            g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre (d));
        }
    }

    // Halo de particules : il respire avec le niveau, s'agite avec l'écart d'entrée
    {
        constexpr int n = 96;
        const float agitation = jlimit (0.0f, 1.0f, std::abs (shownIn) / 50.0f);
        const float amp = rHalo * (0.015f + 0.06f * (float) level + 0.05f * agitation) * (0.25f + 0.75f * alive);
        Path halo;
        for (int i = 0; i <= n; ++i)
        {
            const float a = (float) i / n * kTwoPi;
            const float w = std::sin (3.0f * a + (float) phase * 2.1f) * 0.55f + std::sin (7.0f * a - (float) phase * 3.3f) * 0.3f
                          + std::sin (13.0f * a + (float) phase * 5.7f) * 0.15f;
            const auto p = c.getPointOnCircumference (rHalo * (0.94f + 0.08f * (float) level) + amp * w, a);
            if (i == 0) halo.startNewSubPath (p); else halo.lineTo (p);
        }
        halo.closeSubPath();
        const auto haloColour = colours::violet.interpolatedWith (colours::cyan, 0.35f + 0.65f * (1.0f - agitation));
        strokeGlow (g, halo, haloColour.withAlpha (0.25f + 0.65f * alive), 1.4f, 0.4f + 0.6f * alive);
        // poussière : quelques points sur l'anneau
        for (int i = 0; i < 36; ++i)
        {
            const float a = (float) i / 36.0f * kTwoPi + (float) phase * (i % 2 == 0 ? 0.21f : -0.13f);
            const float rr = rHalo * (0.86f + 0.12f * std::sin ((float) phase * 1.7f + (float) i * 2.4f));
            const float s = 1.2f + 1.6f * (float) level * alive;
            g.setColour ((i % 3 == 0 ? colours::violet : colours::cyan).withAlpha (0.18f + 0.5f * alive));
            g.fillEllipse (Rectangle<float> (s, s).withCentre (c.getPointOnCircumference (rr, a)));
        }
    }

    // Repères sur la couronne : voix entrante (ambre) et corrigée (cyan)
    if (alive > 0.02f)
    {
        const float rMark = 0.5f * (rRing + rOut - 3.0f) - (rOut - rRing) * 0.62f;
        auto marker = [&] (float pos, Colour col, float d)
        {
            const auto p = c.getPointOnCircumference (rMark, pos / 12.0f * kTwoPi);
            g.setColour (col.withAlpha (0.25f * alive));
            g.fillEllipse (Rectangle<float> (d * 2.2f, d * 2.2f).withCentre (p));
            g.setColour (col.withAlpha (alive));
            g.fillEllipse (Rectangle<float> (d, d).withCentre (p));
        };
        // trait de correction : de l'entrée à la sortie, le long de la couronne
        float d = outputMidi - inputMidi;
        d -= 12.0f * std::round (d / 12.0f);
        if (std::abs (d) > 0.02f)
        {
            Path arc;
            const float a0 = inputMidi / 12.0f * kTwoPi, a1 = (inputMidi + d) / 12.0f * kTwoPi;
            arc.addCentredArc (c.x, c.y, rMark, rMark, 0.0f, jmin (a0, a1), jmax (a0, a1), true);
            g.setColour (colours::amber.interpolatedWith (colours::cyan, 0.5f).withAlpha (0.55f * alive));
            g.strokePath (arc, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
        }
        marker (inputMidi, colours::amber, 7.0f);
        marker (outputMidi, colours::cyan, 9.0f);
    }

    // Centre
    const float inner = rHalo * 0.86f;
    auto t = Rectangle<float> (2.0f * inner, 2.0f * inner).withCentre (c);
    if (! hasTarget)
    {
        g.setFont (font (jlimit (11.0f, 18.0f, size * 0.05f), true));
        g.setColour (colours::textDim);
        g.drawText ("LISTENING", t, Justification::centred);
        g.setFont (font (jlimit (9.0f, 12.0f, size * 0.034f)));
        g.setColour (colours::textFaint);
        g.drawText ("sing or play a note", t.translated (0.0f, size * 0.07f), Justification::centred);
        return;
    }
    const String note = String (kNames[target % 12]) + String (target / 12 - 1);
    auto top = t.removeFromTop (t.getHeight() * 0.64f);
    g.setFont (font (jlimit (28.0f, 76.0f, size * 0.20f), true));
    g.setColour (colours::text.withAlpha (0.35f + 0.65f * alive));
    g.drawText (note, top, Justification::centredBottom);
    g.setFont (monoFont (jlimit (10.0f, 17.0f, size * 0.05f)));
    auto row = t.removeFromTop (t.getHeight() * 0.42f);
    g.setColour (colours::amber.withAlpha (0.4f + 0.6f * alive));
    g.drawText ((shownIn >= 0.0f ? "+" : "") + String (shownIn, 1) + " ct", row, Justification::centred);
    const bool locked = std::abs (shownOut) < 3.0f;
    g.setFont (font (jlimit (8.5f, 12.0f, size * 0.036f), true));
    g.setColour (locked ? colours::cyan.withAlpha (0.4f + 0.6f * alive) : colours::textDim);
    g.drawText (locked ? String ("LOCKED") : String ("OUT ") + (shownOut >= 0.0f ? "+" : "") + String (shownOut, 1) + " ct",
                t.removeFromTop (t.getHeight() * 0.6f), Justification::centredTop);

    // fréquence d'entrée, sous l'état de sortie
    g.setFont (monoFont (jlimit (8.0f, 10.5f, size * 0.03f)));
    g.setColour (colours::textDim.withAlpha (0.4f + 0.5f * alive));
    g.drawText (String (inputHz, 1) + " Hz", t, Justification::centredTop);
}
}

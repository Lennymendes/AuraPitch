// AuraPitch — NextDSP · AuraSuite
#include "PitchDisplay.h"

namespace nextdsp
{
using namespace juce;

String midiNoteName (int midiNote)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pc = ((midiNote % 12) + 12) % 12;
    return String (names[pc]) + String (midiNote / 12 - 1);
}

//==============================================================================================================
PitchGraph::PitchGraph()
{
    points.resize (16384);
    setOpaque (false);
    freezeButton.setClickingTogglesState (true);
    freezeButton.getProperties().set ("accent", (int64) colours::cyan.getARGB());
    freezeButton.setTooltip ("Freeze the pitch history (audio keeps running)");
    freezeButton.onClick = [this] { setFrozen (freezeButton.getToggleState()); };
    addAndMakeVisible (freezeButton);
}

void PitchGraph::setFrozen (bool shouldFreeze)
{
    frozen = shouldFreeze;
    freezeButton.setToggleState (frozen, dontSendNotification);
    freezeButton.setButtonText (frozen ? "FROZEN" : "FREEZE");
    repaint();
}

void PitchGraph::resized()
{
    // en haut, à gauche de la légende
    const auto legend = getLocalBounds().reduced (14, 6).removeFromTop (20);
    freezeButton.setBounds (legend.withTrimmedRight (240).removeFromRight (72));
}

void PitchGraph::push (const aura::pitch::PitchFrame& f, double sampleRate)
{
    if (sampleRate <= 0.0 || frozen) return;     // figé : les trames sont lues (la FIFO ne déborde pas) mais ignorées
    const double t = (double) f.samplePosition / sampleRate;
    if (count > 0 && t + 1.0 < latestTime)          // horloge remise à zéro (prepareToPlay, changement de fs)
    {
        count = 0;
        displayTime = t;
    }
    points[head] = { t, f.inputMidi, f.outputMidi, f.targetMidi, f.voiced };
    head = (head + 1) % points.size();
    count = std::min (count + 1, points.size());
    latestTime = jmax (latestTime, t);
    if (count == 1) displayTime = t;
}

void PitchGraph::tick (double dt)
{
    if (frozen) return;
    // Défilement : on suit l'horloge audio avec un léger lissage (les trames arrivent par paquets de blocs).
    displayTime += dt;
    const double err = latestTime - displayTime;
    if (std::abs (err) > 0.25) displayTime = latestTime;
    else                       displayTime += 0.2 * err;
    if (displayTime > latestTime + 0.05) displayTime = latestTime + 0.05;   // transport arrêté : le graphe se fige

    // Cadrage vertical : moyenne des hauteurs voisées des 1,5 dernières secondes.
    double sum = 0.0; int n = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto& p = points[(head + points.size() - 1 - i) % points.size()];
        if (p.t < latestTime - 1.5) break;
        if (p.voiced) { sum += p.in; ++n; }
    }
    if (n > 8) wantedCentre = (float) (sum / n);
    viewCentre += 0.08f * (wantedCentre - viewCentre);
    repaint();
}

Rectangle<float> PitchGraph::plotArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f).withTrimmedLeft (46.0f).withTrimmedTop (28.0f).withTrimmedBottom (8.0f).withTrimmedRight (10.0f);
}

float PitchGraph::yFor (float midi, Rectangle<float> a) const
{
    const float semis = jlimit (9.0f, 24.0f, a.getHeight() / 22.0f);
    return a.getCentreY() - (midi - viewCentre) / semis * a.getHeight();
}

void PitchGraph::paint (Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    const auto a = plotArea();
    const float semis = jlimit (9.0f, 24.0f, a.getHeight() / 22.0f);
    const float rowH = a.getHeight() / semis;

    // Grille de notes
    const int lo = (int) std::floor (viewCentre - semis * 0.5f) - 1, hi = (int) std::ceil (viewCentre + semis * 0.5f) + 1;
    int targetNote = -1000;
    for (size_t i = 0; i < count; ++i)
    {
        const auto& p = points[(head + points.size() - 1 - i) % points.size()];
        if (p.t < latestTime - 0.15) break;
        if (p.voiced) { targetNote = roundToInt (p.target); break; }
    }
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (a.withLeft (2.0f).toNearestInt());
        for (int m = lo; m <= hi; ++m)
        {
            const float y = yFor ((float) m, a);
            const int pc = ((m % 12) + 12) % 12;
            const bool inScale = ((mask >> pc) & 1u) != 0;
            const bool midiHeld = m >= 0 && m < 128 && ((midiNotes[(size_t) (m >> 6)] >> (m & 63)) & 1u) != 0;
            if (midiHeld)
            {
                g.setColour (colours::amber.withAlpha (0.10f));
                g.fillRect (Rectangle<float> (a.getX(), y - rowH * 0.5f, a.getWidth(), rowH));
                g.setColour (colours::amber.withAlpha (0.55f));
                g.drawHorizontalLine (roundToInt (y), a.getX(), a.getRight());
            }
            if (m == targetNote)
            {
                g.setColour (colours::cyan.withAlpha (0.07f));
                g.fillRect (Rectangle<float> (a.getX(), y - rowH * 0.5f, a.getWidth(), rowH));
            }
            else if (inScale)
            {
                g.setColour (Colours::white.withAlpha (0.018f));
                g.fillRect (Rectangle<float> (a.getX(), y - rowH * 0.5f, a.getWidth(), rowH));
            }
            if (! midiHeld)
            {
                g.setColour (inScale ? colours::grid.brighter (0.25f) : colours::grid);
                g.drawHorizontalLine (roundToInt (y), a.getX(), a.getRight());
            }

            g.setFont (monoFont (jlimit (9.0f, 11.5f, rowH * 0.62f)));
            g.setColour (m == targetNote ? colours::cyan : midiHeld ? colours::amber : inScale ? colours::textDim : colours::textFaint);
            g.drawText (midiNoteName (m), Rectangle<float> (4.0f, y - 8.0f, 38.0f, 16.0f).toNearestInt(),
                        pc == key ? Justification::centredRight : Justification::centredRight);
        }
        // Repères d'une seconde
        g.setColour (colours::grid);
        for (double sec = std::floor (displayTime); sec > displayTime - windowSeconds; sec -= 1.0)
        {
            const float x = a.getRight() - (float) ((displayTime - sec) / windowSeconds) * a.getWidth();
            g.drawVerticalLine (roundToInt (x), a.getY(), a.getBottom());
        }
    }

    // Courbes
    Path in, out;
    bool inOpen = false, outOpen = false;
    float lastX = -1.0e9f;
    double lastT = -1.0;
    Point<float> lastIn, lastOut;
    const size_t start = (head + points.size() - count) % points.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto& p = points[(start + i) % points.size()];
        if (p.t < displayTime - windowSeconds - 0.1) continue;
        const float x = a.getRight() - (float) ((displayTime - p.t) / windowSeconds) * a.getWidth();
        const bool gap = ! p.voiced || (lastT >= 0.0 && p.t - lastT > 0.05);
        lastT = p.t;
        if (gap) { inOpen = outOpen = false; if (! p.voiced) continue; }
        if (inOpen && x - lastX < 0.75f) continue;                      // ≤ 1 point par pixel
        lastX = x;
        const Point<float> pi (x, yFor (p.in, a)), po (x, yFor (p.out, a));
        if (! inOpen)  { in.startNewSubPath (pi);  inOpen = true; }  else in.lineTo (pi);
        if (! outOpen) { out.startNewSubPath (po); outOpen = true; } else out.lineTo (po);
        lastIn = pi; lastOut = po;
    }

    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (a.toNearestInt());
        // Entrée : violet (ancien) -> ambre (récent)
        g.setGradientFill (ColourGradient (colours::violet.withAlpha (0.55f), a.getX(), 0.0f, colours::amber, a.getRight(), 0.0f, false));
        g.strokePath (in, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
        strokeGlow (g, out, colours::cyan, 2.0f, 1.2f);

        if (latestTime - displayTime > -0.04 && targetNote > -1000)
        {
            g.setColour (colours::amber);
            g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (lastIn));
            g.setColour (colours::cyan.withAlpha (0.25f));
            g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (lastOut));
            g.setColour (colours::cyan);
            g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (lastOut));
        }
    }

    // Légende
    auto legend = getLocalBounds().toFloat().reduced (14.0f, 8.0f).removeFromTop (16.0f);
    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("PITCH HISTORY", legend, Justification::centredLeft);
    auto right = legend.removeFromRight (230.0f);
    auto item = [&] (Colour c, const String& txt)
    {
        auto r = right.removeFromLeft (115.0f);
        g.setColour (c);
        g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre ({ r.getX() + 4.0f, r.getCentreY() }));
        g.setColour (colours::textDim);
        g.drawText (txt, r.withTrimmedLeft (14.0f), Justification::centredLeft);
    };
    item (colours::amber, "INPUT");
    item (colours::cyan, "CORRECTED");
}

//==============================================================================================================
void TunerArc::push (const aura::pitch::PitchFrame& f, double referenceA4)
{
    if (! f.voiced) return;
    silence = 0.0;
    target = roundToInt (f.targetMidi);
    wantedIn  = jlimit (-50.0f, 50.0f, (f.inputMidi - (float) target) * 100.0f);
    wantedOut = jlimit (-50.0f, 50.0f, (f.outputMidi - (float) target) * 100.0f);
    inputMidi = f.inputMidi;
    inputHz = referenceA4 * std::exp2 ((f.inputMidi - 69.0) / 12.0);
}

void TunerArc::tick (double dt)
{
    silence += dt;
    const float k = (float) (1.0 - std::exp (-dt / 0.05));
    shownIn  += k * (wantedIn - shownIn);
    shownOut += k * (wantedOut - shownOut);
    activity += (silence < 0.15 ? 1.0 - activity : -activity) * (1.0 - std::exp (-dt / 0.12));
    repaint();
}

void TunerArc::paint (Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    auto r = getLocalBounds().toFloat().reduced (14.0f, 10.0f);

    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("TUNER", r.removeFromTop (16.0f), Justification::centredLeft);

    const float size = jmin (r.getWidth(), r.getHeight() * 1.15f);
    const auto centre = Point<float> (r.getCentreX(), r.getY() + size * 0.56f);
    const float radius = size * 0.44f;
    const float span = MathConstants<float>::pi * 0.62f;               // ±112°
    auto angleFor = [span] (float cents) { return jlimit (-1.0f, 1.0f, cents / 50.0f) * span; };
    const float alive = (float) activity;

    // Graduations
    for (int c = -50; c <= 50; c += 5)
    {
        const float ang = angleFor ((float) c);
        const bool major = c % 25 == 0;
        const float r0 = radius - (major ? 12.0f : 6.0f);
        g.setColour (c == 0 ? colours::cyan.withAlpha (0.8f) : (major ? colours::textDim : colours::textFaint));
        g.drawLine (Line<float> (centre.getPointOnCircumference (r0, ang), centre.getPointOnCircumference (radius, ang)), major ? 1.6f : 1.0f);
        if (major)
        {
            g.setFont (monoFont (10.0f));
            const auto p = centre.getPointOnCircumference (radius + 12.0f, ang);
            g.drawText (c > 0 ? "+" + String (c) : String (c), Rectangle<float> (30.0f, 12.0f).withCentre (p), Justification::centred);
        }
    }

    // Rail + zone juste (±5 cents)
    Path rail;
    rail.addCentredArc (centre.x, centre.y, radius + 3.0f, radius + 3.0f, 0.0f, -span, span, true);
    g.setColour (colours::border);
    g.strokePath (rail, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));
    Path zone;
    zone.addCentredArc (centre.x, centre.y, radius + 3.0f, radius + 3.0f, 0.0f, angleFor (-5.0f), angleFor (5.0f), true);
    strokeGlow (g, zone, colours::cyan.withAlpha (0.35f + 0.65f * alive), 3.0f, 0.8f * alive);

    // Écart de la voix entrante (aiguille ambre) et de la sortie corrigée (repère cyan)
    const bool inTune = std::abs (shownIn) < 5.0f;
    const auto needleColour = (inTune ? colours::cyan : colours::amber).withAlpha (0.25f + 0.75f * alive);
    {
        const float ang = angleFor (shownIn);
        Path needle;
        needle.startNewSubPath (centre.getPointOnCircumference (radius * 0.70f, ang));
        needle.lineTo (centre.getPointOnCircumference (radius - 4.0f, ang));
        strokeGlow (g, needle, needleColour, 2.2f, alive);
        g.setColour (needleColour);
        g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (centre.getPointOnCircumference (radius + 3.0f, ang)));
    }
    {
        const float ang = angleFor (shownOut);
        // repère cyan à l'intérieur de l'arc, pointe vers l'extérieur
        const auto p = centre.getPointOnCircumference (radius - 20.0f, ang);
        Path tri;
        tri.addTriangle (p.translated (-5.0f, 7.0f), p.translated (5.0f, 7.0f), p.translated (0.0f, -1.0f));
        tri.applyTransform (AffineTransform::rotation (ang, p.x, p.y));
        g.setColour (colours::cyan.withAlpha (0.3f + 0.7f * alive));
        g.fillPath (tri);
    }

    // Note cible en grand
    const String note = target >= 0 && alive > 0.02f ? midiNoteName (target) : String ("--");
    g.setFont (font (radius * 0.52f, true));
    g.setColour (alive > 0.02f ? colours::text : colours::textFaint);
    g.drawText (note, Rectangle<float> (radius * 2.0f, radius * 0.6f).withCentre (centre.translated (0.0f, -radius * 0.05f)), Justification::centred);

    const String cents = alive > 0.02f ? (shownIn >= 0.0f ? "+" : "-") + String (std::abs (shownIn), 1) + " ct" : String ("-- ct");
    g.setFont (monoFont (14.0f));
    g.setColour (alive > 0.02f ? needleColour : colours::textFaint);
    g.drawText (cents, Rectangle<float> (radius * 2.0f, 18.0f).withCentre (centre.translated (0.0f, radius * 0.32f)), Justification::centred);

    auto info = getLocalBounds().toFloat().reduced (14.0f, 10.0f).removeFromBottom (34.0f);
    g.setFont (monoFont (11.0f));
    auto col = info.removeFromLeft (info.getWidth() * 0.5f);
    g.setColour (colours::textFaint);
    g.drawText ("INPUT", col.removeFromTop (14.0f), Justification::centred);
    g.setColour (colours::amber.withAlpha (0.4f + 0.6f * alive));
    g.drawText (alive > 0.02f ? String (inputHz, 1) + " Hz" : String ("--"), col, Justification::centred);
    g.setColour (colours::textFaint);
    g.drawText ("OUTPUT", info.removeFromTop (14.0f), Justification::centred);
    g.setColour (colours::cyan.withAlpha (0.4f + 0.6f * alive));
    g.drawText (alive > 0.02f ? (shownOut >= 0.0f ? "+" : "-") + String (std::abs (shownOut), 1) + " ct" : String ("--"), info, Justification::centred);
}
}

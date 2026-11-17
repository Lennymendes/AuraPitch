// AuraPitch — NextDSP · AuraSuite
#include "ScaleKeyboard.h"

#include "../Parameters.h"

namespace nextdsp
{
using namespace juce;
using aura::pitch::ScaleQuantizer;
using aura::pitch::ScaleType;

namespace
{
    constexpr bool isBlack (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
    constexpr int whiteIndex[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };   // touche blanche à gauche des noires
}

ScaleKeyboard::ScaleKeyboard (AudioProcessorValueTreeState& s) : state (s)
{
    lastShown.fill (-1);
    setRepaintsOnMouseActivity (true);
    setTooltip ("Click: active / removed  -  Alt-click or right-click: bypass  -  top strip: sung notes (amber) and corrected notes (cyan)");
}

int ScaleKeyboard::tonic() const
{
    return roundToInt (state.getRawParameterValue (aura::params::key)->load());
}

std::array<int, 12> ScaleKeyboard::displayedStates() const
{
    std::array<int, 12> st {};
    const auto scale = (ScaleType) roundToInt (state.getRawParameterValue (aura::params::scale)->load());
    if (scale == ScaleType::Custom)
    {
        for (int pc = 0; pc < 12; ++pc)
            st[(size_t) pc] = roundToInt (state.getRawParameterValue (aura::params::noteId (pc))->load());
    }
    else
    {
        const auto mask = ScaleQuantizer::rotate (ScaleQuantizer::intervalsOf (scale), tonic());
        for (int pc = 0; pc < 12; ++pc)
            st[(size_t) pc] = ((mask >> pc) & 1u) != 0 ? 0 : 2;
    }
    return st;
}

std::uint16_t ScaleKeyboard::activeMask() const
{
    std::uint16_t m = 0;
    const auto st = displayedStates();
    for (int pc = 0; pc < 12; ++pc)
        if (st[(size_t) pc] == 0) m = (std::uint16_t) (m | (1u << pc));
    return m;
}

void ScaleKeyboard::refresh()
{
    const auto st = displayedStates();
    if (st != lastShown || tonic() != lastTonic)
    {
        lastShown = st;
        lastTonic = tonic();
        repaint();
    }
}

void ScaleKeyboard::setNote (int pc, int stateIndex)
{
    if (auto* p = state.getParameter (aura::params::noteId (pc)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) stateIndex));
        p->endChangeGesture();
    }
}

void ScaleKeyboard::pushActivity (double inputMidi, double outputMidi) noexcept
{
    auto hit = [] (std::array<float, 12>& heat, double midi)
    {
        if (! std::isfinite (midi) || midi <= 0.0) return;
        // réparti entre les deux notes voisines : une voix à mi-chemin allume les deux
        const double fl = std::floor (midi), frac = midi - fl;
        const int pc = (((int) fl % 12) + 12) % 12;
        heat[(size_t) pc]             = jmax (heat[(size_t) pc], (float) (1.0 - frac));
        heat[(size_t) ((pc + 1) % 12)] = jmax (heat[(size_t) ((pc + 1) % 12)], (float) frac);
    };
    hit (heatIn, inputMidi);
    hit (heatOut, outputMidi);
    heatDirty = true;
}

void ScaleKeyboard::tick (double dt)
{
    const float k = (float) std::exp (-dt / 1.2);       // ≈ 3 s pour s'éteindre
    bool any = heatDirty;
    for (auto* h : { &heatIn, &heatOut })
        for (auto& v : *h)
        {
            if (v > 0.004f) any = true;
            v = v > 0.004f ? v * k : 0.0f;
        }
    heatDirty = false;
    if (any) repaint (getLocalBounds().removeFromTop ((int) kHeatStrip + 2));
}

Rectangle<float> ScaleKeyboard::keyBounds (int pc) const
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f).withTrimmedTop (kHeatStrip);
    const float ww = r.getWidth() / 7.0f;
    if (! isBlack (pc))
        return { r.getX() + ww * (float) whiteIndex[pc], r.getY(), ww, r.getHeight() };
    const float bw = ww * 0.62f;
    return { r.getX() + ww * (float) (whiteIndex[pc] + 1) - bw * 0.5f, r.getY(), bw, r.getHeight() * 0.58f };
}

int ScaleKeyboard::keyAt (Point<float> p) const
{
    for (int pc : { 1, 3, 6, 8, 10 })
        if (keyBounds (pc).contains (p)) return pc;
    for (int pc : { 0, 2, 4, 5, 7, 9, 11 })
        if (keyBounds (pc).contains (p)) return pc;
    return -1;
}

void ScaleKeyboard::paint (Graphics& g)
{
    const auto st = displayedStates();
    const int key = tonic();
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    auto drawKey = [&] (int pc)
    {
        const auto b = keyBounds (pc).reduced (1.5f, 0.0f);
        const bool black = isBlack (pc);
        const int s = st[(size_t) pc];
        const bool hover = pc == hovered;

        Colour fill = black ? Colour (0xff0d1016) : Colour (0xff1a1f2b);
        if (s == 0) fill = black ? Colour (0xff0b2a31) : Colour (0xff0f3640);
        if (hover)  fill = fill.brighter (0.12f);
        g.setColour (fill);
        g.fillRoundedRectangle (b, 4.0f);

        if (s == 0)
        {
            g.setColour (colours::cyan.withAlpha (0.18f));
            g.drawRoundedRectangle (b.expanded (1.0f), 5.0f, 2.0f);
            g.setColour (colours::cyan.withAlpha (0.9f));
            g.drawRoundedRectangle (b, 4.0f, 1.0f);
            // barre lumineuse en bas de la touche
            const auto bar = Rectangle<float> (b.getWidth() * 0.5f, 2.5f).withCentre ({ b.getCentreX(), b.getBottom() - 6.0f });
            g.setColour (colours::cyan.withAlpha (0.3f));
            g.fillRoundedRectangle (bar.expanded (2.0f), 2.0f);
            g.setColour (colours::cyan);
            g.fillRoundedRectangle (bar, 1.25f);
        }
        else if (s == 1)
        {
            Path outline;
            outline.addRoundedRectangle (b.reduced (0.5f), 4.0f);
            Path dashed;
            const float dash[] = { 3.0f, 3.0f };
            PathStrokeType (1.2f).createDashedStroke (dashed, outline, dash, 2);
            g.setColour (colours::amber);
            g.fillPath (dashed);
        }
        else
        {
            g.setColour (colours::border);
            g.drawRoundedRectangle (b, 4.0f, 1.0f);
        }

        g.setFont (font (black ? 10.0f : 11.5f, true));
        g.setColour (s == 0 ? colours::cyan : s == 1 ? colours::amber : colours::textFaint);
        const auto label = b.withTrimmedBottom (black ? 6.0f : 14.0f).removeFromBottom (14.0f);
        g.drawText (s == 1 ? String ("BYP") : String (names[pc]), label, Justification::centred);

        if (((midiMask >> pc) & 1u) != 0)          // note MIDI tenue
        {
            g.setColour (colours::amber.withAlpha (0.22f));
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (colours::amber.withAlpha (0.35f));
            g.drawRoundedRectangle (b.expanded (1.5f), 5.5f, 2.5f);
            g.setColour (colours::amber);
            g.drawRoundedRectangle (b, 4.0f, 1.2f);
        }

        if (pc == key)
        {
            g.setColour (colours::text);
            g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre ({ b.getCentreX(), b.getY() + 8.0f }));
        }
    };

    for (int pc : { 0, 2, 4, 5, 7, 9, 11 }) drawKey (pc);
    for (int pc : { 1, 3, 6, 8, 10 })       drawKey (pc);

    // Bandeau d'activité : 12 colonnes à l'aplomb des touches ; haut = entrée (ambre), bas = sortie (cyan)
    const float cellW = (getLocalBounds().toFloat().reduced (1.0f).getWidth() / 7.0f) * 0.46f;
    const float rowH = (kHeatStrip - 4.0f) * 0.5f;
    for (int pc = 0; pc < 12; ++pc)
    {
        const float cx = keyBounds (pc).getCentreX();
        const auto in  = Rectangle<float> (cx - cellW * 0.5f, 1.0f, cellW, rowH);
        const auto out = in.translated (0.0f, rowH + 1.0f);
        g.setColour (colours::panelHi.brighter (0.04f));
        g.fillRoundedRectangle (in, 1.2f);
        g.fillRoundedRectangle (out, 1.2f);
        if (heatIn[(size_t) pc] > 0.004f)
        {
            g.setColour (colours::amber.withAlpha (jmin (1.0f, 0.15f + 0.85f * heatIn[(size_t) pc])));
            g.fillRoundedRectangle (in, 1.2f);
        }
        if (heatOut[(size_t) pc] > 0.004f)
        {
            g.setColour (colours::cyan.withAlpha (jmin (1.0f, 0.15f + 0.85f * heatOut[(size_t) pc])));
            g.fillRoundedRectangle (out, 1.2f);
        }
    }
}

void ScaleKeyboard::mouseDown (const MouseEvent& e)
{
    const int pc = keyAt (e.position);
    if (pc < 0) return;

    auto* scaleParam = state.getParameter (aura::params::scale);
    const auto scale = (ScaleType) roundToInt (state.getRawParameterValue (aura::params::scale)->load());
    auto st = displayedStates();

    if (scale != ScaleType::Custom)
    {
        // Le clavier part de la gamme affichée.
        for (int n = 0; n < 12; ++n) setNote (n, st[(size_t) n]);
        scaleParam->beginChangeGesture();
        scaleParam->setValueNotifyingHost (scaleParam->convertTo0to1 ((float) ScaleType::Custom));
        scaleParam->endChangeGesture();
    }

    const bool bypassClick = e.mods.isAltDown() || e.mods.isPopupMenu();
    const int current = st[(size_t) pc];
    const int next = bypassClick ? (current == 1 ? 0 : 1) : (current == 0 ? 2 : 0);
    setNote (pc, next);
    refresh();
}

void ScaleKeyboard::mouseMove (const MouseEvent& e)
{
    const int h = keyAt (e.position);
    if (h != hovered) { hovered = h; repaint(); }
}

void ScaleKeyboard::mouseExit (const MouseEvent&)
{
    hovered = -1;
    repaint();
}
}

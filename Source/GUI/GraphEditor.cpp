// AuraPitch — NextDSP · AuraSuite
#include "GraphEditor.h"
#include "PitchDisplay.h"

namespace nextdsp
{
using namespace juce;
using aura::pitch::NoteBlob;
using aura::pitch::NoteGraphModel;

namespace
{
    constexpr float kHeader = 28.0f, kRuler = 18.0f, kKeys = 46.0f;
    constexpr float kHandle = 7.0f;
}

GraphEditor::GraphEditor (AuraPitchProcessor& p) : processor (p)
{
    setOpaque (false);
    captureButton.setClickingTogglesState (true);
    captureButton.getProperties().set ("accent", (int64) colours::red.getARGB());
    captureButton.setTooltip ("Capture: play the track in your DAW, the sung notes are recorded on its timeline (no audio is stored)");
    captureButton.onClick = [this]
    {
        processor.setGraphCapture (captureButton.getToggleState());
        if (! captureButton.getToggleState()) { syncSelection(); fitToNotes(); }
        repaint();
    };
    clearButton.setTooltip ("Remove all notes (the automatic correction applies again)");
    clearButton.onClick = [this] { processor.getGraphModel().clear(); edited(); };
    quantizeButton.setTooltip ("Move every note to the nearest note of the scale (0 cent)");
    quantizeButton.onClick = [this] { processor.getGraphModel().quantizeAll (mask); edited(); };
    followButton.setClickingTogglesState (true);
    followButton.setToggleState (true, dontSendNotification);
    followButton.setTooltip ("Follow the DAW playhead");
    zoomOut.onClick = [this] { zoomTime (1.5, viewStart + 0.5 * viewSpan); zoomPitch (1.25); };
    zoomIn.onClick  = [this] { zoomTime (1.0 / 1.5, viewStart + 0.5 * viewSpan); zoomPitch (0.8); };
    for (auto* b : { &captureButton, &clearButton, &quantizeButton, &followButton, &zoomOut, &zoomIn })
        addAndMakeVisible (b);
}

//==============================================================================================================
Rectangle<float> GraphEditor::plotArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f).withTrimmedTop (kHeader + kRuler).withTrimmedLeft (kKeys)
                                                     .withTrimmedBottom (8.0f).withTrimmedRight (10.0f);
}
float GraphEditor::rowHeight() const              { return plotArea().getHeight() / (float) semitones; }
float GraphEditor::xForTime (double t) const       { const auto a = plotArea(); return a.getX() + (float) ((t - viewStart) / viewSpan) * a.getWidth(); }
double GraphEditor::timeForX (float x) const       { const auto a = plotArea(); return viewStart + (double) ((x - a.getX()) / a.getWidth()) * viewSpan; }
float GraphEditor::yForMidi (double m) const       { const auto a = plotArea(); return a.getCentreY() - (float) (m - centre) * rowHeight(); }
double GraphEditor::midiForY (float y) const       { const auto a = plotArea(); return centre + (double) ((a.getCentreY() - y) / rowHeight()); }

void GraphEditor::showRange (double startSec, double spanSec, double centreMidi, double semis)
{
    viewStart = startSec; viewSpan = jlimit (0.25, 600.0, spanSec);
    centre = centreMidi; semitones = jlimit (6.0, 60.0, semis);
    repaint();
}

void GraphEditor::fitToNotes()
{
    const auto& b = processor.getGraphModel().getBlobs();
    if (b.empty()) return;
    double lo = 1e9, hi = -1e9, t0 = 1e300, t1 = -1e300;
    for (const auto& n : b)
    {
        lo = std::min ({ lo, n.targetPitch, n.originalCenterPitch });
        hi = std::max ({ hi, n.targetPitch, n.originalCenterPitch });
        t0 = std::min (t0, n.startTimeSec); t1 = std::max (t1, n.endTimeSec());
    }
    const double span = std::max (1.0, (t1 - t0) * 1.1);
    showRange (t0 - 0.05 * span, span, 0.5 * (lo + hi), std::max (12.0, hi - lo + 6.0));
}

void GraphEditor::resized()
{
    auto h = getLocalBounds().reduced (12, 5).removeFromTop ((int) kHeader - 4);
    h.removeFromLeft (248);                     // titre + sélecteur de vue MONITOR | GRAPH (placé par l'éditeur)
    followButton.setBounds (h.removeFromRight (58));   h.removeFromRight (5);
    quantizeButton.setBounds (h.removeFromRight (92)); h.removeFromRight (5);
    clearButton.setBounds (h.removeFromRight (50));    h.removeFromRight (5);
    captureButton.setBounds (h.removeFromRight (70));
    // zoom : en bout de règle
    const auto a = plotArea();
    const auto ruler = Rectangle<float> (a.getRight() - 44.0f, a.getY() - kRuler + 1.0f, 44.0f, kRuler - 2.0f).toNearestInt();
    zoomOut.setBounds (ruler.withWidth (21));
    zoomIn.setBounds (ruler.withTrimmedLeft (23));
}

//==============================================================================================================
void GraphEditor::tick (double)
{
    const double ph = processor.getPlayheadSeconds();
    const bool playing = processor.isTransportPlaying();
    if (followButton.getToggleState() && playing)
    {
        // la tête de lecture reste au quart de la vue
        if (ph < viewStart + 0.1 * viewSpan || ph > viewStart + 0.75 * viewSpan)
            viewStart = ph - 0.25 * viewSpan;
    }
    if (captureButton.getToggleState() != processor.isGraphCapturing())
        captureButton.setToggleState (processor.isGraphCapturing(), dontSendNotification);
    syncSelection();
    if (playing || std::abs (ph - lastPlayhead) > 1.0e-6 || processor.isGraphCapturing())
        repaint();
    lastPlayhead = ph;
}

void GraphEditor::syncSelection()
{
    selected.resize (processor.getGraphModel().getBlobs().size(), false);
}

void GraphEditor::edited()
{
    syncSelection();
    processor.graphEdited();
    repaint();
}

void GraphEditor::zoomTime (double factor, double anchor)
{
    const double newSpan = jlimit (0.25, 600.0, viewSpan * factor);
    viewStart = anchor - (anchor - viewStart) * newSpan / viewSpan;
    viewSpan = newSpan;
    repaint();
}

void GraphEditor::zoomPitch (double factor)
{
    semitones = jlimit (6.0, 60.0, semitones * factor);
    repaint();
}

//==============================================================================================================
Rectangle<float> GraphEditor::noteBounds (int i) const
{
    const auto& n = processor.getGraphModel().getBlobs()[(size_t) i];
    const float x0 = xForTime (n.startTimeSec), x1 = xForTime (n.endTimeSec());
    const float rh = rowHeight();
    return { x0, yForMidi (n.targetPitch) - 0.5f * rh, std::max (2.0f, x1 - x0), rh };
}

int GraphEditor::noteAt (Point<float> p, Part& part) const
{
    part = Part::None;
    const auto& b = processor.getGraphModel().getBlobs();
    for (int i = (int) b.size() - 1; i >= 0; --i)
    {
        const auto r = noteBounds (i).expanded (0.0f, 3.0f);
        if (! r.contains (p)) continue;
        if (p.x < r.getX() + kHandle && r.getWidth() > 3.0f * kHandle) part = Part::LeftHandle;
        else if (p.y < r.getY() + kHandle && std::abs (p.x - r.getCentreX()) < 10.0f) part = Part::TopHandle;
        else part = Part::Body;
        return i;
    }
    return -1;
}

void GraphEditor::mouseMove (const MouseEvent& e)
{
    Part part;
    const int n = noteAt (e.position, part);
    if (n != hoverNote || part != hoverPart)
    {
        hoverNote = n; hoverPart = part;
        setMouseCursor (part == Part::LeftHandle ? MouseCursor::LeftRightResizeCursor
                        : part == Part::TopHandle ? MouseCursor::UpDownResizeCursor
                        : n >= 0 ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
        repaint();
    }
}

void GraphEditor::mouseDown (const MouseEvent& e)
{
    syncSelection();
    auto& model = processor.getGraphModel();
    Part part;
    const int n = noteAt (e.position, part);
    drag = Drag::None;
    dragNote = n;
    dragStart = e.position;
    if (! plotArea().contains (e.position)) return;

    if (e.mods.isPopupMenu())
    {
        if (n >= 0 && model.split (n, timeForX (e.position.x))) edited();   // scission au curseur
        return;
    }
    if (n < 0)
    {
        std::fill (selected.begin(), selected.end(), false);
        drag = Drag::Lasso;
        lasso = { e.position, e.position };
        repaint();
        return;
    }
    if (! selected[(size_t) n])
    {
        std::fill (selected.begin(), selected.end(), false);
        selected[(size_t) n] = true;
    }
    const bool cmd = e.mods.isCommandDown() || e.mods.isCtrlDown();
    if (part == Part::LeftHandle)                  drag = Drag::Transition;
    else if (cmd && e.mods.isAltDown())            drag = Drag::Formant;
    else if (cmd || part == Part::TopHandle)       drag = Drag::Vibrato;
    else                                           drag = Drag::Pitch;

    const auto& b = model.getBlobs();
    dragOrigin.assign (b.size(), 0.0);
    for (size_t i = 0; i < b.size(); ++i)
        dragOrigin[i] = drag == Drag::Pitch ? b[i].targetPitch : drag == Drag::Vibrato ? b[i].vibratoDepth
                      : drag == Drag::Formant ? b[i].formantOffsetSemitones : b[i].transitionInRatio;
    repaint();
}

void GraphEditor::mouseDrag (const MouseEvent& e)
{
    auto& model = processor.getGraphModel();
    auto& b = model.getBlobs();
    const float dy = e.position.y - dragStart.y, dx = e.position.x - dragStart.x;
    switch (drag)
    {
        case Drag::Lasso:
        {
            lasso = Rectangle<float> (dragStart, e.position);
            for (size_t i = 0; i < b.size(); ++i)
                selected[i] = noteBounds ((int) i).intersects (lasso);
            repaint();
            return;
        }
        case Drag::Pitch:
        {
            const double delta = -dy / rowHeight();
            const bool fine = e.mods.isAltDown() || e.mods.isShiftDown();
            for (size_t i = 0; i < b.size(); ++i)
                if (selected[i])
                    b[i].targetPitch = fine ? std::round ((dragOrigin[i] + delta) * 100.0) / 100.0          // au cent
                                            : NoteGraphModel::nearestInScale (dragOrigin[i] + std::round (delta), mask);
            break;
        }
        case Drag::Vibrato:
            for (size_t i = 0; i < b.size(); ++i)
                if (selected[i]) b[i].vibratoDepth = jlimit (0.0, 2.0, dragOrigin[i] - dy / 100.0);   // 100 px = 100 %
            break;
        case Drag::Formant:
            for (size_t i = 0; i < b.size(); ++i)
                if (selected[i]) b[i].formantOffsetSemitones = jlimit (-12.0, 12.0, std::round ((dragOrigin[i] - dy / 20.0) * 10.0) / 10.0);
            break;
        case Drag::Transition:
            if (dragNote >= 0)
                b[(size_t) dragNote].transitionInRatio = jlimit (0.0, 1.0, dragOrigin[(size_t) dragNote] + dx / 80.0);
            break;
        case Drag::None: return;
    }
    edited();
}

void GraphEditor::mouseUp (const MouseEvent&)
{
    drag = Drag::None;
    lasso = {};
    repaint();
}

void GraphEditor::mouseDoubleClick (const MouseEvent& e)
{
    auto& model = processor.getGraphModel();
    Part part;
    const int n = noteAt (e.position, part);
    if (n < 0) return;
    if (e.mods.isAltDown())
    {
        if (model.split (n, timeForX (e.position.x))) edited();            // Alt + double-clic : scission
        return;
    }
    model.quantize (n, mask);                                               // double-clic : note juste, 0 c
    edited();
}

void GraphEditor::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    const bool cmd = e.mods.isCommandDown() || e.mods.isCtrlDown();
    if (cmd && e.mods.isAltDown()) { zoomPitch (w.deltaY > 0 ? 0.85 : 1.0 / 0.85); return; }
    if (cmd) { zoomTime (w.deltaY > 0 ? 0.8 : 1.25, timeForX (e.position.x)); return; }
    followButton.setToggleState (false, dontSendNotification);
    const float hx = e.mods.isShiftDown() ? w.deltaY : w.deltaX;
    const float vy = e.mods.isShiftDown() ? 0.0f : w.deltaY;
    viewStart -= (double) hx * viewSpan * 0.25;
    centre += (double) vy * semitones * 0.25;
    repaint();
}

//==============================================================================================================
void GraphEditor::paint (Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    const auto a = plotArea();
    const float rh = rowHeight();
    const auto& model = processor.getGraphModel();
    const auto& blobs = model.getBlobs();

    // titre
    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("GRAPH EDITOR", getLocalBounds().toFloat().reduced (14.0f, 8.0f).removeFromTop (16.0f), Justification::centredLeft);

    // grille piano-roll
    const int lo = (int) std::floor (centre - semitones * 0.5) - 1, hi = (int) std::ceil (centre + semitones * 0.5) + 1;
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (a.withLeft (2.0f).toNearestInt());
        for (int m = lo; m <= hi; ++m)
        {
            const float y = yForMidi (m);
            const int pc = ((m % 12) + 12) % 12;
            const bool inScale = ((mask >> pc) & 1u) != 0;
            if (inScale)
            {
                g.setColour (Colours::white.withAlpha (0.022f));
                g.fillRect (Rectangle<float> (a.getX(), y - rh * 0.5f, a.getWidth(), rh));
            }
            g.setColour (pc == key ? colours::grid.brighter (0.5f) : inScale ? colours::grid.brighter (0.2f) : colours::grid);
            g.drawHorizontalLine (roundToInt (y + rh * 0.5f), a.getX(), a.getRight());
            g.setFont (monoFont (jlimit (8.5f, 11.0f, rh * 0.7f)));
            g.setColour (inScale ? colours::textDim : colours::textFaint);
            g.drawText (midiNoteName (m), Rectangle<float> (4.0f, y - 8.0f, kKeys - 8.0f, 16.0f).toNearestInt(), Justification::centredRight);
        }
    }

    // règle : secondes, et mesures si le tempo est connu (4/4)
    {
        auto ruler = Rectangle<float> (a.getX(), a.getY() - kRuler, a.getWidth(), kRuler);
        g.setColour (colours::panelHi);
        g.fillRect (ruler);
        double step = 0.1;
        for (double s : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0 })
            if (viewSpan / s <= 12.0) { step = s; break; }
        g.setFont (monoFont (10.0f));
        for (double t = std::ceil (viewStart / step) * step; t < viewStart + viewSpan; t += step)
        {
            const float x = xForTime (t);
            g.setColour (colours::grid.brighter (0.15f));
            g.drawVerticalLine (roundToInt (x), a.getY(), a.getBottom());
            g.setColour (colours::textFaint);
            g.drawVerticalLine (roundToInt (x), ruler.getY() + 10.0f, ruler.getBottom());
            const int minutes = (int) (t / 60.0);
            const String label = minutes > 0 ? String (minutes) + ":" + String (std::fmod (t, 60.0), step < 1.0 ? 2 : 0).paddedLeft ('0', step < 1.0 ? 5 : 2)
                                             : String (t, step < 1.0 ? 2 : 0) + "s";
            if (x < ruler.getRight() - 100.0f)          // la fin de la règle porte les boutons de zoom
            {
                g.setColour (colours::textDim);
                g.drawText (label, Rectangle<float> (x + 3.0f, ruler.getY(), 60.0f, 11.0f).toNearestInt(), Justification::centredLeft);
            }
        }
        const double bpm = processor.getBpm();
        if (bpm > 20.0)
        {
            const double bar = 4.0 * 60.0 / bpm;
            if (viewSpan / bar < 40.0)
                for (double t = std::ceil (viewStart / bar) * bar; t < viewStart + viewSpan; t += bar)
                {
                    const float x = xForTime (t);
                    g.setColour (colours::violet.withAlpha (0.55f));
                    g.drawVerticalLine (roundToInt (x), ruler.getY(), ruler.getY() + 6.0f);
                    g.setColour (colours::violet.withAlpha (0.8f));
                    g.drawText (String (1 + (int) std::lround (t / bar)), Rectangle<float> (x + 2.0f, ruler.getY() + 8.0f, 30.0f, 10.0f).toNearestInt(), Justification::centredLeft);
                }
        }
    }

    Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (a.toNearestInt());

    // capture en cours : trames brutes
    if (processor.isGraphCapturing())
    {
        g.setColour (colours::red.withAlpha (0.7f));
        for (const auto& f : model.getFrames())
            if (f.voiced && f.time >= viewStart && f.time <= viewStart + viewSpan)
                g.fillRect (Rectangle<float> (xForTime (f.time) - 0.75f, yForMidi (f.midi) - 0.75f, 1.5f, 1.5f));
    }

    // notes
    for (int i = 0; i < (int) blobs.size(); ++i)
    {
        const auto& n = blobs[(size_t) i];
        if (n.endTimeSec() < viewStart || n.startTimeSec > viewStart + viewSpan) continue;
        const bool sel = i < (int) selected.size() && selected[(size_t) i];
        const float x0 = xForTime (n.startTimeSec), x1 = xForTime (n.endTimeSec());
        const float yc = yForMidi (n.targetPitch);
        const float stepPx = 3.0f;

        // capsule : épaisseur = niveau
        Path body;
        std::vector<Point<float>> lower;
        bool first = true;
        for (float x = x0; ; x += stepPx)
        {
            const float xx = std::min (x, x1);
            const double lvl = n.levelAt (timeForX (xx));
            const float half = rh * (0.18f + 0.32f * (float) jlimit (0.0, 1.0, (lvl + 54.0) / 42.0));
            if (first) body.startNewSubPath (xx, yc - half); else body.lineTo (xx, yc - half);
            first = false;
            lower.emplace_back (xx, yc + half);
            if (xx >= x1) break;
        }
        for (auto it = lower.rbegin(); it != lower.rend(); ++it) body.lineTo (*it);
        body.closeSubPath();
        g.setColour (colours::cyan.withAlpha (sel ? 0.30f : (i == hoverNote ? 0.22f : 0.15f)));
        g.fillPath (body);
        g.setColour ((sel ? colours::amber : colours::cyan).withAlpha (sel ? 0.95f : 0.55f));
        g.strokePath (body, PathStrokeType (sel ? 1.4f : 1.0f));

        // courbe chantée (pointillés ambre) et courbe rejouée (cyan lumineux)
        Path original, result;
        for (float x = x0; x <= x1; x += 2.0f)
        {
            const bool start = x <= x0;
            const double t = timeForX (x);
            const Point<float> po (x, yForMidi (n.originalCenterPitch + n.deviationAt (t) / 100.0));
            const Point<float> pr (x, yForMidi (model.resultPitchAt (i, t)));
            if (start) { original.startNewSubPath (po); result.startNewSubPath (pr); }
            else         { original.lineTo (po); result.lineTo (pr); }
        }
        Path dashed;
        const float dash[] = { 3.0f, 3.0f };
        PathStrokeType (1.2f).createDashedStroke (dashed, original, dash, 2);
        g.setColour (colours::amber.withAlpha (0.85f));
        g.fillPath (dashed);
        strokeGlow (g, result, colours::cyan, 1.8f, 1.0f);

        // poignées : gauche (glissé d'attaque), haut (vibrato)
        if (sel || i == hoverNote)
        {
            const float rw = x1 - x0;
            if (rw > 3.0f * kHandle)
            {
                g.setColour (colours::violet.withAlpha (0.9f));
                g.fillRoundedRectangle (Rectangle<float> (x0, yc - 0.5f * rh, 3.0f, rh), 1.5f);
                if (n.transitionInRatio > 0.0)
                    g.drawLine (x0, yc - 0.5f * rh, x0 + (float) n.transitionInRatio * std::min (rw, 60.0f), yc, 1.2f);
            }
            g.setColour (colours::cyan);
            g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre ({ 0.5f * (x0 + x1), yc - 0.5f * rh }));
        }

        // étiquette : note, cents, vibrato, formant
        if (x1 - x0 > 34.0f)
        {
            String label = midiNoteName ((int) std::round (n.targetPitch));
            const double cents = (n.targetPitch - std::round (n.targetPitch)) * 100.0;
            if (std::abs (cents) >= 0.5) label << (cents > 0 ? " +" : " ") << String (roundToInt (cents)) << "c";
            if (std::abs (n.vibratoDepth - 1.0) > 0.005) label << "  vib " << String (roundToInt (n.vibratoDepth * 100.0)) << "%";
            if (std::abs (n.formantOffsetSemitones) > 0.05) label << "  fmt " << (n.formantOffsetSemitones > 0 ? "+" : "") << String (n.formantOffsetSemitones, 1);
            g.setFont (font (10.0f, true));
            g.setColour (sel ? colours::amber : colours::text.withAlpha (0.85f));
            g.drawText (label, Rectangle<float> (x0 + 4.0f, yc - 0.5f * rh - 14.0f, std::max (60.0f, x1 - x0), 12.0f).toNearestInt(), Justification::centredLeft);
        }
    }

    // lasso
    if (drag == Drag::Lasso && ! lasso.isEmpty())
    {
        g.setColour (colours::cyan.withAlpha (0.08f));
        g.fillRect (lasso);
        g.setColour (colours::cyan.withAlpha (0.6f));
        g.drawRect (lasso, 1.0f);
    }

    // tête de lecture
    {
        const float x = xForTime (processor.getPlayheadSeconds());
        if (x >= a.getX() && x <= a.getRight())
        {
            Path line;
            line.startNewSubPath (x, a.getY());
            line.lineTo (x, a.getBottom());
            strokeGlow (g, line, processor.isGraphCapturing() ? colours::red : colours::text, 1.2f, 1.0f);
        }
    }

    if (blobs.empty() && ! processor.isGraphCapturing())
    {
        g.setFont (font (13.0f, true));
        g.setColour (colours::textDim);
        g.drawText ("Press CAPTURE, then play the track in your DAW", a.toNearestInt(), Justification::centred);
    }
}
}

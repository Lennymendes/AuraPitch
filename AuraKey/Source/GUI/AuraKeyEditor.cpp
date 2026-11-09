// AuraKey — NextDSP · AuraSuite
#include "AuraKeyEditor.h"

using namespace juce;
using namespace nextdsp;
using namespace aura::key;

namespace
{
    const String dot (CharPointer_UTF8 ("  \xc2\xb7  "));

    String relativeOf (int tonic, bool minor)
    {
        return minor ? String (keyName ((tonic + 3) % 12, false)) : String (keyName ((tonic + 9) % 12, true));
    }

    String signedCents (double c) { return (c >= 0.0 ? "+" : "") + String (roundToInt (c)) + " c"; }

    void drawLabel (Graphics& g, const String& text, Rectangle<float> r)
    {
        g.setFont (font (11.0f, true));
        g.setColour (colours::textDim);
        g.drawText (text, r, Justification::centredLeft);
    }

    void drawChroma (Graphics& g, Rectangle<float> r, const std::array<double, 12>& chroma, unsigned scale, Colour accent)
    {
        const float w = r.getWidth() / 12.0f;
        const auto bars = r.withTrimmedBottom (14.0f);
        double mx = 1e-12;
        for (double v : chroma) mx = jmax (mx, v);
        for (int i = 0; i < 12; ++i)
        {
            auto col = Rectangle<float> (r.getX() + (float) i * w, bars.getY(), w, bars.getHeight()).reduced (w * 0.18f, 0.0f);
            g.setColour (colours::panelHi);
            g.fillRoundedRectangle (col, 2.0f);
            const float h = (float) (chroma[(size_t) i] / mx) * col.getHeight();
            const bool inScale = ((scale >> i) & 1u) != 0;
            g.setColour ((inScale ? accent : colours::textDim).withAlpha (inScale ? 0.85f : 0.45f));
            g.fillRoundedRectangle (col.withTrimmedTop (col.getHeight() - h), 2.0f);
            g.setFont (monoFont (9.0f));
            g.setColour (inScale ? colours::text.withAlpha (0.8f) : colours::textFaint);
            g.drawText (pitchName (i), Rectangle<float> (col.getX() - 4.0f, r.getBottom() - 12.0f, col.getWidth() + 8.0f, 12.0f),
                        Justification::centred);
        }
    }
}

//==============================================================================================================
void FilePanelView::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    using FS = AuraKeyProcessor::FileState;
    const bool hasResult = analysis != nullptr && state != FS::Running;

    // contour de la zone de dépôt
    {
        Path border;
        border.addRoundedRectangle (r.reduced (1.0f), 8.0f);
        Path dashed;
        const float dashes[] = { 6.0f, 5.0f };
        PathStrokeType (1.2f).createDashedStroke (dashed, border, dashes, 2);
        if (dragHover)
        {
            g.setColour (colours::cyan.withAlpha (0.07f));
            g.fillRoundedRectangle (r.reduced (1.0f), 8.0f);
            strokeGlow (g, border, colours::cyan, 1.5f, 1.0f);
        }
        else
        {
            g.setColour (hasResult ? colours::border : colours::textFaint);
            g.fillPath (dashed);
        }
    }

    if (dragHover)
    {
        g.setFont (font (16.0f, true));
        g.setColour (colours::cyan);
        g.drawText ("Release to analyse", r, Justification::centred);
        return;
    }

    if (state == FS::Running)
    {
        auto c = r.withSizeKeepingCentre (r.getWidth() - 60.0f, 90.0f);
        g.setFont (font (14.0f, true));
        g.setColour (colours::text);
        g.drawText (pendingName, c.removeFromTop (24.0f), Justification::centred, true);
        c.removeFromTop (10.0f);
        auto bar = c.removeFromTop (8.0f);
        g.setColour (colours::panelHi);
        g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (colours::cyan);
        g.fillRoundedRectangle (bar.withWidth (jmax (8.0f, bar.getWidth() * progress)), 4.0f);
        c.removeFromTop (10.0f);
        g.setFont (font (11.5f));
        g.setColour (colours::textDim);
        g.drawText ("Decoding and analysing in the background  " + String (roundToInt (100.0f * progress)) + " %",
                    c.removeFromTop (18.0f), Justification::centred);
        return;
    }

    if (! hasResult)
    {
        auto c = r.withSizeKeepingCentre (r.getWidth() - 40.0f, 120.0f);
        // icône : flèche vers un plateau
        const auto ic = c.removeFromTop (44.0f).withSizeKeepingCentre (40.0f, 40.0f);
        Path p;
        p.startNewSubPath (ic.getCentreX(), ic.getY() + 4.0f); p.lineTo (ic.getCentreX(), ic.getY() + 26.0f);
        p.startNewSubPath (ic.getCentreX() - 9.0f, ic.getY() + 17.0f); p.lineTo (ic.getCentreX(), ic.getY() + 26.0f);
        p.lineTo (ic.getCentreX() + 9.0f, ic.getY() + 17.0f);
        p.startNewSubPath (ic.getX() + 4.0f, ic.getY() + 26.0f); p.lineTo (ic.getX() + 4.0f, ic.getBottom() - 4.0f);
        p.lineTo (ic.getRight() - 4.0f, ic.getBottom() - 4.0f); p.lineTo (ic.getRight() - 4.0f, ic.getY() + 26.0f);
        strokeGlow (g, p, colours::cyan, 1.8f, 0.6f);
        g.setFont (font (15.0f, true));
        g.setColour (colours::text);
        g.drawText ("Drop an audio file here", c.removeFromTop (26.0f), Justification::centred);
        g.setFont (font (11.5f));
        g.setColour (colours::textDim);
        g.drawText (String (CharPointer_UTF8 ("WAV \xc2\xb7 AIFF \xc2\xb7 FLAC \xc2\xb7 MP3  \xe2\x80\x94  up to DXD 384 kHz")),
                    c.removeFromTop (18.0f), Justification::centred);
        if (message.isNotEmpty())
        {
            g.setColour (state == FS::Failed ? colours::red : colours::textDim);
            g.drawText (message, c.removeFromTop (22.0f), Justification::centred, true);
        }
        return;
    }

    const auto& a = *analysis;
    const auto& res = a.result;
    auto c = r.reduced (18.0f, 14.0f);
    const auto accent = res.minor ? colours::violet : colours::cyan;

    // ligne 1 : clé | tempo
    auto top = c.removeFromTop (jmin (96.0f, c.getHeight() * 0.42f));
    auto keyCol = top.removeFromLeft (top.getWidth() * 0.58f);
    drawLabel (g, "KEY", keyCol.removeFromTop (16.0f));
    g.setFont (font (jlimit (20.0f, 30.0f, keyCol.getWidth() * 0.12f), true));
    g.setColour (colours::text);
    auto kr = keyCol.removeFromTop (keyCol.getHeight() * 0.55f);
    const String kname = keyName (res.tonic, res.minor);
    g.drawText (kname, kr, Justification::centredLeft);
    const float kw = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), kname);
    g.setFont (font (18.0f, true));
    g.setColour (accent);
    g.drawText ("(" + String (roundToInt (100.0 * res.confidence)) + "%)", kr.withTrimmedLeft (kw + 8.0f), Justification::centredLeft);
    g.setFont (monoFont (11.0f));
    g.setColour (colours::textDim);
    g.drawText ("Camelot " + String (camelot (res.tonic, res.minor)) + dot + "rel. " + relativeOf (res.tonic, res.minor), keyCol,
                Justification::topLeft, true);

    drawLabel (g, "TEMPO", top.removeFromTop (16.0f));
    auto br = top.removeFromTop (top.getHeight() * 0.55f);
    g.setFont (font (30.0f, true));
    g.setColour (colours::text);
    const String bpm = res.bpm > 0.0 ? String (res.bpm, 1) : String ("--");
    g.drawText (bpm, br, Justification::centredLeft);
    const float bw = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), bpm);
    g.setFont (font (13.0f, true));
    g.setColour (colours::cyan);
    g.drawText ("BPM", br.withTrimmedLeft (bw + 6.0f), Justification::centredLeft);
    g.setFont (monoFont (11.0f));
    g.setColour (colours::textDim);
    g.drawText ("tuning " + signedCents (res.tuningCents), top, Justification::topLeft, true);

    // chroma du fichier
    c.removeFromTop (8.0f);
    drawLabel (g, "PITCH CLASSES", c.removeFromTop (16.0f));
    auto chromaBox = c.removeFromTop (jmax (40.0f, c.getHeight() - 52.0f));
    drawChroma (g, chromaBox.reduced (0.0f, 4.0f), res.chroma, scaleMask (res.tonic, res.minor), accent);

    // caractéristiques du fichier
    c.removeFromTop (6.0f);
    g.setFont (font (12.5f, true));
    g.setColour (colours::text);
    g.drawText (a.fileName, c.removeFromTop (18.0f), Justification::centredLeft, true);
    const String bits = a.floatingPoint ? String (a.bitsPerSample) + "-bit float" : String (a.bitsPerSample) + "-bit";
    g.setFont (monoFont (10.5f));
    g.setColour (colours::textDim);
    g.drawText (a.formatName.upToFirstOccurrenceOf (" ", false, false).toUpperCase() + dot + bits + dot
                    + String (a.sampleRate / 1000.0, 1) + " kHz" + (a.sampleRate >= 352800.0 - 1.0 ? " DXD" : "") + dot
                    + String (a.numChannels) + " ch" + dot + formatTime (res.durationSec) + dot + "analysed in "
                    + String (a.analysisSeconds, 1) + " s",
                c.removeFromTop (16.0f), Justification::centredLeft, true);
}

//==============================================================================================================
AuraKeyEditor::AuraKeyEditor (AuraKeyProcessor& p)
    : AudioProcessorEditor (p), keyProcessor (p),
      memory (*p.getState().getParameter (akparams::memory), akparams::memoryLabels, "MEMORY")
{
    setLookAndFeel (&lnf);
    for (auto* c : std::initializer_list<Component*> { &circle, &sectionMap, &filePanel, &memory, &resetButton, &importButton })
        addAndMakeVisible (c);
    addChildComponent (cancelButton);
    filePanel.setInterceptsMouseClicks (false, false);

    memory.setTooltip (String (CharPointer_UTF8 ("How long the live analysis remembers: 8 s follows modulations, \xe2\x88\x9e keeps the whole song")));
    resetButton.setTooltip ("Forget the live history and start listening again");
    resetButton.onClick = [this] { keyProcessor.resetLive(); };
    importButton.setTooltip ("Choose an audio file to analyse (or drop it on the plug-in)");
    importButton.onClick = [this] { chooseFile(); };
    cancelButton.onClick = [this] { keyProcessor.cancelFileAnalysis(); refresh(); };

    circle.onPinChanged = [this] (int k) { pinned = k; sectionMap.setHighlightKey (k); };
    sectionMap.onSectionClicked = [this] (int k)
    {
        pinned = (k == pinned) ? -1 : k;
        circle.setPinned (pinned);
        sectionMap.setHighlightKey (pinned);
    };

    setResizable (true, true);
    setResizeLimits (840, 560, 1800, 1200);
    setSize (1000, 640);
    refresh();
    startTimerHz (30);
}

AuraKeyEditor::~AuraKeyEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void AuraKeyEditor::chooseFile()
{
    chooser = std::make_unique<FileChooser> ("Analyse an audio file", File(), keyProcessor.getSupportedWildcard());
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [safe = Component::SafePointer<AuraKeyEditor> (this)] (const FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) { safe->keyProcessor.analyseFile (f); safe->refresh(); }
                          });
}

bool AuraKeyEditor::isInterestedInFileDrag (const StringArray& files)
{
    for (const auto& f : files)
        if (keyProcessor.isSupportedFile (File (f)))
            return true;
    return false;
}

void AuraKeyEditor::fileDragEnter (const StringArray&, int, int) { filePanel.setDragHover (true); }
void AuraKeyEditor::fileDragExit (const StringArray&)            { filePanel.setDragHover (false); }

void AuraKeyEditor::filesDropped (const StringArray& files, int, int)
{
    filePanel.setDragHover (false);
    for (const auto& f : files)
        if (keyProcessor.isSupportedFile (File (f)))
        {
            keyProcessor.analyseFile (File (f));
            break;
        }
    refresh();
}

//==============================================================================================================
void AuraKeyEditor::refresh()
{
    LiveKeyState s;
    if (keyProcessor.getLiveState (s))
    {
        const bool changed = s.frames != live.frames || s.tonic != live.tonic || s.minor != live.minor;
        live = s;
        circle.setLive (s.posteriors, s.tonic, s.minor, s.tonic >= 0);
        if (changed) repaint (livePanel);
    }

    const auto state = keyProcessor.getFileState();
    const auto analysis = keyProcessor.getFileAnalysis();
    const float progress = keyProcessor.getFileProgress();
    const auto message = keyProcessor.getFileMessage();
    const auto pending = keyProcessor.getPendingFileName();
    if (state != filePanel.state || analysis != filePanel.analysis || std::abs (progress - filePanel.progress) > 0.002f
        || message != filePanel.message || pending != filePanel.pendingName)
    {
        filePanel.state = state;
        filePanel.analysis = analysis;
        filePanel.progress = progress;
        filePanel.message = message;
        filePanel.pendingName = pending;
        filePanel.repaint();
        cancelButton.setVisible (state == AuraKeyProcessor::FileState::Running);
    }
    if (analysis != nullptr)
    {
        circle.setFileKey (analysis->result.tonic, analysis->result.minor);
        sectionMap.setSections (analysis->result.sections, analysis->result.durationSec);
    }
    else
    {
        circle.setFileKey (-1, false);
        sectionMap.setSections ({}, 0.0);
    }

    const double rate = keyProcessor.getCurrentSampleRate();
    const String rt = rate > 0.0 ? String (rate / 1000.0, 1) + " kHz" : String ("-- kHz");
    const String bits = keyProcessor.isDoublePrecisionActive() ? "64-BIT" : "32-BIT";
    const bool isDxd = rate >= 352800.0 - 1.0;
    if (rt != rateText || bits != precisionText || isDxd != dxd)
    {
        rateText = rt; precisionText = bits; dxd = isDxd;
        repaint (headerArea);
    }
    if (sectionMap.getHoverText().isNotEmpty() || sectionMap.isMouseOver())
        repaint (sectionPanel.withHeight (30));
}

//==============================================================================================================
void AuraKeyEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    headerArea = r.removeFromTop (46);
    r.removeFromTop (8);
    sectionPanel = r.removeFromBottom (jlimit (120, 190, r.getHeight() / 4));
    r.removeFromBottom (10);
    livePanel = r.removeFromLeft (r.getWidth() * 57 / 100);
    r.removeFromLeft (10);
    filePanelArea = r;

    // LIVE : en-tête (titre, MEMORY, RESET), colonne de texte, cercle
    {
        auto a = livePanel.reduced (14, 10);
        auto head = a.removeFromTop (30);
        resetButton.setBounds (head.removeFromRight (62).withSizeKeepingCentre (62, 26));
        head.removeFromRight (8);
        memory.setBounds (head.removeFromRight (176));
        a.removeFromTop (6);
        const int circleSize = jmin (a.getHeight(), a.getWidth() - 200);
        circle.setBounds (a.removeFromRight (circleSize).withSizeKeepingCentre (circleSize, circleSize));
        a.removeFromRight (10);
        chromaArea = a.removeFromBottom (jmin (90, a.getHeight() / 3));
        liveText = a;
    }
    // FICHIER
    {
        auto a = filePanelArea.reduced (14, 10);
        auto head = a.removeFromTop (30);
        importButton.setBounds (head.removeFromRight (78).withSizeKeepingCentre (78, 26));
        head.removeFromRight (8);
        cancelButton.setBounds (head.removeFromRight (70).withSizeKeepingCentre (70, 26));
        a.removeFromTop (6);
        filePanel.setBounds (a);
    }
    // SECTIONS
    sectionMap.setBounds (sectionPanel.reduced (14, 10).withTrimmedTop (26));
}

void AuraKeyEditor::paint (Graphics& g)
{
    g.fillAll (colours::background);
    const auto glowCentre = circle.getBounds().getCentre().toFloat();
    g.setGradientFill (ColourGradient (colours::cyan.withAlpha (0.04f), glowCentre.x, glowCentre.y,
                                       Colours::transparentBlack, glowCentre.x + (float) getWidth() * 0.55f, glowCentre.y, true));
    g.fillRect (getLocalBounds());

    // en-tête : logo (cercle des quintes stylisé) et statut
    {
        auto logo = headerArea.withWidth (240).toFloat();
        const auto c = Point<float> (logo.getX() + 16.0f, logo.getCentreY());
        Path ring;
        ring.addCentredArc (c.x, c.y, 11.0f, 11.0f, 0.0f, 0.0f, MathConstants<float>::twoPi, true);
        strokeGlow (g, ring, colours::cyan, 2.0f, 1.0f);
        for (int i = 0; i < 12; ++i)
        {
            const float ang = (float) i * MathConstants<float>::twoPi / 12.0f;
            const float rr = i == 0 ? 2.4f : 1.2f;
            g.setColour (i == 0 ? colours::amber : colours::cyan.withAlpha (0.7f));
            g.fillEllipse (Rectangle<float> (2.0f * rr, 2.0f * rr).withCentre ({ c.x + 6.0f * std::sin (ang), c.y - 6.0f * std::cos (ang) }));
        }
        g.setFont (font (21.0f, true));
        g.setColour (colours::text);
        g.drawText ("AuraKey", logo.withTrimmedLeft (38.0f).removeFromTop (logo.getHeight() * 0.62f), Justification::bottomLeft);
        g.setFont (font (9.5f, true));
        g.setColour (colours::textDim);
        g.drawText (String (CharPointer_UTF8 ("NEXTDSP  \xc2\xb7  AURASUITE")), logo.withTrimmedLeft (39.0f).removeFromBottom (logo.getHeight() * 0.36f), Justification::topLeft);

        auto s = headerArea.toFloat();
        auto badge = [&] (const String& text, Colour col, bool lit)
        {
            g.setFont (font (10.5f, true));
            const float w = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 14.0f;
            auto b = s.removeFromRight (w).withSizeKeepingCentre (w, 24.0f);
            s.removeFromRight (5.0f);
            g.setColour (lit ? col.withAlpha (0.14f) : colours::panelHi);
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (lit ? col.withAlpha (0.85f) : colours::border);
            g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
            g.setColour (lit ? col : colours::textDim);
            g.drawText (text, b, Justification::centred);
        };
        badge (precisionText, colours::cyan, precisionText == "64-BIT");
        if (dxd) badge ("DXD", colours::cyan, true);
        badge (rateText, colours::text, false);
        badge ("ANALYSER " + String (CharPointer_UTF8 ("\xc2\xb7")) + " AUDIO UNCHANGED", colours::textDim, false);
    }

    drawPanel (g, livePanel.toFloat());
    drawPanel (g, filePanelArea.toFloat());
    drawPanel (g, sectionPanel.toFloat());
    drawLabel (g, "LIVE KEY", livePanel.reduced (14, 10).removeFromTop (30).toFloat());
    drawLabel (g, "FILE ANALYSIS", filePanelArea.reduced (14, 10).removeFromTop (30).toFloat());
    {
        auto head = sectionPanel.reduced (14, 8).removeFromTop (22).toFloat();
        drawLabel (g, "SECTIONS", head.removeFromLeft (80.0f));
        const auto hover = sectionMap.getHoverText();
        // légende
        auto legend = head.removeFromRight (150.0f);
        for (auto [label, col] : { std::pair<const char*, Colour> { "MAJOR", colours::cyan }, { "MINOR", colours::violet } })
        {
            auto item = legend.removeFromLeft (72.0f);
            g.setColour (col.withAlpha (0.8f));
            g.fillRoundedRectangle (item.removeFromLeft (12.0f).withSizeKeepingCentre (12.0f, 8.0f), 2.0f);
            g.setFont (font (10.0f, true));
            g.setColour (colours::textDim);
            g.drawText (label, item.withTrimmedLeft (6.0f), Justification::centredLeft);
        }
        g.setFont (monoFont (10.5f));
        g.setColour (colours::text.withAlpha (0.85f));
        if (hover.isNotEmpty())
            g.drawText (hover, head, Justification::centredLeft, true);
        else if (auto a = filePanel.analysis)
            g.drawText (String ((int) a->result.sections.size()) + (a->result.sections.size() == 1 ? " section" : " sections")
                            + dot + "click a section to pin its key on the circle", head, Justification::centredLeft, true);
    }
    paintLive (g);
}

void AuraKeyEditor::paintLive (Graphics& g)
{
    auto t = liveText.toFloat();
    const bool hasKey = live.tonic >= 0;
    const auto accent = live.minor ? colours::violet : colours::cyan;

    drawLabel (g, "DETECTED KEY", t.removeFromTop (16.0f));
    g.setFont (font (jlimit (22.0f, 34.0f, t.getWidth() * 0.15f), true));
    g.setColour (hasKey ? colours::text : colours::textFaint);
    g.drawText (hasKey ? String (keyName (live.tonic, live.minor)) : String ("Listening..."), t.removeFromTop (42.0f), Justification::centredLeft, true);

    // confiance
    auto conf = t.removeFromTop (54.0f);
    const String pct = hasKey ? String (roundToInt (100.0 * live.confidence)) + "%" : String ("--");
    g.setFont (font (44.0f, true));
    const float pw = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), pct);
    if (hasKey)
    {
        GlyphArrangement ga;
        ga.addLineOfText (g.getCurrentFont(), pct, conf.getX(), conf.getCentreY() + 15.0f);
        Path p;
        ga.createPath (p);
        g.setColour (accent.withAlpha (0.18f));
        g.strokePath (p, PathStrokeType (4.0f));
    }
    g.setColour (hasKey ? accent : colours::textFaint);
    g.drawText (pct, conf, Justification::centredLeft);
    g.setFont (font (10.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("CONFIDENCE", conf.withTrimmedLeft (pw + 10.0f), Justification::centredLeft);

    // détails
    g.setFont (monoFont (11.0f));
    auto line = [&] (const String& label, const String& value)
    {
        auto row = t.removeFromTop (18.0f);
        g.setColour (colours::textFaint);
        g.drawText (label, row.removeFromLeft (78.0f), Justification::centredLeft);
        g.setColour (colours::text.withAlpha (0.85f));
        g.drawText (value, row, Justification::centredLeft, true);
    };
    t.removeFromTop (4.0f);
    line ("Camelot", hasKey ? String (camelot (live.tonic, live.minor)) : String ("--"));
    line ("Relative", hasKey ? relativeOf (live.tonic, live.minor) : String ("--"));
    line ("Listened", String (roundToInt (live.seconds)) + " s");
    line ("Tuning", hasKey ? signedCents (live.tuningCents) + " (A " + String (440.0 * std::exp2 (live.tuningCents / 1200.0), 1) + ")" : String ("--"));
    line ("Input", live.levelDb > -120.0 ? String (live.levelDb, 1) + " dBFS" : String ("silence"));
    if (pinned >= 0)
        line ("Pinned", String (keyShortName (pinned % 12, pinned >= 12)) + "  " + String (100.0 * live.posteriors[(size_t) pinned], 1) + " % live");

    auto ch = chromaArea.toFloat();
    drawLabel (g, "PITCH CLASSES", ch.removeFromTop (16.0f));
    drawChroma (g, ch.reduced (0.0f, 2.0f), live.chroma, hasKey ? scaleMask (live.tonic, live.minor) : 0u, accent);
}

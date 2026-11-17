// AuraPitch — NextDSP · AuraSuite
#include "PluginEditor.h"

using namespace juce;
using namespace nextdsp;
namespace P = aura::params;

namespace
{
    RangedAudioParameter& param (AudioProcessorValueTreeState& s, const char* id)
    {
        auto* p = s.getParameter (id);
        jassert (p != nullptr);
        return *p;
    }
}

AuraPitchEditor::AuraPitchEditor (AuraPitchProcessor& p)
    : AudioProcessorEditor (p), pitchProcessor (p),
      graphEditor (p),
      settings (p),
      keyboard (p.getState()),
      inputType (param (p.getState(), P::inputType), { "Soprano", "Alto/Ten", "Low Male", "Instr" }, "Input Type"),
      channelMode (param (p.getState(), P::channelMode), { "Linked", "Dual" }, "Stereo"),
      trackingMode (param (p.getState(), P::trackingMode)),
      noteHold (param (p.getState(), P::transition), { "Off", "Short", "Mid", "Long" }, { 0.0f, 40.0f, 90.0f, 160.0f }, "Note Hold"),
      retune (p.getState(), P::retuneSpeed, "Retune"),
      humanize (p.getState(), P::humanize, "Humanize"),
      formant (p.getState(), P::formantShift, "Formant", colours::violet, true),
      transpose (p.getState(), P::transpose, "Transpose", colours::cyan, true),
      fine (p.getState(), P::fineTune, "Fine Tune", colours::cyan, true),
      a4 (p.getState(), P::referenceA4, "A4 Ref", colours::amber, true),
      mix (p.getState(), P::mix, "Mix"),
      flex (p.getState(), P::transition, "Flex"),
      throat (p.getState(), P::throat, "Throat", colours::violet, true),
      doubler (p.getState(), P::doubler, "Doubler", colours::amber)
{
    setLookAndFeel (&lnf);
    auto& state = p.getState();

    for (auto* c : std::initializer_list<Component*> { &graph, &core, &keyboard, &noteHold, &presetBox, &keyBox, &scaleBox,
                                                      &inputType, &trackingMode, &channelMode, &retune, &humanize,
                                                      &formant, &transpose, &fine, &a4, &mix, &preserve, &bypass,
                                                      &flex, &throat, &doubler, &autoKeyButton, &midiButton })
        addAndMakeVisible (c);

    // Vue du panneau de hauteur : MONITOR | GRAPH (au-dessus des deux vues)
    addChildComponent (graphEditor);
    addChildComponent (detachedPanel);
    graphEditor.onExpandToggle = [this] { setGraphExpanded (! isGraphExpanded()); };
    detachedPanel.showButton.onClick   = [this] { if (graphWindow != nullptr) graphWindow->toFront (true); };
    detachedPanel.returnButton.onClick = [this] { setGraphExpanded (false); };
    addChildComponent (settings);
    for (auto* b : { &viewMonitor, &viewGraph, &viewSettings })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x5b);
        b->getProperties().set ("accent", (int64) colours::cyan.getARGB());
        addAndMakeVisible (b);
    }
    viewMonitor.setToggleState (true, dontSendNotification);
    viewMonitor.onClick  = [this] { setView (View::Monitor); };
    viewGraph.onClick    = [this] { setView (View::Graph); };
    viewSettings.onClick = [this] { setView (View::Settings); };
    viewMonitor.setTooltip ("MONITOR: live pitch history, sung and corrected");
    viewGraph.setTooltip ("GRAPH EDITOR: capture the sung notes on the DAW timeline and edit them");
    viewSettings.setTooltip ("SETTINGS: Ultra mode, workflow, interface size");

    // ULTRA : accès direct depuis la barre d'onglets (le détail est dans SETTINGS)
    ultraButton.setClickingTogglesState (true);
    ultraButton.getProperties().set ("accent", (int64) colours::cyan.getARGB());
    ultraButton.setTooltip ("Ultra mode: maximum-precision engine, same latency. Engaged automatically during offline bounce.");
    ultraAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::ultraMode, ultraButton);
    addAndMakeVisible (ultraButton);

    settings.onScaleChosen = [this] (double sc) { setUiScale (sc); };

    keyBox.addItemList (P::noteNames(), 1);
    scaleBox.addItemList (P::scaleNames(), 1);
    keyAttachment   = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (state, P::key, keyBox);
    scaleAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (state, P::scale, scaleBox);

    for (int i = 0; i < pitchProcessor.getNumPrograms(); ++i)
        presetBox.addItem (pitchProcessor.getProgramName (i), i + 1);
    presetBox.setTextWhenNothingSelected ("Factory Presets");
    presetBox.setSelectedId (0, dontSendNotification);
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0) pitchProcessor.setCurrentProgram (id - 1);
    };

    preserveAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::formantPreserve, preserve);
    bypass.setClickingTogglesState (true);
    bypass.getProperties().set ("accent", (int64) colours::amber.getARGB());
    bypassAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::bypass, bypass);
    preserve.setTooltip ("Formant Preserve: keep the original vocal timbre when the pitch moves");

    autoKeyButton.getProperties().set ("accent", (int64) colours::cyan.getARGB());
    autoKeyButton.setTooltip ("Listen to the voice and detect its key; click again to stop and apply the best match");
    autoKeyButton.onClick = [this] { autoKeyClicked(); };
    midiButton.setClickingTogglesState (true);
    midiButton.getProperties().set ("accent", (int64) colours::amber.getARGB());
    midiButton.setTooltip ("MIDI Target: correct to the MIDI notes played (nearest held note, pitch bend +/-2 st)");
    midiAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::midiTarget, midiButton);
    noteHold.setTooltip ("Note Hold: how long slides and inflections pass before the note locks (sets Flex: 0 / 40 / 90 / 160 ms)");
    flex.slider.setTooltip ("Transition (Flex): slides and expressive inflections pass, then the note locks within this time");
    throat.slider.setTooltip ("Throat Length: moves F1-F3 like a longer (> 100 %) or shorter (< 100 %) vocal tract, top end untouched");
    doubler.slider.setTooltip ("Doubler Width (stereo): two extra voices, +9 ms / -7 cents left, +14 ms / +7 cents right");

    // Mise à l'échelle : proportions verrouillées, 75 … 250 %
    const double initialScale = jlimit (kMinScale, kMaxScale, pitchProcessor.editorScale);
    setResizable (true, true);
    setResizeLimits (roundToInt (kBaseWidth * kMinScale), roundToInt (kBaseHeight * kMinScale),
                     roundToInt (kBaseWidth * kMaxScale), roundToInt (kBaseHeight * kMaxScale));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kBaseWidth / (double) kBaseHeight);
    setSize (roundToInt (kBaseWidth * initialScale), roundToInt (kBaseHeight * initialScale));
    constructed = true;                 // à partir d'ici, resized() mémorise l'échelle choisie
    pitchProcessor.editorScale = uiScale;
    settings.setCurrentScale (uiScale);

    refreshStatus();
    lastTick = Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

AuraPitchEditor::~AuraPitchEditor()
{
    stopTimer();
    if (graphWindow != nullptr)
    {
        pitchProcessor.graphWindowBounds = graphWindow->getBounds();
        graphWindow.reset();
    }
    setLookAndFeel (nullptr);
}

//==============================================================================================================
void AuraPitchEditor::timerCallback()
{
    const double now = Time::getMillisecondCounterHiRes();
    const double dt = jlimit (0.0, 0.1, (now - lastTick) * 0.001);
    lastTick = now;
    update (dt);
}

void AuraPitchEditor::update (double dt)
{
    const auto status = pitchProcessor.getStatus();
    const double a4ref = pitchProcessor.getState().getRawParameterValue (P::referenceA4)->load();
    pitchProcessor.drainPitchFrames ([&] (const aura::pitch::PitchFrame& f)
    {
        graph.push (f, status.sampleRate);
        core.push (f, a4ref);
        if (f.voiced) keyboard.pushActivity (f.inputMidi, f.outputMidi);
    });
    keyboard.refresh();
    keyboard.tick (dt);
    if (status.sampleRate > 0.0)
        trackingMode.setLatencies (1000.0 * pitchProcessor.latencyFor (aura::pitch::TrackingMode::Live) / status.sampleRate,
                                   1000.0 * pitchProcessor.latencyFor (aura::pitch::TrackingMode::Studio) / status.sampleRate);
    if (view == View::Settings) settings.refresh();

    // ULTRA : ambre « BOUNCE » pendant un export hors ligne (engagé d'office), cyan quand le réglage est actif
    if (status.offlineRender != offlineShown || status.ultraActive != ultraShown)
    {
        offlineShown = status.offlineRender; ultraShown = status.ultraActive;
        ultraButton.getProperties().set ("accent", (int64) (offlineShown ? colours::amber : colours::cyan).getARGB());
        ultraButton.setButtonText (offlineShown ? "ULTRA BOUNCE" : "ULTRA");
        ultraButton.getProperties().set ("forceLit", offlineShown);
        resized();
        repaint();
    }
    graph.setScale (keyboard.activeMask(), keyboard.tonic());
    core.setScale (keyboard.activeMask(), keyboard.tonic());
    graphEditor.setScale (keyboard.activeMask(), keyboard.tonic());
    if (graphView || graphWindow != nullptr) graphEditor.tick (dt);

    // MIDI Target : notes tenues sur le clavier et la grille
    const bool midiOn = pitchProcessor.getState().getRawParameterValue (P::midiTarget)->load() > 0.5f;
    const auto held = midiOn ? pitchProcessor.getHeldMidiNotes() : std::array<std::uint64_t, 2> {};
    std::uint16_t pcMask = 0;
    for (int n = 0; n < 128; ++n)
        if (((held[(size_t) (n >> 6)] >> (n & 63)) & 1u) != 0) pcMask = (std::uint16_t) (pcMask | (1u << (n % 12)));
    keyboard.setMidiPitchClasses (pcMask);
    graph.setMidiNotes (held);
    updateAutoKey (dt);
    graph.tick (dt);
    if (view == View::Monitor) core.tick (dt);
    // Jauge PULL : correction appliquée (sortie − entrée, cents), lue sur l'Aura Core (déjà lissée)
    {
        const float pull = core.getShownOutputCents() - core.getShownInputCents();
        const float act = core.getActivity();
        if (std::abs (pull - pullShown) > 0.05f || std::abs (act - pullActivity) > 0.01f)
        {
            pullShown = pull; pullActivity = act;
            repaint (pullArea.toFloat().transformedBy (AffineTransform::scale ((float) uiScale)).getSmallestIntegerContainer().expanded (2));
        }
    }
    refreshStatus();
}

void AuraPitchEditor::setGraphView (bool showGraph) { setView (showGraph ? View::Graph : View::Monitor); }

void AuraPitchEditor::setView (View v)
{
    view = v;
    graphView = v == View::Graph;
    viewMonitor.setToggleState (v == View::Monitor, dontSendNotification);
    viewGraph.setToggleState (v == View::Graph, dontSendNotification);
    viewSettings.setToggleState (v == View::Settings, dontSendNotification);
    graph.setVisible (v == View::Monitor);
    core.setVisible (v == View::Monitor);
    graphEditor.setVisible (graphView || graphWindow != nullptr);
    detachedPanel.setVisible (graphView && graphWindow != nullptr);
    settings.setVisible (v == View::Settings);
    if (v == View::Settings) settings.refresh();
    if (graphView && graphWindow == nullptr) graphEditor.fitToNotes();
}

void AuraPitchEditor::setUiScale (double scale)
{
    scale = jlimit (kMinScale, kMaxScale, scale);
    setSize (roundToInt (kBaseWidth * scale), roundToInt (kBaseHeight * scale));      // resized() applique et mémorise
}

void AuraPitchEditor::setGraphExpanded (bool expanded)
{
    if (expanded == isGraphExpanded())
        return;
    if (expanded)
    {
        // La fermeture arrive pendant un rappel de la fenêtre elle-même : elle est détruite plus tard, hors de ce rappel.
        graphEditor.setTransform ({});              // la fenêtre a sa propre taille : pas l'échelle de l'éditeur
        graphWindow = std::make_unique<nextdsp::GraphWindow> (graphEditor, lnf, pitchProcessor.graphWindowBounds, this,
            [safe = Component::SafePointer<AuraPitchEditor> (this)]
            {
                MessageManager::callAsync ([safe] { if (safe != nullptr) safe->setGraphExpanded (false); });
            });
        setGraphView (true);
    }
    else
    {
        pitchProcessor.graphWindowBounds = graphWindow->getBounds();
        graphWindow.reset();
        graphEditor.setDetached (false);
        addChildComponent (graphEditor);
        setView (view);
        resized();
    }
}

nextdsp::Knob& AuraPitchEditor::getKnob (const String& id)
{
    if (id == P::transition) return flex;
    if (id == P::throat)     return throat;
    if (id == P::doubler)    return doubler;
    if (id == P::retuneSpeed) return retune;
    if (id == P::humanize)   return humanize;
    if (id == P::formantShift) return formant;
    if (id == P::transpose)  return transpose;
    if (id == P::fineTune)   return fine;
    if (id == P::referenceA4) return a4;
    return mix;
}

//==============================================================================================================
// Auto-Key : AUTO-KEY -> « LISTENING… » (confiance en direct) -> « B MINOR (94%) », appliqué aux paramètres Key /
// Scale dès que la confiance atteint 90 % après au moins 4 s de voix. Un second clic arrête l'écoute et applique le
// meilleur résultat s'il dépasse 60 %. 30 s sans verdict : arrêt.
void AuraPitchEditor::autoKeyClicked()
{
    if (keyState == KeyState::Listening)
    {
        finishAutoKey (true);
        return;
    }
    pitchProcessor.startKeyDetection();
    keyState = KeyState::Listening;
    keyTimer = 0.0;
    autoKeyButton.setToggleState (true, dontSendNotification);
    autoKeyButton.setButtonText (String (CharPointer_UTF8 ("LISTENING\xe2\x80\xa6")));
}

void AuraPitchEditor::finishAutoKey (bool apply)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto r = pitchProcessor.getKeyDetection();
    pitchProcessor.stopKeyDetection();
    autoKeyButton.setToggleState (false, dontSendNotification);
    keyState = KeyState::Showing;
    keyTimer = 0.0;
    if (apply && r.key >= 0 && r.confidence >= 0.6)
    {
        pitchProcessor.applyKey (r.key, r.minor);
        const String text = String (names[r.key]) + (r.minor ? " MINOR" : " MAJOR") + " (" + String (roundToInt (r.confidence * 100.0)) + "%)";
        autoKeyButton.setButtonText (text);
        autoKeyButton.setTooltip ("Detected and applied: " + text);
    }
    else
    {
        autoKeyButton.setButtonText ("NO KEY");
    }
}

void AuraPitchEditor::updateAutoKey (double dt)
{
    keyTimer += dt;
    if (keyState == KeyState::Listening)
    {
        const auto r = pitchProcessor.getKeyDetection();
        if (r.key >= 0 && r.confidence >= 0.9 && r.seconds >= 4.0)
            finishAutoKey (true);
        else if (keyTimer > 30.0)
            finishAutoKey (true);
        else
        {
            static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const String text = r.key >= 0 && r.seconds > 1.0
                ? String (names[r.key]) + (r.minor ? "m " : " ") + String (roundToInt (r.confidence * 100.0)) + String (CharPointer_UTF8 ("%\xe2\x80\xa6"))
                : String (CharPointer_UTF8 ("LISTENING\xe2\x80\xa6"));
            if (autoKeyButton.getButtonText() != text) autoKeyButton.setButtonText (text);
        }
    }
    else if (keyState == KeyState::Showing && keyTimer > 6.0)
    {
        keyState = KeyState::Idle;
        autoKeyButton.setButtonText ("AUTO-KEY");
    }
}

void AuraPitchEditor::refreshStatus()
{
    const auto s = pitchProcessor.getStatus();
    const String rate = s.sampleRate > 0.0 ? String (s.sampleRate / 1000.0, s.sampleRate >= 100000.0 ? 1 : 1) + " kHz" : String ("-- kHz");
    const String bits = s.doublePrecision ? "64-BIT" : "32-BIT";
    const String lat  = s.sampleRate > 0.0 ? String ((double) s.latencySamples / s.sampleRate * 1000.0, 1) + " ms" : String ("-- ms");
    const bool isDxd = s.sampleRate >= 352800.0 - 1.0;
    if (rate != rateText || bits != precisionText || lat != latencyText || isDxd != dxd || s.studioActive != studio)
    {
        const bool relayout = isDxd != dxd;
        rateText = rate; precisionText = bits; latencyText = lat; dxd = isDxd; studio = s.studioActive;
        if (relayout) resized();
        repaint();
    }
}

//==============================================================================================================
void AuraPitchEditor::paint (Graphics& g)
{
    g.fillAll (colours::background);
    g.addTransform (AffineTransform::scale ((float) uiScale));       // tout est dessiné à 960 × 640
    const auto base = Rectangle<int> (kBaseWidth, kBaseHeight);

    // Halo discret derrière le graphe
    const auto glowCentre = core.getBounds().getCentre().toFloat();
    g.setGradientFill (ColourGradient (colours::cyan.withAlpha (0.035f), glowCentre.x, glowCentre.y,
                                       Colours::transparentBlack, glowCentre.x + (float) kBaseWidth * 0.6f, glowCentre.y, true));
    g.fillRect (base);

    // En-tête : logo
    auto h = headerArea;
    auto logo = h.removeFromLeft (200).toFloat();
    {
        const auto mark = Rectangle<float> (26.0f, 26.0f).withCentre ({ logo.getX() + 16.0f, logo.getCentreY() });
        Path ring;
        ring.addCentredArc (mark.getCentreX(), mark.getCentreY(), 11.0f, 11.0f, 0.0f, -2.4f, 2.4f, true);
        strokeGlow (g, ring, colours::cyan, 2.2f, 1.0f);
        Path wave;
        wave.startNewSubPath (mark.getX() + 6.0f, mark.getCentreY() + 2.0f);
        wave.quadraticTo (mark.getCentreX() - 2.0f, mark.getY() + 4.0f, mark.getCentreX(), mark.getCentreY());
        wave.quadraticTo (mark.getCentreX() + 2.0f, mark.getBottom() - 4.0f, mark.getRight() - 6.0f, mark.getCentreY() - 2.0f);
        strokeGlow (g, wave, colours::amber, 1.6f, 0.6f);

        g.setFont (font (21.0f, true));
        g.setColour (colours::text);
        g.drawText ("AuraPitch", logo.withTrimmedLeft (38.0f).removeFromTop (logo.getHeight() * 0.62f), Justification::bottomLeft);
        g.setFont (font (9.5f, true));
        g.setColour (colours::textDim);
        g.drawText (String (CharPointer_UTF8 ("NEXTDSP  \xc2\xb7  AURASUITE")), logo.withTrimmedLeft (39.0f).removeFromBottom (logo.getHeight() * 0.36f), Justification::topLeft);
    }

    // Statut : fréquence (+ DXD), précision. La latence est dans le sélecteur de workflow.
    auto s = statusArea.toFloat();
    auto badge = [&] (const String& text, Colour c, bool lit)
    {
        g.setFont (font (10.5f, true));
        const float w = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 14.0f;
        auto b = s.removeFromRight (w).withSizeKeepingCentre (w, 24.0f);
        s.removeFromRight (5.0f);
        if (lit)
        {
            g.setColour (c.withAlpha (0.12f));
            g.fillRoundedRectangle (b.expanded (2.0f), 7.0f);
        }
        g.setColour (lit ? c.withAlpha (0.14f) : colours::panelHi);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (lit ? c.withAlpha (0.85f) : colours::border);
        g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (lit ? c : colours::textDim);
        g.drawText (text, b, Justification::centred);
    };
    badge (precisionText, colours::cyan, precisionText == "64-BIT");
    if (dxd) badge ("DXD", colours::cyan, true);
    badge (rateText, colours::text, false);

    // Barre d'onglets : filet, et rappel du workflow à droite
    g.setColour (colours::border);
    g.drawHorizontalLine (tabArea.getBottom() + 2, (float) tabArea.getX(), (float) tabArea.getRight());
    {
        auto hint = tabArea.withLeft (viewSettings.getRight() + 14).withRight (ultraButton.getX() - 12);
        g.setFont (font (10.5f));
        g.setColour (colours::textFaint);
        const String text = offlineShown ? String ("Offline bounce: Ultra engaged automatically, your setting is untouched")
                          : studio ? String ("Studio DXD: look-ahead correction for mixing, latency ") + latencyText + " compensated by the host"
                                   : String ("Live: ") + latencyText + " monitoring for tracking, pitch followed at the instant read";
        g.setColour (offlineShown ? colours::amber.withAlpha (0.9f) : colours::textFaint.brighter (0.25f));
        g.drawFittedText (text, hint, Justification::centredRight, 1, 0.8f);
    }

    // Panneaux
    drawPanel (g, scalePanel.toFloat());
    drawPanel (g, controlPanel.toFloat());
    // Colonne CORRECTION : panneau plus lumineux, liseré cyan (Live) ou violet (Studio DXD)
    {
        const auto mp = macroPanel.toFloat();
        drawPanel (g, mp);
        const auto accent = studio ? colours::violet : colours::cyan;
        g.setGradientFill (ColourGradient (accent.withAlpha (0.07f), mp.getCentreX(), mp.getY() + mp.getHeight() * 0.42f,
                                           Colours::transparentBlack, mp.getCentreX(), mp.getBottom(), true));
        g.fillRoundedRectangle (mp.reduced (1.0f), 8.0f);
        Path top;
        top.startNewSubPath (mp.getX() + 18.0f, mp.getY() + 0.5f);
        top.lineTo (mp.getRight() - 18.0f, mp.getY() + 0.5f);
        strokeGlow (g, top, accent.withAlpha (0.8f), 1.2f, 0.5f);
        g.setFont (font (11.0f, true));
        g.setColour (colours::textDim);
        auto t = mp.reduced (12.0f, 10.0f).removeFromTop (16.0f);
        g.drawText ("CORRECTION", t, Justification::centredLeft);
        g.setColour (accent.withAlpha (0.85f));
        g.setFont (font (9.5f, true));
        g.drawText (studio ? "STUDIO DXD" : "LIVE", t, Justification::centredRight);
    }
    // Jauge PULL : de combien la sortie est tirée vers la note (±50 cents), centre = aucune correction
    {
        auto pa = pullArea.toFloat();
        auto head = pa.removeFromTop (16.0f);
        g.setFont (font (11.0f, true));
        g.setColour (colours::textDim);
        g.drawText ("PULL", head, Justification::centredLeft);
        g.setFont (monoFont (11.0f));
        g.setColour (pullActivity > 0.05f ? colours::text.withAlpha (0.4f + 0.6f * pullActivity) : colours::textFaint);
        g.drawText (pullActivity > 0.05f ? (pullShown >= 0.0f ? "+" : "") + String (pullShown, 1) + " ct" : String ("--"), head, Justification::centredRight);
        auto bar = pa.withSizeKeepingCentre (pa.getWidth(), 10.0f).translated (0.0f, -2.0f);
        g.setColour (colours::background.withAlpha (0.7f));
        g.fillRoundedRectangle (bar, 5.0f);
        g.setColour (colours::border);
        g.drawRoundedRectangle (bar, 5.0f, 1.0f);
        const float mid = bar.getCentreX();
        const float x = mid + jlimit (-1.0f, 1.0f, pullShown / 50.0f) * (bar.getWidth() * 0.5f - 3.0f);
        if (pullActivity > 0.02f)
        {
            auto fill = Rectangle<float>::leftTopRightBottom (jmin (mid, x), bar.getY() + 2.0f, jmax (mid, x), bar.getBottom() - 2.0f);
            g.setGradientFill (ColourGradient (colours::amber.withAlpha (0.9f * pullActivity), mid, 0.0f,
                                               colours::cyan.withAlpha (0.9f * pullActivity), x, 0.0f, false));
            g.fillRoundedRectangle (fill, 3.0f);
        }
        g.setColour (colours::textDim);
        g.fillRect (Rectangle<float> (1.0f, bar.getHeight() + 6.0f).withCentre ({ mid, bar.getCentreY() }));
        g.setFont (monoFont (8.5f));
        g.setColour (colours::textFaint);
        auto scale = Rectangle<float> (bar.getX(), bar.getBottom() + 2.0f, bar.getWidth(), 12.0f);
        g.drawText ("-50", scale, Justification::centredLeft);
        g.drawText ("0", scale, Justification::centred);
        g.drawText ("+50", scale, Justification::centredRight);
    }
    // séparateurs des groupes de commandes
    g.setColour (colours::border);
    for (auto [l, r] : { std::pair<Component*, Component*> { &humanize, &formant }, { &throat, &transpose }, { &a4, &doubler } })
    {
        const float x = 0.5f * (float) (l->getRight() + r->getX());
        g.drawVerticalLine (roundToInt (x), (float) controlPanel.getY() + 18.0f, (float) controlPanel.getBottom() - 18.0f);
    }
    g.setFont (font (11.5f, true));
    g.setColour (colours::textDim);
    auto sp = scalePanel.reduced (14, 8);
    g.drawText ("KEY", sp.removeFromTop (16).removeFromLeft (keyBox.getWidth()), Justification::centredLeft);
    g.drawText ("SCALE", Rectangle<int> (scaleBox.getX(), scalePanel.getY() + 8, scaleBox.getWidth(), 16), Justification::centredLeft);
    g.drawText ("NOTES", Rectangle<int> (keyboard.getX(), scalePanel.getY() + 8, 60, 16), Justification::centredLeft);
    // légende du bandeau d'activité
    {
        auto lg = Rectangle<int> (keyboard.getRight() - 96, scalePanel.getY() + 8, 96, 16).toFloat();
        g.setFont (font (9.5f, true));
        for (auto [label, col] : { std::pair<const char*, Colour> { "OUT", colours::cyan }, { "IN", colours::amber } })
        {
            const float w = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label);
            g.setColour (colours::textDim);
            g.drawText (label, lg.removeFromRight (w + 2.0f), Justification::centredRight);
            g.setColour (col.withAlpha (0.9f));
            g.fillRoundedRectangle (lg.removeFromRight (11.0f).withSizeKeepingCentre (7.0f, 4.0f), 1.5f);
            lg.removeFromRight (8.0f);
        }
    }
}

void AuraPitchEditor::resized()
{
    // Un seul facteur pour toute l'interface : les enfants sont placés à 960 × 640 et portent la même transformation.
    uiScale = jlimit (kMinScale, kMaxScale, jmin ((double) getWidth() / kBaseWidth, (double) getHeight() / kBaseHeight));
    if (constructed) pitchProcessor.editorScale = uiScale;
    settings.setCurrentScale (uiScale);
    const auto transform = AffineTransform::scale ((float) uiScale);
    for (auto* c : getChildren())
        if (c != &graphEditor || graphWindow == nullptr)
            if (dynamic_cast<TooltipWindow*> (c) == nullptr && dynamic_cast<ResizableCornerComponent*> (c) == nullptr)
                c->setTransform (transform);

    auto r = Rectangle<int> (kBaseWidth, kBaseHeight).reduced (14, 10);

    headerArea = r.removeFromTop (46);
    {
        // logo | presets | workflow LIVE / STUDIO DXD | bypass | fréquence, DXD, précision
        auto h = headerArea;
        h.removeFromLeft (200);
        statusArea = h.removeFromRight (dxd ? 196 : 150);
        h.removeFromRight (10);
        bypass.setBounds (h.removeFromRight (70).withSizeKeepingCentre (70, 28));
        h.removeFromRight (8);
        trackingMode.setBounds (h.removeFromRight (252).withSizeKeepingCentre (252, 38));
        h.removeFromRight (12);
        const int presetW = jmin (h.getWidth(), 240);
        presetBox.setBounds (h.removeFromLeft (presetW).withSizeKeepingCentre (presetW, 28));
    }
    r.removeFromTop (6);

    // Onglets : MONITOR | GRAPH | SETTINGS ............ rappel du workflow · ULTRA
    tabArea = r.removeFromTop (24);
    {
        auto t = tabArea;
        viewMonitor.setBounds (t.removeFromLeft (78));  t.removeFromLeft (4);
        viewGraph.setBounds (t.removeFromLeft (66));    t.removeFromLeft (4);
        viewSettings.setBounds (t.removeFromLeft (82));
        const int uw = offlineShown ? 112 : 64;
        ultraButton.setBounds (t.removeFromRight (uw));
    }
    r.removeFromTop (8);

    controlPanel = r.removeFromBottom (112);
    r.removeFromBottom (10);
    scalePanel = r.removeFromBottom (96);
    r.removeFromBottom (10);

    // Scène : [AURA CORE | historique] ou GRAPH / SETTINGS, et la colonne CORRECTION toujours à droite
    stageArea = r;
    macroPanel = stageArea.removeFromRight (214);
    stageArea.removeFromRight (10);
    {
        auto viz = stageArea;
        settings.setBounds (viz);
        detachedPanel.setBounds (viz);
        if (graphWindow == nullptr)
            graphEditor.setBounds (viz);
        const int coreSize = jmin (viz.getHeight(), viz.getWidth() * 45 / 100);
        core.setBounds (viz.removeFromLeft (coreSize).withSizeKeepingCentre (coreSize, coreSize));
        viz.removeFromLeft (10);
        graph.setBounds (viz);
    }
    {
        auto m = macroPanel.reduced (12, 10);
        m.removeFromTop (18);                                            // titre CORRECTION
        noteHold.setBounds (m.removeFromBottom (44));
        m.removeFromBottom (10);
        pullArea = m.removeFromBottom (54);
        m.removeFromBottom (6);
        const int w = m.getWidth() / 2;
        const int kh = jmin (m.getHeight(), w + 44);
        auto knobs = m.withSizeKeepingCentre (m.getWidth(), kh);
        retune.setBounds (knobs.removeFromLeft (w).reduced (2, 0));
        mix.setBounds (knobs.reduced (2, 0));
    }

    // Gamme : Key, Scale, clavier, Input Type, Stereo
    {
        auto a = scalePanel.reduced (14, 8);
        a.removeFromTop (18);
        const int rowH = a.getHeight();
        keyBox.setBounds (a.removeFromLeft (64).withSizeKeepingCentre (64, jmin (30, rowH)));
        a.removeFromLeft (8);
        scaleBox.setBounds (a.removeFromLeft (118).withSizeKeepingCentre (118, jmin (30, rowH)));
        a.removeFromLeft (8);
        {
            // AUTO-KEY au-dessus, MIDI en dessous
            auto col = a.removeFromLeft (112).withY (scalePanel.getY() + 10).withBottom (scalePanel.getBottom() - 10);
            const int bh = (col.getHeight() - 6) / 2;
            autoKeyButton.setBounds (col.removeFromTop (bh));
            col.removeFromTop (6);
            midiButton.setBounds (col.removeFromTop (bh));
        }
        a.removeFromLeft (14);
        auto right = a.removeFromRight (jmax (a.getWidth() * 11 / 20, 330));
        keyboard.setBounds (a.withTrimmedRight (14).withY (scalePanel.getY() + 26).withHeight (scalePanel.getHeight() - 32));
        auto cm = right.removeFromRight (right.getWidth() * 3 / 10);
        right.removeFromRight (10);
        inputType.setBounds (right.withY (scalePanel.getY() + 8).withHeight (scalePanel.getHeight() - 16).reduced (0, 6));
        channelMode.setBounds (cm.withY (scalePanel.getY() + 8).withHeight (scalePanel.getHeight() - 16).reduced (0, 6));
    }

    // Réglages fins : 8 boutons + Preserve, en 4 groupes (tenue | timbre | hauteur | sortie)
    {
        auto a = controlPanel.reduced (10, 8);
        constexpr int n = 9, gaps = 3;
        const int gap = 12;
        const int w = (a.getWidth() - gaps * gap) / n;
        Component* order[] = { &flex, &humanize, nullptr, &formant, &preserve, &throat, nullptr,
                               &transpose, &fine, &a4, nullptr, &doubler };
        for (auto* c : order)
        {
            if (c == nullptr) { a.removeFromLeft (gap); continue; }
            auto cell = a.removeFromLeft (w).reduced (2, 0);
            if (c == &preserve)
                preserve.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 56));
            else
                c->setBounds (cell);
        }
    }
}

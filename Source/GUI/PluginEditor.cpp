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
    : AudioProcessorEditor (p), processor (p),
      keyboard (p.getState()),
      inputType (param (p.getState(), P::inputType), { "Soprano", "Alto/Ten", "Low Male", "Instr" }, "Input Type"),
      trackingMode (param (p.getState(), P::trackingMode), { "Live 2 ms", "Studio DXD" }, {}, colours::violet),
      channelMode (param (p.getState(), P::channelMode), { "Linked", "Dual" }, "Stereo"),
      retune (p.getState(), P::retuneSpeed, "Retune"),
      humanize (p.getState(), P::humanize, "Humanize"),
      formant (p.getState(), P::formantShift, "Formant", colours::violet, true),
      transpose (p.getState(), P::transpose, "Transpose", colours::cyan, true),
      fine (p.getState(), P::fineTune, "Fine Tune", colours::cyan, true),
      a4 (p.getState(), P::referenceA4, "A4 Ref", colours::amber, true),
      mix (p.getState(), P::mix, "Mix")
{
    setLookAndFeel (&lnf);
    auto& state = p.getState();

    for (auto* c : std::initializer_list<Component*> { &graph, &tuner, &keyboard, &presetBox, &keyBox, &scaleBox,
                                                      &inputType, &trackingMode, &channelMode, &retune, &humanize,
                                                      &formant, &transpose, &fine, &a4, &mix, &preserve, &bypass })
        addAndMakeVisible (c);

    keyBox.addItemList (P::noteNames(), 1);
    scaleBox.addItemList (P::scaleNames(), 1);
    keyAttachment   = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (state, P::key, keyBox);
    scaleAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (state, P::scale, scaleBox);

    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem (processor.getProgramName (i), i + 1);
    presetBox.setTextWhenNothingSelected ("Factory Presets");
    presetBox.setSelectedId (0, dontSendNotification);
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0) processor.setCurrentProgram (id - 1);
    };

    preserveAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::formantPreserve, preserve);
    bypass.setClickingTogglesState (true);
    bypass.getProperties().set ("accent", (int64) colours::amber.getARGB());
    bypassAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (state, P::bypass, bypass);
    preserve.setTooltip ("Formant Preserve: keep the original vocal timbre when the pitch moves");

    setResizable (true, true);
    setResizeLimits (900, 560, 1800, 1150);
    setSize (920, 600);

    refreshStatus();
    lastTick = Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

AuraPitchEditor::~AuraPitchEditor()
{
    stopTimer();
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
    const auto status = processor.getStatus();
    const double a4ref = processor.getState().getRawParameterValue (P::referenceA4)->load();
    processor.drainPitchFrames ([&] (const aura::pitch::PitchFrame& f)
    {
        graph.push (f, status.sampleRate);
        tuner.push (f, a4ref);
    });
    keyboard.refresh();
    graph.setScale (keyboard.activeMask(), keyboard.tonic());
    graph.tick (dt);
    tuner.tick (dt);
    refreshStatus();
}

void AuraPitchEditor::refreshStatus()
{
    const auto s = processor.getStatus();
    const String rate = s.sampleRate > 0.0 ? String (s.sampleRate / 1000.0, s.sampleRate >= 100000.0 ? 1 : 1) + " kHz" : String ("-- kHz");
    const String bits = s.doublePrecision ? "64-BIT" : "32-BIT";
    const String lat  = s.sampleRate > 0.0 ? String ((double) s.latencySamples / s.sampleRate * 1000.0, 1) + " ms" : String ("-- ms");
    const bool isDxd = s.sampleRate >= 352800.0 - 1.0;
    if (rate != rateText || bits != precisionText || lat != latencyText || isDxd != dxd || s.studioActive != studio)
    {
        rateText = rate; precisionText = bits; latencyText = lat; dxd = isDxd; studio = s.studioActive;
        repaint (headerArea);
    }
}

//==============================================================================================================
void AuraPitchEditor::paint (Graphics& g)
{
    g.fillAll (colours::background);

    // Halo discret derrière le graphe
    const auto glowCentre = graph.getBounds().getCentre().toFloat();
    g.setGradientFill (ColourGradient (colours::cyan.withAlpha (0.035f), glowCentre.x, glowCentre.y,
                                       Colours::transparentBlack, glowCentre.x + (float) getWidth() * 0.6f, glowCentre.y, true));
    g.fillRect (getLocalBounds());

    // En-tête : logo
    auto h = headerArea;
    auto logo = h.removeFromLeft (230).toFloat();
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

    // Statut : fréquence (+ DXD), précision, latence, moteur
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
    badge ("LAT " + latencyText, studio ? colours::violet : colours::textDim, studio);
    badge (precisionText, colours::cyan, precisionText == "64-BIT");
    if (dxd) badge ("DXD", colours::cyan, true);
    badge (rateText, colours::text, false);

    // Panneaux
    drawPanel (g, scalePanel.toFloat());
    drawPanel (g, controlPanel.toFloat());
    g.setFont (font (11.5f, true));
    g.setColour (colours::textDim);
    auto sp = scalePanel.reduced (14, 8);
    g.drawText ("KEY", sp.removeFromTop (16).removeFromLeft (keyBox.getWidth()), Justification::centredLeft);
    g.drawText ("SCALE", Rectangle<int> (scaleBox.getX(), scalePanel.getY() + 8, scaleBox.getWidth(), 16), Justification::centredLeft);
    g.drawText ("NOTES", Rectangle<int> (keyboard.getX(), scalePanel.getY() + 8, 200, 16), Justification::centredLeft);
}

void AuraPitchEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    const float k = jmin ((float) getWidth() / 920.0f, (float) getHeight() / 600.0f);
    auto scaled = [k] (int v) { return roundToInt ((float) v * jlimit (0.85f, 1.6f, k)); };

    headerArea = r.removeFromTop (scaled (46));
    {
        // logo | presets | Live / Studio | bypass | fréquence, DXD, précision, latence
        auto h = headerArea;
        h.removeFromLeft (212);
        statusArea = h.removeFromRight (262);
        h.removeFromRight (10);
        bypass.setBounds (h.removeFromRight (72).withSizeKeepingCentre (72, 28));
        h.removeFromRight (8);
        trackingMode.setBounds (h.removeFromRight (176).withSizeKeepingCentre (176, 30));
        h.removeFromRight (12);
        const int presetW = jmin (h.getWidth(), scaled (240));
        presetBox.setBounds (h.removeFromLeft (presetW).withSizeKeepingCentre (presetW, 28));
    }
    r.removeFromTop (8);

    controlPanel = r.removeFromBottom (scaled (150));
    r.removeFromBottom (10);
    scalePanel = r.removeFromBottom (scaled (96));
    r.removeFromBottom (10);

    // Visualisation : graphe + accordeur
    auto viz = r;
    tuner.setBounds (viz.removeFromRight (jlimit (220, 380, viz.getWidth() / 3)));
    viz.removeFromRight (10);
    graph.setBounds (viz);

    // Gamme : Key, Scale, clavier, Input Type, Tracking
    {
        auto a = scalePanel.reduced (14, 8);
        a.removeFromTop (18);
        const int rowH = a.getHeight();
        keyBox.setBounds (a.removeFromLeft (scaled (64)).withSizeKeepingCentre (scaled (64), jmin (30, rowH)));
        a.removeFromLeft (8);
        scaleBox.setBounds (a.removeFromLeft (scaled (118)).withSizeKeepingCentre (scaled (118), jmin (30, rowH)));
        a.removeFromLeft (14);
        auto right = a.removeFromRight (jmax (a.getWidth() * 3 / 5, 380));
        keyboard.setBounds (a.withTrimmedRight (14).withY (scalePanel.getY() + 26).withHeight (scalePanel.getHeight() - 34));
        auto cm = right.removeFromRight (right.getWidth() * 3 / 10);
        right.removeFromRight (10);
        inputType.setBounds (right.withY (scalePanel.getY() + 8).withHeight (scalePanel.getHeight() - 16).reduced (0, 6));
        channelMode.setBounds (cm.withY (scalePanel.getY() + 8).withHeight (scalePanel.getHeight() - 16).reduced (0, 6));
    }

    // Commandes : 7 boutons + Preserve
    {
        auto a = controlPanel.reduced (12, 10);
        const int n = 8;
        const int w = a.getWidth() / n;
        Component* order[] = { &retune, &humanize, &formant, &preserve, &transpose, &fine, &a4, &mix };
        for (int i = 0; i < n; ++i)
        {
            auto cell = a.removeFromLeft (w).reduced (4, 0);
            if (order[i] == &preserve)
                preserve.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 56));
            else
                order[i]->setBounds (cell);
        }
    }
}

// AuraPitch — NextDSP · AuraSuite
#include "SettingsPanel.h"

#include "../Parameters.h"

namespace nextdsp
{
using namespace juce;

namespace
{
    const String dot (CharPointer_UTF8 (" \xc2\xb7 "));

    void sectionTitle (Graphics& g, const String& text, Rectangle<int> area)
    {
        g.setFont (font (10.5f, true));
        g.setColour (colours::textDim);
        g.drawText (text, area.removeFromTop (14), Justification::centredLeft);
    }
}

SettingsPanel::SettingsPanel (AuraPitchProcessor& p) : processor (p)
{
    ultraAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (p.getState(), aura::params::ultraMode, ultraToggle);
    ultraToggle.setTooltip ("Ultra: 96-point sinc kernel (125 dB) and full-resolution splice search. Same latency. "
                            "Always engaged automatically while the host renders offline.");
    ultraToggle.setButtonText ({});
    ultraToggle.onStateChange = [this] { repaint(); };
    addAndMakeVisible (ultraToggle);

    for (int i = 0; i < kNumScales; ++i)
    {
        auto& b = scaleButtons[(size_t) i];
        b.setButtonText (String (roundToInt (kScales[i] * 100.0)) + "%");
        b.setClickingTogglesState (false);
        b.getProperties().set ("accent", (int64) colours::cyan.getARGB());
        b.onClick = [this, i] { if (onScaleChosen) onScaleChosen (kScales[i]); };
        addAndMakeVisible (b);
    }
    refresh();
}

void SettingsPanel::setCurrentScale (double scale)
{
    shownScale = scale;
    for (int i = 0; i < kNumScales; ++i)
        scaleButtons[(size_t) i].setToggleState (std::abs (kScales[i] - scale) < 0.02, dontSendNotification);
    repaint (interfaceArea);
}

void SettingsPanel::refresh()
{
    const auto s = processor.getStatus();
    const bool u = s.ultraActive, off = s.offlineRender;
    const String engine = off ? "ULTRA" + dot + "OFFLINE BOUNCE" : u ? String ("ULTRA") : String ("STANDARD");
    const auto ms = [&] (aura::pitch::TrackingMode m)
    {
        return s.sampleRate > 0.0 ? String (1000.0 * processor.latencyFor (m) / s.sampleRate, 1) + " ms" : String ("-- ms");
    };
    const String live = ms (aura::pitch::TrackingMode::Live), stud = ms (aura::pitch::TrackingMode::Studio);
    const String session = (s.sampleRate > 0.0 ? String (s.sampleRate / 1000.0, 1) + " kHz" : String ("-- kHz"))
                           + (s.sampleRate >= 352800.0 - 1.0 ? dot + "DXD" : String())
                           + dot + (s.doublePrecision ? "64-bit" : "32-bit") + " I/O" + dot + "64-bit engine" + dot
                           + "reported latency " + (s.sampleRate > 0.0 ? String (1000.0 * s.latencySamples / s.sampleRate, 1) + " ms" : String ("--"));
    if (engine != engineText || session != sessionText || live != liveText || stud != studioText || s.studioActive != studio)
    {
        engineText = engine; sessionText = session; liveText = live; studioText = stud;
        ultra = u; offline = off; studio = s.studioActive;
        repaint();
    }
}

void SettingsPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 10);
    r.removeFromTop (20);                                   // titre
    sessionArea = r.removeFromBottom (30);
    r.removeFromBottom (6);
    auto left = r.removeFromLeft (r.getWidth() * 54 / 100);
    r.removeFromLeft (16);
    engineArea = left;
    workflowArea = r.removeFromTop (r.getHeight() * 56 / 100);
    r.removeFromTop (8);
    interfaceArea = r;

    {
        auto a = engineArea;
        a.removeFromTop (18);
        ultraToggle.setBounds (a.removeFromTop (30).removeFromLeft (46).withSizeKeepingCentre (46, 24));   // libellé peint à droite
    }
    {
        auto a = interfaceArea;
        a.removeFromTop (18);
        auto row = a.removeFromTop (jmin (26, a.getHeight()));
        const int w = (row.getWidth() - (kNumScales - 1) * 5) / kNumScales;
        for (auto& b : scaleButtons)
        {
            b.setBounds (row.removeFromLeft (w));
            row.removeFromLeft (5);
        }
    }
}

void SettingsPanel::paint (Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    g.setFont (font (11.0f, true));
    g.setColour (colours::textDim);
    g.drawText ("SETTINGS", getLocalBounds().reduced (14, 8).removeFromTop (16), Justification::centredLeft);

    //--- ENGINE ------------------------------------------------------------------------------------------------
    sectionTitle (g, "ENGINE", engineArea);
    {
        auto a = engineArea;
        a.removeFromTop (18);
        auto head = a.removeFromTop (30);
        // libellé de l'interrupteur (à sa droite : sous la pilule il serait coupé aux petites tailles)
        g.setFont (font (12.0f, true));
        g.setColour (ultraToggle.getToggleState() ? colours::text : colours::textDim);
        g.drawText ("ULTRA MODE", head.withTrimmedLeft (56), Justification::centredLeft);
        // état réel du moteur, à droite
        const auto accent = offline ? colours::amber : ultra ? colours::cyan : colours::textDim;
        g.setFont (font (10.5f, true));
        const int w = GlyphArrangement::getStringWidthInt (g.getCurrentFont(), engineText) + 18;
        const auto badge = head.removeFromRight (w).withSizeKeepingCentre (w, 22).toFloat();
        g.setColour ((ultra || offline) ? accent.withAlpha (0.14f) : colours::panelHi);
        g.fillRoundedRectangle (badge, 6.0f);
        g.setColour ((ultra || offline) ? accent.withAlpha (0.85f) : colours::border);
        g.drawRoundedRectangle (badge.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (accent);
        g.drawText (engineText, badge, Justification::centred);

        a.removeFromTop (6);
        g.setFont (font (11.0f));
        const char* lines[] = {
            "Pushes the engine to its ceiling for DXD masters:",
            "96-point sinc kernel: aliasing floor at -125 dB",
            "splice points searched one sample at a time",
            "same latency: switching never moves the track",
        };
        for (int i = 0; i < 4; ++i)
        {
            if (a.getHeight() < 15) break;
            auto row = a.removeFromTop (15);
            if (i > 0)
            {
                g.setColour (colours::cyan.withAlpha (0.7f));
                g.fillEllipse (Rectangle<float> (3.0f, 3.0f).withCentre ({ (float) row.getX() + 4.0f, (float) row.getCentreY() }));
                row.removeFromLeft (12);
            }
            g.setColour (i == 0 ? colours::text.withAlpha (0.85f) : colours::textDim);
            g.drawFittedText (lines[i], row, Justification::centredLeft, 1, 0.8f);
        }
        a.removeFromTop (5);
        if (a.getHeight() >= 30)
        {
            // encadré : bascule automatique à l'export
            const auto box = a.removeFromTop (jmin (36, a.getHeight())).toFloat();
            g.setColour ((offline ? colours::amber : colours::cyan).withAlpha (offline ? 0.12f : 0.05f));
            g.fillRoundedRectangle (box, 6.0f);
            g.setColour ((offline ? colours::amber : colours::cyan).withAlpha (offline ? 0.8f : 0.3f));
            g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);
            auto t = box.reduced (10.0f, 3.0f);
            g.setFont (font (10.5f, true));
            g.setColour (offline ? colours::amber : colours::cyan);
            g.drawText (offline ? "OFFLINE BOUNCE IN PROGRESS" : "AUTO-BOUNCE", t.removeFromTop (t.getHeight() * 0.5f), Justification::centredLeft);
            g.setFont (font (10.5f));
            g.setColour (colours::textDim);
            g.drawFittedText (offline ? "Ultra is engaged for this render; your setting is untouched"
                                      : "Offline export always renders in Ultra, even with the switch off",
                              t.toNearestInt(), Justification::centredLeft, 1, 0.8f);
        }
    }

    //--- WORKFLOW ----------------------------------------------------------------------------------------------
    sectionTitle (g, "WORKFLOW", workflowArea);
    {
        auto a = workflowArea;
        a.removeFromTop (18);
        struct Row { const char* name; const String& latency; const char* text; Colour accent; bool on; };
        const Row rows[2] = { { "LIVE", liveText, "tracking: pitch followed at the instant read", colours::cyan, ! studio },
                              { "STUDIO DXD", studioText, "mixing: look-ahead control, formants restored", colours::violet, studio } };
        const int rh = jmin (34, a.getHeight() / 2);
        for (const auto& row : rows)
        {
            auto r = a.removeFromTop (rh).reduced (0, 2).toFloat();
            g.setColour (row.on ? row.accent.withAlpha (0.10f) : colours::panelHi);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (row.on ? row.accent.withAlpha (0.7f) : colours::border);
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            auto t = r.reduced (9.0f, 2.0f);
            auto top = t.removeFromTop (t.getHeight() * 0.52f);
            g.setFont (font (11.0f, true));
            g.setColour (row.on ? row.accent : colours::textDim);
            g.drawText (row.name, top, Justification::centredLeft);
            g.setFont (monoFont (10.5f));
            g.setColour (colours::text.withAlpha (row.on ? 0.9f : 0.6f));
            g.drawText (row.latency, top, Justification::centredRight);
            g.setFont (font (10.0f));
            g.setColour (colours::textDim);
            g.drawFittedText (row.text, t.toNearestInt(), Justification::centredLeft, 1, 0.8f);
        }
    }

    //--- INTERFACE ---------------------------------------------------------------------------------------------
    sectionTitle (g, "INTERFACE SIZE", interfaceArea);
    {
        g.setFont (monoFont (10.0f));
        g.setColour (colours::textFaint);
        g.drawText (String (roundToInt (shownScale * 100.0)) + "%", interfaceArea.withHeight (14), Justification::centredRight);
    }

    //--- SESSION -----------------------------------------------------------------------------------------------
    {
        g.setColour (colours::border);
        g.drawHorizontalLine (sessionArea.getY(), (float) sessionArea.getX(), (float) sessionArea.getRight());
        auto a = sessionArea.withTrimmedTop (4);
        g.setFont (font (10.5f, true));
        g.setColour (colours::textDim);
        g.drawText ("SESSION", a.removeFromLeft (62), Justification::centredLeft);
        g.setFont (monoFont (10.5f));
        g.setColour (colours::text.withAlpha (0.8f));
        g.drawFittedText (sessionText, a, Justification::centredLeft, 1, 0.75f);
    }
}
}

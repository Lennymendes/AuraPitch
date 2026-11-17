// AuraPitch — banc d'essai de l'étape 3 : le processeur JUCE tel qu'un hôte le voit.
// Paramètres et presets, latence annoncée (prepareToPlay, changement de mode, de fréquence), traitement 32 et 64 bits,
// double mono, bypass sans clic et compensé, sauvegarde / restauration, éditeur (rendu hors écran -> PNG).
#include "TestSupport.h"

#include "PluginProcessor.h"
#include "GUI/PluginEditor.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace test;
namespace P = aura::params;

static void setParam (AuraPitchProcessor& p, const char* id, float value)
{
    auto* param = p.getState().getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

/** Ce que fait le timer du message thread du plug-in (20 Hz). */
static void pumpMessages (AuraPitchProcessor& p) { p.syncLatency(); }

template <typename T>
static std::vector<std::vector<double>> run (AuraPitchProcessor& p, const std::vector<std::vector<double>>& in, int block,
                                             const std::vector<std::pair<size_t, juce::MidiMessage>>& events = {})
{
    const int nc = (int) in.size();
    const size_t len = in[0].size();
    juce::AudioBuffer<T> buf (nc, block);
    juce::MidiBuffer midi;
    size_t nextEvent = 0;
    std::vector<std::vector<double>> out ((size_t) nc, std::vector<double> (len));
    for (size_t pos = 0; pos < len; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, len - pos);
        buf.setSize (nc, n, false, false, true);
        for (int ch = 0; ch < nc; ++ch)
            for (int i = 0; i < n; ++i) buf.setSample (ch, i, (T) in[(size_t) ch][pos + (size_t) i]);
        midi.clear();
        while (nextEvent < events.size() && events[nextEvent].first < pos + (size_t) n)
        {
            midi.addEvent (events[nextEvent].second, (int) (events[nextEvent].first - std::min (pos, events[nextEvent].first)));
            ++nextEvent;
        }
        p.processBlock (buf, midi);
        for (int ch = 0; ch < nc; ++ch)
            for (int i = 0; i < n; ++i) out[(size_t) ch][pos + (size_t) i] = (double) buf.getSample (ch, i);
    }
    return out;
}

static std::unique_ptr<AuraPitchProcessor> makeProcessor (double fs, int channels, int block = 512)
{
    std::unique_ptr<AuraPitchProcessor> p (dynamic_cast<AuraPitchProcessor*> (createPluginFilter()));
    const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (set);
    layout.outputBuses.add (set);
    p->setBusesLayout (layout);
    p->setRateAndBufferSizeDetails (fs, block);
    p->prepareToPlay (fs, block);
    return p;
}

//==================================================================================================================
static void testParameters()
{
    std::printf ("\n[1] Paramètres et presets\n");
    auto p = makeProcessor (48000, 2);
    const auto& params = p->getParameters();
    std::printf ("  %d paramètres, %d presets d'usine :", params.size(), p->getNumPrograms());
    for (int i = 0; i < p->getNumPrograms(); ++i) std::printf (" « %s »", p->getProgramName (i).toRawUTF8());
    std::printf ("\n");
    check (params.size() == 31, "31 paramètres (19 + 12 notes)");
    check (p->getNumPrograms() == 4, "4 presets d'usine");

    for (const char* id : { P::inputType, P::key, P::scale, P::retuneSpeed, P::humanize, P::formantShift, P::formantPreserve,
                            P::transpose, P::fineTune, P::referenceA4, P::trackingMode, P::mix, P::channelMode, P::bypass })
        check (p->getState().getParameter (id) != nullptr, "identifiant de paramètre présent");

    auto* retune = p->getState().getParameter (P::retuneSpeed);
    std::printf ("  Retune Speed : 0 -> « %s », 0,5 -> « %s », 1 -> « %s » · Formant +3 -> « %s » · A4 -> « %s »\n",
                 retune->getText (0.0f, 32).toRawUTF8(), retune->getText (0.5f, 32).toRawUTF8(), retune->getText (1.0f, 32).toRawUTF8(),
                 p->getState().getParameter (P::formantShift)->getText (p->getState().getParameter (P::formantShift)->convertTo0to1 (3.0f), 32).toRawUTF8(),
                 p->getState().getParameter (P::referenceA4)->getCurrentValueAsText().toRawUTF8());
    check (std::abs (retune->convertFrom0to1 (0.5f) - 19.0f) < 0.05f, "Retune : courbe logarithmique (0,5 -> 19 ms)");

    p->setCurrentProgram (1);
    const auto r = p->getState().getRawParameterValue (P::retuneSpeed)->load();
    const auto sc = p->getState().getRawParameterValue (P::scale)->load();
    check (std::abs (r) < 1e-6f && std::abs (sc - 2.0f) < 1e-6f && p->getCurrentProgram() == 1, "preset « 0ms Hard-Tune Urban » appliqué");
    p->setCurrentProgram (3);
    check (std::abs (p->getState().getRawParameterValue (P::transpose)->load() + 12.0f) < 1e-6f
           && std::abs (p->getState().getRawParameterValue (P::mix)->load() - 50.0f) < 0.01f, "preset « Deep Octave Formant » appliqué");
}

//==================================================================================================================
static void testLatency()
{
    std::printf ("\n[2] Latence annoncée à l'hôte\n");
    for (double fs : { 44100.0, 96000.0, 352800.0, 384000.0 })
    {
        auto p = makeProcessor (fs, 2);
        const int live = p->getLatencySamples();
        setParam (*p, P::trackingMode, 1);
        auto x = makeNoise (fs, 0.05, 0.05, 3);
        run<float> (*p, { x, x }, 512);
        pumpMessages (*p);                                      // timer du message thread
        const int studio = p->getLatencySamples();
        setParam (*p, P::inputType, 2);                        // Low Male : la latence Studio ne bouge pas
        pumpMessages (*p);
        const int studioLow = p->getLatencySamples();
        setParam (*p, P::trackingMode, 0);
        pumpMessages (*p);
        std::printf ("  %5.1f kHz : Live %4d éch. (%.2f ms) -> Studio %5d éch. (%.1f ms), Low Male %5d -> retour Live %d\n",
                     fs / 1000, live, live / fs * 1000, studio, studio / fs * 1000, studioLow, p->getLatencySamples());
        check (live == p->latencyFor (aura::pitch::TrackingMode::Live) && live == (int) std::lround (0.002 * fs), "Live : 2 ms annoncées");
        check (studio == p->latencyFor (aura::pitch::TrackingMode::Studio) && studio > 0.03 * fs, "Studio : latence mise à jour par le timer");
        check (studioLow == studio, "Studio : latence indépendante de l'InputType");
        check (p->getLatencySamples() == live, "retour à la latence Live");
    }
}

//==================================================================================================================
static void testProcessing()
{
    std::printf ("\n[3] Traitement : 32 / 64 bits, hard-tune, double mono, bypass, état\n");
    const double fs = 96000;

    // Hard-tune en 32 et 64 bits, Live et Studio
    for (int mode = 0; mode < 2; ++mode)
    {
        double err[2] {};
        for (int prec = 0; prec < 2; ++prec)
        {
            auto p = makeProcessor (fs, 2);
            p->setProcessingPrecision (prec ? juce::AudioProcessor::doublePrecision : juce::AudioProcessor::singlePrecision);
            setParam (*p, P::retuneSpeed, 0); setParam (*p, P::scale, 1); setParam (*p, P::trackingMode, (float) mode);
            p->prepareToPlay (fs, 512);
            auto v = makeVowel (fs, 1.2, [] (double) { return 220.0 * std::exp2 (35.0 / 1200); });
            auto out = prec ? run<double> (*p, { v, v }, 480) : run<float> (*p, { v, v }, 480);
            auto m = measurePitch (out[1], fs, 220.0, p->getLatencySamples() / fs + 0.2, 1.15);
            err[prec] = m.meanCents;
            check (std::abs (m.meanCents) < 1.0 && m.voicedRatio > 0.95, "hard-tune A3 +35 c -> A3 (canal droit)");
            check (p->getStatus().doublePrecision == (prec == 1), "précision rapportée à l'interface");
        }
        std::printf ("  %-6s : A3 +35 c -> %+.2f c (32 bits) / %+.2f c (64 bits)\n", mode ? "Studio" : "Live", err[0], err[1]);
    }

    // Double mono : deux voix différentes, chacune corrigée sur sa propre note
    {
        auto p = makeProcessor (fs, 2);
        setParam (*p, P::retuneSpeed, 0); setParam (*p, P::channelMode, 1);
        p->prepareToPlay (fs, 512);
        auto l = makeVowel (fs, 1.2, [] (double) { return 196.0 * std::exp2 (-30.0 / 1200); });
        auto r = makeVowel (fs, 1.2, [] (double) { return 293.66 * std::exp2 (25.0 / 1200); });
        auto out = run<float> (*p, { l, r }, 512);
        auto ml = measurePitch (out[0], fs, 196.0, 0.2, 1.15), mr = measurePitch (out[1], fs, 293.66, 0.2, 1.15);
        std::printf ("  double mono : G3 −30 c -> %+.2f c (gauche), D4 +25 c -> %+.2f c (droite)\n", ml.meanCents, mr.meanCents);
        check (std::abs (ml.meanCents) < 1.0 && std::abs (mr.meanCents) < 1.0, "double mono : chaque canal corrigé sur sa note");
    }

    // Bypass : fondu de 20 ms puis sortie = entrée retardée de la latence (PDC intacte), sans saut
    for (int mode = 0; mode < 2; ++mode)
    {
        auto p = makeProcessor (fs, 2);
        setParam (*p, P::trackingMode, (float) mode); setParam (*p, P::transpose, 7);
        p->prepareToPlay (fs, 512);
        const int L = p->getLatencySamples();
        auto v = makeVowel (fs, 1.6, [] (double) { return 247.0; });
        std::vector<std::vector<double>> in { v, v }, out ((size_t) 2, std::vector<double> (v.size()));
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (size_t pos = 0; pos < v.size(); pos += 512)
        {
            if (pos >= (size_t) (0.8 * fs)) setParam (*p, P::bypass, 1);
            const int n = (int) std::min<size_t> (512, v.size() - pos);
            buf.setSize (2, n, false, false, true);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) buf.setSample (ch, i, (float) v[pos + (size_t) i]);
            p->processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) out[(size_t) ch][pos + (size_t) i] = buf.getSample (ch, i);
        }
        double e = 0, jump = 0, steadyJump = 0;
        for (size_t i = (size_t) (0.9 * fs); i < v.size(); ++i) e = std::max (e, std::abs (out[0][i] - v[i - (size_t) L]));
        for (size_t i = (size_t) (0.79 * fs); i < (size_t) (0.85 * fs); ++i) jump = std::max (jump, std::abs (out[0][i] - out[0][i - 1]));
        for (size_t i = (size_t) (0.5 * fs); i < (size_t) (0.75 * fs); ++i) steadyJump = std::max (steadyJump, std::abs (out[0][i] - out[0][i - 1]));
        for (size_t i = (size_t) (1.0 * fs); i < (size_t) (1.5 * fs); ++i) steadyJump = std::max (steadyJump, std::abs (out[0][i] - out[0][i - 1]));
        std::printf ("  bypass %-6s : sortie = entrée retardée de %d éch. à %.1e près ; pente max au fondu %.4f (régimes avant / après : %.4f)\n",
                     mode ? "Studio" : "Live", L, e, jump, steadyJump);
        check (e < 1e-6, "bypass : Dry compensé de la latence (float)");
        check (jump < 1.5 * steadyJump, "bypass sans clic");
    }

    // processBlockBypassed (bypass de l'hôte) : même compensation
    {
        auto p = makeProcessor (48000, 2);
        auto v = makeNoise (48000, 0.3, 0.1, 9);
        juce::AudioBuffer<double> buf (2, (int) v.size());
        for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < v.size(); ++i) buf.setSample (ch, (int) i, v[i]);
        juce::MidiBuffer midi;
        p->processBlockBypassed (buf, midi);
        const int L = p->getLatencySamples();
        double e = 0;
        for (size_t i = (size_t) L; i < v.size(); ++i) e = std::max (e, std::abs (buf.getSample (0, (int) i) - v[i - (size_t) L]));
        check (e < 1e-12, "processBlockBypassed : Dry retardé de la latence annoncée (64 bits)");
    }

    // Sauvegarde / restauration
    {
        auto a = makeProcessor (48000, 2);
        setParam (*a, P::key, 7); setParam (*a, P::scale, 4); setParam (*a, P::retuneSpeed, 33.3f);
        setParam (*a, P::trackingMode, 1); setParam (*a, aura::params::noteId (4).toRawUTF8(), 1);
        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto b = makeProcessor (48000, 2);
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        for (int i = 0; i < a->getParameters().size(); ++i)
            same &= std::abs (a->getParameters()[i]->getValue() - b->getParameters()[i]->getValue()) < 1e-6f;
        std::printf ("  état : %d octets, restauré à l'identique : %s\n", (int) mb.getSize(), same ? "oui" : "non");
        check (same, "sauvegarde / restauration de tous les paramètres");
    }

    // Mono -> mono
    {
        auto p = makeProcessor (48000, 1);
        setParam (*p, P::retuneSpeed, 0);
        p->prepareToPlay (48000, 512);
        auto v = makeVowel (48000, 1.0, [] (double) { return 220.0 * std::exp2 (30.0 / 1200); });
        auto out = run<float> (*p, { v }, 333);
        auto m = measurePitch (out[0], 48000, 220.0, 0.2, 0.95);
        check (std::abs (m.meanCents) < 1.0, "bus mono : corrigé (blocs de 333 échantillons)");
    }
}

//==================================================================================================================
static void testEditor()
{
    std::printf ("\n[4] Éditeur\n");
    const double fs = 384000;
    auto p = makeProcessor (fs, 2);
    p->setProcessingPrecision (juce::AudioProcessor::doublePrecision);
    setParam (*p, P::retuneSpeed, 12); setParam (*p, P::scale, 2); setParam (*p, P::key, 9); setParam (*p, P::trackingMode, 1);
    p->prepareToPlay (fs, 1024);
    pumpMessages (*p);

    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* editor = dynamic_cast<AuraPitchEditor*> (ed.get());
    std::printf ("  éditeur %d × %d\n", ed->getWidth(), ed->getHeight());
    check (editor != nullptr && ed->getWidth() == 960 && ed->getHeight() == 640, "éditeur 960 × 640 par défaut");

    // Une phrase chantée : notes légèrement fausses, vibrato, glissés.
    auto f0 = [] (double t)
    {
        const double notes[] = { 57, 60, 62, 64, 62, 60, 59, 57 };
        const int k = std::min (7, (int) (t / 0.45));
        const double base = notes[k] + 0.35 * std::sin (k * 1.7) + 0.25 * std::sin (2 * kPi * 5.5 * t) * std::min (1.0, std::fmod (t, 0.45) / 0.2);
        return 440.0 * std::exp2 ((base - 69) / 12);
    };
    auto v = makeVowel (fs, 3.6, f0);
    for (size_t pos = 0; pos < v.size(); pos += (size_t) (fs / 30))
    {
        const size_t end = std::min (v.size(), pos + (size_t) (fs / 30));
        std::vector<double> chunk (v.begin() + (long) pos, v.begin() + (long) end);
        run<double> (*p, { chunk, chunk }, 1024);
        editor->pullFramesForTest (1.0 / 30.0);
    }
    for (int i = 0; i < 3; ++i) editor->pullFramesForTest (1.0 / 60.0);

    auto save = [] (juce::Component& c, const char* name)
    {
        auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
        file.deleteFile();
        juce::FileOutputStream os (file);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::printf ("  capture : %s (%d × %d)\n", file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
        return img.isValid();
    };
    check (save (*ed, "aurapitch_editor_100.png"), "rendu de l'éditeur");
    editor->setUiScale (1.5);
    check (ed->getWidth() == 1440 && ed->getHeight() == 960 && save (*ed, "aurapitch_editor_150.png"), "rendu à 150 %");
    editor->setUiScale (0.75);
    check (ed->getWidth() == 720 && ed->getHeight() == 480 && save (*ed, "aurapitch_editor_75.png"), "rendu à la taille minimale (75 %)");
    ed.reset();
}

//==================================================================================================================
static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods, int clicks = 1)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (src, pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now, clicks, false);
}

static void testStep5A()
{
    std::printf ("\n[5] Étape 5A : paramètres, MIDI Target par l'hôte, bypass avec Throat, ergonomie de l'éditeur\n");
    {
        auto p = makeProcessor (48000, 2);
        check (p->acceptsMidi(), "le plug-in accepte le MIDI (NEEDS_MIDI_INPUT)");
        auto txt = [&] (const char* id) { auto* q = p->getState().getParameter (id); return q->getText (q->getValue(), 32); };
        std::printf ("  Transition « %s », Throat « %s », Doubler « %s », MIDI « %s »\n", txt (P::transition).toRawUTF8(),
                     txt (P::throat).toRawUTF8(), txt (P::doubler).toRawUTF8(), txt (P::midiTarget).toRawUTF8());
        check (txt (P::transition) == "0.0 ms" && txt (P::throat) == "100.0 %" && txt (P::doubler) == "0 %",
               "nouveaux paramètres : valeurs par défaut neutres (0 ms, 100 %, 0 %)");
        check (p->getParameters().size() == 31, "31 paramètres (dont Transition, Throat, Doubler, MIDI Target, Ultra Mode)");
    }

    // MIDI Target par processBlock : NoteOn C4 au milieu d'un bloc, NoteOn vélocité 0 = NoteOff
    for (double fs : { 48000.0, 384000.0 })
    {
        auto p = makeProcessor (fs, 2);
        setParam (*p, P::retuneSpeed, 0); setParam (*p, P::scale, 0); setParam (*p, P::midiTarget, 1);
        pumpMessages (*p);
        auto v = makeVowel (fs, 3.0, [] (double) { return 220.0 * std::exp2 (15.0 / 1200.0); });
        auto out = run<float> (*p, { v, v }, 512, { { (size_t) (0.5 * fs) + 77, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                                                    { (size_t) (1.8 * fs) + 3, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0) } });
        const auto held = p->getHeldMidiNotes();
        const auto c4 = measurePitch (out[0], fs, 261.6256, 0.9, 1.7);
        const auto a3 = measurePitch (out[0], fs, 220.0, 2.2, 2.9);
        std::printf ("  %5.1f k : NoteOn C4 -> %+5.2f c (max %.2f) · NoteOn vélocité 0 -> A3 %+5.2f c\n", fs / 1000, c4.meanCents, c4.maxCents, a3.meanCents);
        check (std::abs (c4.meanCents) < 1.0 && c4.voicedRatio > 0.95, "MIDI de l'hôte : A3 -> C4 exact");
        check (std::abs (a3.meanCents) < 1.0 && held[0] == 0, "NoteOn de vélocité 0 = NoteOff");
    }

    // Bypass avec Throat 112 % et Doubler 50 % : la sortie redevient le Dry retardé exact
    {
        const double fs = 48000;
        auto p = makeProcessor (fs, 2);
        setParam (*p, P::throat, 112); setParam (*p, P::doubler, 50); setParam (*p, P::trackingMode, 0);
        pumpMessages (*p);
        auto v = makeVowel (fs, 1.0, [] (double t) { return 200.0 + 30.0 * t; });
        run<double> (*p, { v, v }, 256);
        setParam (*p, P::bypass, 1);
        auto out = run<double> (*p, { v, v }, 256);
        const int lat = p->getLatencySamples();
        double worst = 0;
        for (size_t i = (size_t) (0.3 * fs); i < v.size(); ++i)
            worst = std::max (worst, std::abs (out[0][i] - v[i - (size_t) lat]));
        std::printf ("  bypass avec Throat 112 %% + Doubler 50 %% : écart au Dry retardé %.2g\n", worst);
        check (worst < 1.0e-12, "bypass : Throat et Doubler se retirent, Dry retardé exact");
    }

    // Éditeur : glissé fin, double-clic, saisie au clavier, FREEZE, AUTO-KEY
    {
        const double fs = 48000;
        auto p = makeProcessor (fs, 2);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        auto* editor = dynamic_cast<AuraPitchEditor*> (ed.get());
        auto& knob = editor->getKnob (P::throat);
        auto& s = knob.slider;
        auto* param = p->getState().getParameter (P::throat);
        const juce::Point<float> c0 = s.getLocalBounds().getCentre().toFloat();

        s.mouseDown (mouseAt (s, c0, juce::ModifierKeys::leftButtonModifier));
        s.mouseDrag (mouseAt (s, c0 + juce::Point<float> (0, -44), juce::ModifierKeys::leftButtonModifier));
        s.mouseUp (mouseAt (s, c0 + juce::Point<float> (0, -44), juce::ModifierKeys::leftButtonModifier));
        const float coarse = param->convertFrom0to1 (param->getValue());
        s.mouseDown (mouseAt (s, c0, juce::ModifierKeys::leftButtonModifier));
        s.mouseDrag (mouseAt (s, c0 + juce::Point<float> (0, -44), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier)));
        s.mouseUp (mouseAt (s, c0 + juce::Point<float> (0, -44), juce::ModifierKeys::leftButtonModifier));
        const float fine = param->convertFrom0to1 (param->getValue());
        std::printf ("  Throat : glissé de 44 px -> %.1f %% ; puis 44 px avec Maj -> %.1f %% (+%.2f, ×%.2f plus fin)\n",
                     coarse, fine, fine - coarse, (coarse - 100.0f) / (fine - coarse));
        check (std::abs (coarse - 108.0f) < 0.2f, "glissé : 220 px pour toute la course (44 px = 20 %)");
        check (std::abs ((coarse - 100.0f) / (fine - coarse) - 5.0f) < 0.3f, "glissé fin (Maj) : sensibilité ÷ 5");

        s.mouseDoubleClick (mouseAt (s, c0, juce::ModifierKeys::leftButtonModifier, 2));
        check (std::abs (param->convertFrom0to1 (param->getValue()) - 100.0f) < 1.0e-3f, "double-clic : valeur par défaut");

        // clic droit -> saisie ; « 3.5 » dans Retune
        auto& rk = editor->getKnob (P::retuneSpeed).slider;
        rk.mouseDown (mouseAt (rk, rk.getLocalBounds().getCentre().toFloat(), juce::ModifierKeys::rightButtonModifier));
        juce::Label* label = nullptr;
        for (auto* child : rk.getChildren()) if (auto* l = dynamic_cast<juce::Label*> (child)) label = l;
        const bool editing = label != nullptr && label->getCurrentTextEditor() != nullptr;
        if (editing)
        {
            label->getCurrentTextEditor()->setText ("3.5", false);
            label->hideEditor (false);
        }
        const float typed = p->getState().getRawParameterValue (P::retuneSpeed)->load();
        std::printf ("  clic droit sur Retune -> saisie %s ; « 3.5 » -> %.2f ms ; clic simple sur la valeur : %s\n",
                     editing ? "ouverte" : "absente", typed, label != nullptr && label->isEditableOnSingleClick() ? "édition" : "non");
        check (editing && std::abs (typed - 3.5f) < 1.0e-3f, "clic droit : saisie au clavier de la valeur exacte");
        check (label != nullptr && label->isEditableOnSingleClick(), "clic sur la valeur : saisie au clavier");

        auto& graph = editor->getGraph();
        auto click = [] (juce::Button& b)       // ce que fait un clic (bouton à bascule) : état puis onClick
        {
            b.setToggleState (! b.getToggleState(), juce::dontSendNotification);
            if (b.onClick) b.onClick();
        };
        click (graph.freezeButton);
        check (graph.isFrozen() && graph.freezeButton.getButtonText() == "FROZEN", "FREEZE : graphe figé");
        click (graph.freezeButton);
        check (! graph.isFrozen(), "FREEZE : reprise");

        // AUTO-KEY : mélodie en Si mineur chantée, détection puis application à Key / Scale
        editor->autoKeyClicked();
        check (editor->getAutoKeyButton().getButtonText().startsWith ("LISTENING"), "AUTO-KEY : écoute");
        const int notes[] = { 59, 62, 66, 64, 62, 61, 59, 57, 59, 66, 67, 66, 64, 62, 61, 59, 62, 66, 71, 69, 67, 66, 62, 59 };
        auto mel = makeVowel (fs, 9.0, [&] (double t) { return 440.0 * std::exp2 ((notes[std::min (23, (int) (t / 0.37))] - 69 + 0.1 * std::sin (t * 3.0)) / 12.0); });
        for (size_t pos = 0; pos < mel.size(); pos += (size_t) (fs / 30))
        {
            const size_t end = std::min (mel.size(), pos + (size_t) (fs / 30));
            std::vector<double> chunk (mel.begin() + (long) pos, mel.begin() + (long) end);
            run<float> (*p, { chunk, chunk }, 512);
            editor->pullFramesForTest (1.0 / 30.0);
        }
        const auto txtKey = editor->getAutoKeyButton().getButtonText();
        const int keyNow = (int) p->getState().getRawParameterValue (P::key)->load();
        const int scaleNow = (int) p->getState().getRawParameterValue (P::scale)->load();
        std::printf ("  AUTO-KEY -> « %s » ; Key = %s, Scale = %s\n", txtKey.toRawUTF8(), P::noteNames()[keyNow].toRawUTF8(), P::scaleNames()[scaleNow].toRawUTF8());
        check (txtKey.startsWith ("B MINOR") && keyNow == 11 && scaleNow == 2, "AUTO-KEY : Si mineur détecté et appliqué");

        // MIDI : touches éclairées
        setParam (*p, P::midiTarget, 1);
        setParam (*p, P::doubler, 35); setParam (*p, P::throat, 108); setParam (*p, P::transition, 60);
        auto v = makeVowel (fs, 1.2, [] (double t) { return 300.0 * std::exp2 (0.3 * std::sin (2 * kPi * 0.7 * t) / 12.0); });
        for (size_t pos = 0; pos < v.size(); pos += (size_t) (fs / 30))
        {
            const size_t end = std::min (v.size(), pos + (size_t) (fs / 30));
            std::vector<double> chunk (v.begin() + (long) pos, v.begin() + (long) end);
            std::vector<std::pair<size_t, juce::MidiMessage>> ev;
            if (pos == 0) ev = { { 100, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90) }, { 120, juce::MidiMessage::noteOn (1, 59, (juce::uint8) 90) } };
            run<float> (*p, { chunk, chunk }, 512, ev);
            editor->pullFramesForTest (1.0 / 30.0);
        }
        check (((p->getHeldMidiNotes()[1] >> (64 - 64)) & 1u) != 0, "MIDI : note tenue visible par l'interface (E4)");
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aurapitch_editor_5a.png");
            file.deleteFile();
            juce::FileOutputStream os (file);
            juce::PNGImageFormat().writeImageToStream (img, os);
            std::printf ("  capture : %s\n", file.getFullPathName().toRawUTF8());
        }
        ed.reset();
    }
}

//==================================================================================================================
static void testLot2()
{
    std::printf ("\n[6] Lot 2 : mode Ultra, export hors ligne, mise à l'échelle, onglets, workflow\n");
    const double fs = 48000;
    auto voice = makeVowel (fs, 2.0, [] (double t) { return 220.0 * std::exp2 ((35.0 + 20.0 * std::sin (2 * kPi * 5.5 * t)) / 1200.0); });

    // 1. Paramètre Ultra : présent, éteint par défaut, non automatisable, épargné par les presets d'usine
    {
        auto p = makeProcessor (fs, 2);
        auto* ultra = p->getState().getParameter (P::ultraMode);
        check (ultra != nullptr && ultra->getValue() < 0.5f && ! ultra->isAutomatable(), "Ultra Mode : éteint par défaut, non automatisable");
        setParam (*p, P::ultraMode, 1);
        p->setCurrentProgram (1);
        check (ultra->getValue() > 0.5f, "un preset d'usine ne touche pas au mode Ultra");
    }

    // 2. Export hors ligne : Ultra engagé d'office, réglage intact, latence identique, retour à l'état initial
    for (int mode = 0; mode < 2; ++mode)
    {
        auto p = makeProcessor (fs, 2);
        setParam (*p, P::trackingMode, (float) mode);
        setParam (*p, P::retuneSpeed, 0);
        p->prepareToPlay (fs, 512);
        pumpMessages (*p);
        const int latency = p->getLatencySamples();
        const size_t third = voice.size() / 3;
        auto part = [&] (size_t k) { return std::vector<double> (voice.begin() + (long) (k * third), voice.begin() + (long) ((k + 1) * third)); };

        auto y0 = run<double> (*p, { part (0), part (0) }, 512);
        const auto s0 = p->getStatus();
        p->setNonRealtime (true);                               // l'hôte lance un bounce
        auto y1 = run<double> (*p, { part (1), part (1) }, 512);
        pumpMessages (*p);
        const auto s1 = p->getStatus();
        const bool paramUntouched = p->getState().getRawParameterValue (P::ultraMode)->load() < 0.5f;
        const int latencyOffline = p->getLatencySamples();
        p->setNonRealtime (false);                              // bounce terminé
        auto y2 = run<double> (*p, { part (2), part (2) }, 512);
        const auto s2 = p->getStatus();
        std::printf ("  %s : temps réel %s -> bounce %s%s -> temps réel %s · latence %d / %d éch. · réglage %s\n",
                     mode ? "Studio" : "Live  ", s0.ultraActive ? "ULTRA" : "standard", s1.ultraActive ? "ULTRA" : "standard",
                     s1.offlineRender ? " (hors ligne)" : "", s2.ultraActive ? "ULTRA" : "standard", latency, latencyOffline,
                     paramUntouched ? "intact" : "MODIFIÉ");
        check (! s0.ultraActive && s1.ultraActive && s1.offlineRender, "bounce hors ligne : Ultra engagé d'office");
        check (paramUntouched && ! s2.ultraActive && ! s2.offlineRender, "fin du bounce : retour à l'état initial, réglage intact");
        check (latency == latencyOffline, "latence identique pendant le bounce (export aligné)");

        // aucune discontinuité aux deux bascules : énergie > 6 kHz d'une voyelle sombre, comparée à un passage sans bascule
        std::vector<double> y (y0[0]);
        y.insert (y.end(), y1[0].begin(), y1[0].end());
        y.insert (y.end(), y2[0].begin(), y2[0].end());
        auto q = makeProcessor (fs, 2);
        setParam (*q, P::trackingMode, (float) mode);
        setParam (*q, P::retuneSpeed, 0);
        q->prepareToPlay (fs, 512);
        pumpMessages (*q);
        std::vector<double> whole (voice.begin(), voice.begin() + (long) (3 * third));
        const auto ref = run<double> (*q, { whole, whole }, 512)[0];
        const double L = latency / fs;
        const double with = maxDb (highBandRms (y, fs), fs, 0.3 + L, 1.9), without = maxDb (highBandRms (ref, fs), fs, 0.3 + L, 1.9);
        std::printf ("           aigus > 6 kHz : %.1f dB avec les deux bascules, %.1f dB sans\n", with, without);
        check (with < without + 3.0, "bascule vers Ultra et retour : aucun clic");

        // le rendu hors ligne est celui du mode Ultra choisi à la main, au bit près
        auto a = makeProcessor (fs, 2), b = makeProcessor (fs, 2);
        for (auto* x : { a.get(), b.get() }) { setParam (*x, P::trackingMode, (float) mode); setParam (*x, P::retuneSpeed, 0); }
        setParam (*a, P::ultraMode, 1);
        a->prepareToPlay (fs, 512); b->prepareToPlay (fs, 512);
        b->setNonRealtime (true);
        check (run<double> (*a, { whole, whole }, 512) == run<double> (*b, { whole, whole }, 512),
               "bounce = rendu du mode Ultra activé à la main, au bit près");
    }

    // 3. Interface
    {
        auto p = makeProcessor (fs, 2);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
        auto* editor = dynamic_cast<AuraPitchEditor*> (ed.get());
        check (editor != nullptr, "éditeur");
        if (editor == nullptr) return;

        // onglet SETTINGS
        editor->getSettingsTab().onClick();
        check (editor->getView() == AuraPitchEditor::View::Settings && editor->getSettingsPanel().isVisible() && ! editor->getGraph().isVisible(),
               "onglet SETTINGS : panneau des réglages affiché");
        editor->pullFramesForTest (1.0 / 60.0);
        check (editor->getSettingsPanel().engineStateText() == "STANDARD", "SETTINGS : moteur STANDARD");

        // interrupteur Ultra (onglet) et bouton ULTRA (barre) pilotent le même paramètre
        editor->getSettingsPanel().getUltraToggle().setToggleState (true, juce::sendNotificationSync);
        check (p->getState().getRawParameterValue (P::ultraMode)->load() > 0.5f && editor->getUltraButton().getToggleState(),
               "interrupteur ULTRA MODE : paramètre et bouton de la barre à jour");
        editor->pullFramesForTest (1.0 / 60.0);
        check (editor->getSettingsPanel().engineStateText() == "ULTRA", "SETTINGS : moteur ULTRA");
        auto save = [&] (const char* name)
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream os (file);
            juce::PNGImageFormat().writeImageToStream (img, os);
            std::printf ("  capture : %s\n", file.getFullPathName().toRawUTF8());
        };
        save ("aurapitch_settings.png");
        editor->getUltraButton().setToggleState (false, juce::sendNotificationSync);
        check (p->getState().getRawParameterValue (P::ultraMode)->load() < 0.5f, "bouton ULTRA : paramètre éteint");

        // bounce : le bouton passe en « ULTRA BOUNCE » sans que le paramètre change
        p->setNonRealtime (true);
        run<double> (*p, { std::vector<double> (2048, 0.0), std::vector<double> (2048, 0.0) }, 512);
        editor->pullFramesForTest (1.0 / 60.0);
        std::printf ("  pendant un bounce : bouton « %s », moteur « %s »\n", editor->getUltraButton().getButtonText().toRawUTF8(),
                     editor->getSettingsPanel().engineStateText().toRawUTF8());
        check (editor->getUltraButton().getButtonText() == "ULTRA BOUNCE" && ! editor->getUltraButton().getToggleState()
               && editor->getSettingsPanel().engineStateText().contains ("OFFLINE BOUNCE"), "bounce : état affiché, réglage intact");
        save ("aurapitch_settings_bounce.png");
        p->setNonRealtime (false);
        run<double> (*p, { std::vector<double> (2048, 0.0), std::vector<double> (2048, 0.0) }, 512);
        editor->pullFramesForTest (1.0 / 60.0);
        check (editor->getUltraButton().getButtonText() == "ULTRA" && editor->getSettingsPanel().engineStateText() == "STANDARD",
               "fin du bounce : affichage revenu à l'état initial");

        // mise à l'échelle : boutons de taille, proportions, enfants mis à l'échelle, zones cliquables
        editor->getSettingsPanel().getScaleButton (3).onClick();          // 150 %
        const auto t = editor->getWorkflowSwitch().getTransform();
        check (ed->getWidth() == 1440 && ed->getHeight() == 960 && std::abs (editor->getUiScale() - 1.5) < 1e-9
               && std::abs (t.mat00 - 1.5f) < 1e-6f, "taille 150 % : 1440 × 960, interface entière mise à l'échelle");
        check (editor->getWorkflowSwitch().getWidth() == 252
               && std::abs (editor->getWorkflowSwitch().getBoundsInParent().getWidth() - 378) <= 1, "les commandes gardent leur taille de dessin, agrandies à l'écran");
        ed->setSize (1200, 640);                                           // un hôte impose une taille hors proportions
        check (std::abs (editor->getUiScale() - 1.0) < 1e-9, "taille hors proportions : échelle = la plus petite des deux");
        editor->setUiScale (1.25);

        // workflow : clic sur STUDIO DXD puis LIVE
        auto& wf = editor->getWorkflowSwitch();
        wf.mouseDown (mouseAt (wf, wf.cellBounds (1).getCentre(), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
        check ((int) p->getState().getRawParameterValue (P::trackingMode)->load() == 1 && wf.getSelectedIndex() == 1, "workflow : STUDIO DXD");
        wf.mouseDown (mouseAt (wf, wf.cellBounds (0).getCentre(), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
        check ((int) p->getState().getRawParameterValue (P::trackingMode)->load() == 0 && wf.getSelectedIndex() == 0, "workflow : LIVE");

        // clavier : bandeau d'activité (A3 chanté 35 c trop haut, corrigé sur A)
        editor->getSettingsTab().getProperties();
        setParam (*p, P::retuneSpeed, 0);
        run<double> (*p, { voice, voice }, 512);
        editor->pullFramesForTest (1.0 / 60.0);
        auto& kb = editor->getKeyboard();
        std::printf ("  clavier : activité A entrée %.2f / sortie %.2f · A# entrée %.2f / sortie %.2f\n", kb.inputActivity (9),
                     kb.outputActivity (9), kb.inputActivity (10), kb.outputActivity (10));
        check (kb.inputActivity (9) > 0.3f && kb.inputActivity (10) > 0.2f && kb.outputActivity (9) > 0.9f && kb.outputActivity (10) < 0.1f,
               "bandeau d'activité : entrée entre A et A#, sortie sur A");

        // AURA CORE (vue MONITOR) : la même voix -> note visée A, repère d'entrée à ≈ +35 c, sortie verrouillée
        editor->setView (AuraPitchEditor::View::Monitor);
        double inSum = 0.0; int inCount = 0;
        bool lockedSeen = false;
        const size_t slice = (size_t) (0.1 * fs);                         // l'hôte joue, l'interface suit (≈ 100 ms)
        for (size_t pos = 0; pos + slice <= voice.size(); pos += slice)
        {
            std::vector<double> part (voice.begin() + (long) pos, voice.begin() + (long) (pos + slice));
            run<double> (*p, { part, part }, 512);
            for (int i = 0; i < 6; ++i) editor->pullFramesForTest (1.0 / 60.0);
            if (pos > voice.size() / 3)                                    // après l'attaque
            {
                inSum += editor->getCore().getShownInputCents(); ++inCount;
                lockedSeen = lockedSeen || editor->getCore().isLocked();
            }
        }
        auto& core = editor->getCore();
        const float meanIn = inCount > 0 ? (float) (inSum / inCount) : 0.0f;
        std::printf ("  AURA CORE : note %d, entrée moyenne %+.1f c, sortie %+.1f c, %s\n", core.getTargetPitchClass(),
                     meanIn, core.getShownOutputCents(), core.isLocked() ? "LOCKED" : "non verrouillée");
        check (core.isVisible() && core.getTargetPitchClass() == 9 && std::abs (meanIn - 35.0f) < 6.0f && core.isLocked() && lockedSeen,
               "AURA CORE : A visé, entrée à +35 c (moyenne), sortie verrouillée (±3 c)");

        // NOTE HOLD : raccourcis vers Flex 0 / 40 / 90 / 160 ms ; réglé autrement, aucun segment allumé
        auto& hold = editor->getNoteHold();
        hold.choose (2);
        const float flexMid = p->getState().getRawParameterValue (P::transition)->load();
        hold.choose (0);
        const float flexOff = p->getState().getRawParameterValue (P::transition)->load();
        setParam (*p, P::transition, 55.0f);
        const int none = hold.getSelectedIndex();
        setParam (*p, P::transition, 160.0f);
        std::printf ("  NOTE HOLD : MID -> Flex %.1f ms, OFF -> %.1f ms, Flex 55 ms -> segment %d, 160 ms -> segment %d\n",
                     flexMid, flexOff, none, hold.getSelectedIndex());
        check (std::abs (flexMid - 90.0f) < 0.05f && flexOff < 0.05f && none == -1 && hold.getSelectedIndex() == 3,
               "NOTE HOLD : MID = 90 ms, OFF = 0, valeur libre = aucun segment, 160 ms = LONG");
        setParam (*p, P::transition, 0.0f);

        // l'échelle est sauvegardée avec le projet
        juce::MemoryBlock state;
        p->getStateInformation (state);
        ed.reset();
        auto q = makeProcessor (fs, 2);
        q->setStateInformation (state.getData(), (int) state.getSize());
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (q->createEditor());
        std::printf ("  projet rechargé : éditeur %d × %d (échelle %.3f)\n", ed2->getWidth(), ed2->getHeight(), q->editorScale);
        check (ed2->getWidth() == 1200 && ed2->getHeight() == 800, "échelle de l'interface restaurée avec le projet (125 %)");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::setvbuf (stdout, nullptr, _IONBF, 0);     // journal complet même en cas de plantage (CI)
    std::printf ("AuraPitch — banc du plug-in (étapes 3, 5A et lot 2)\n");
    testParameters();
    testLatency();
    testProcessing();
    testEditor();
    testStep5A();
    testLot2();
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

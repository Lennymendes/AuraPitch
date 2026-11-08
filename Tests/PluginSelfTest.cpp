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
static std::vector<std::vector<double>> run (AuraPitchProcessor& p, const std::vector<std::vector<double>>& in, int block)
{
    const int nc = (int) in.size();
    const size_t len = in[0].size();
    juce::AudioBuffer<T> buf (nc, block);
    juce::MidiBuffer midi;
    std::vector<std::vector<double>> out ((size_t) nc, std::vector<double> (len));
    for (size_t pos = 0; pos < len; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, len - pos);
        buf.setSize (nc, n, false, false, true);
        for (int ch = 0; ch < nc; ++ch)
            for (int i = 0; i < n; ++i) buf.setSample (ch, i, (T) in[(size_t) ch][pos + (size_t) i]);
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
    check (params.size() == 26, "26 paramètres (14 + 12 notes)");
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
    check (editor != nullptr && ed->getWidth() == 920 && ed->getHeight() == 600, "éditeur 920 × 600 par défaut");

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
    check (save (*ed, "aurapitch_editor_920x600.png"), "rendu de l'éditeur");
    ed->setSize (1400, 860);
    check (save (*ed, "aurapitch_editor_1400x860.png"), "rendu redimensionné");
    ed->setSize (900, 560);
    check (save (*ed, "aurapitch_editor_900x560.png"), "rendu à la taille minimale");
    ed.reset();
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::setvbuf (stdout, nullptr, _IONBF, 0);     // journal complet même en cas de plantage (CI)
    std::printf ("AuraPitch — banc du plug-in (étape 3)\n");
    testParameters();
    testLatency();
    testProcessing();
    testEditor();
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

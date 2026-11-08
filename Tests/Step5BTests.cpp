// AuraPitch — banc de l'étape 5B : GRAPH EDITOR (capture calée sur la timeline, segmentation en notes, édition,
// rejeu, scission, sauvegarde d'état, temps réel avec éditions concurrentes).
// Sans JUCE : moteur + NoteGraphModel (compilable sous ASan / UBSan / TSan). Avec AURAPITCH_STEP5B_PLUGIN : en plus,
// le processeur JUCE (AudioPlayHead simulé, getStateInformation / setStateInformation). Code de sortie 0 si tout passe.
#include "VocalRealism.h"

#include "../Source/DSP/AuraPitchEngine.h"
#include "../Source/DSP/NoteGraphModel.h"

#include <memory>
#include <string>
#include <thread>

#if AURAPITCH_STEP5B_PLUGIN
 #include "PluginProcessor.h"
 #include "GUI/PluginEditor.h"
#endif

using namespace aura::pitch;
using namespace test;

namespace
{
constexpr double kTimelineStart = 10.0;     // la phrase commence à 10 s sur la timeline du DAW

double noteHz (double m) { return 440.0 * std::exp2 ((m - 69.0) / 12.0); }

std::unique_ptr<AuraPitchEngine> makeEngine (double fs, int ch, TrackingMode mode, bool preserve = true)
{
    auto e = std::make_unique<AuraPitchEngine>();
    e->prepare (fs, ch);
    AuraPitchParams p;
    p.mode = mode; p.retuneMs = 0; p.scale = ScaleType::Chromatic; p.formantPreserve = preserve;
    e->setParameters (p);
    e->reset();
    return e;
}

/** Passe de lecture : timeline à chaque bloc ; en capture, les trames vont dans le modèle (comme le timer du plug-in). */
std::vector<double> playPass (AuraPitchEngine& e, std::vector<double> x, double fs, NoteGraphModel* capture = nullptr,
                              int block = 512)
{
    e.setCapture (capture != nullptr);
    std::vector<CaptureFrame> batch;
    for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, x.size() - pos);
        e.setTimeline (kTimelineStart + (double) pos / fs, true);
        double* io[1] = { x.data() + pos };
        e.process (io, 1, n);
        if (capture != nullptr)
        {
            batch.clear();
            e.drainCapture ([&] (const CaptureFrame& f) { batch.push_back (f); });
            capture->addFrames (batch);
            capture->segment (false);
        }
    }
    if (capture != nullptr) { capture->segment (true); e.setCapture (false); }
    return x;
}

void publish (AuraPitchEngine& e, const NoteGraphModel& m) { e.publishGraph (m.buildSnapshot()); }

/** Trajectoire de hauteur (cents par rapport à refHz) entre t0 et t1, trames voisées. */
std::vector<std::pair<double, double>> pitchTrack (const std::vector<double>& y, double fs, double refHz, double t0, double t1)
{
    PitchDetector det;
    det.prepare (fs);
    det.setInputType (InputType::Instrument);
    std::vector<std::pair<double, double>> out;
    det.process (y.data(), (int) y.size(), [&] (const PitchEstimate& e, int)
    {
        const double t = (double) e.frameCentre / fs;
        if (t >= t0 && t <= t1 && e.voiced) out.emplace_back (t, 1200.0 * std::log2 (e.frequencyHz / refHz));
    });
    return out;
}

/** Moyenne et amplitude de la composante à f Hz (moindres carrés : a + b sin + c cos). */
std::pair<double, double> meanAndComponent (const std::vector<std::pair<double, double>>& tr, double f)
{
    double s[3][3] = {}, r[3] = {};
    for (auto [t, c] : tr)
    {
        const double v[3] = { 1.0, std::sin (2 * kPi * f * t), std::cos (2 * kPi * f * t) };
        for (int i = 0; i < 3; ++i) { r[i] += v[i] * c; for (int j = 0; j < 3; ++j) s[i][j] += v[i] * v[j]; }
    }
    // Gauss 3×3
    for (int i = 0; i < 3; ++i)
        for (int k = i + 1; k < 3; ++k)
        {
            const double m = s[k][i] / s[i][i];
            for (int j = i; j < 3; ++j) s[k][j] -= m * s[i][j];
            r[k] -= m * r[i];
        }
    double x[3];
    for (int i = 2; i >= 0; --i)
    {
        double acc = r[i];
        for (int j = i + 1; j < 3; ++j) acc -= s[i][j] * x[j];
        x[i] = acc / s[i][i];
    }
    return { x[0], std::hypot (x[1], x[2]) };
}

//--- La phrase : A3 +15 c | C4 −20 c ~ legato ~ E4 +10 c | G4 −10 c, vibrato 5,5 Hz ±30 c -------------------------
struct PhraseNote { double start, end, midi; };
const std::vector<PhraseNote> kPhrase { { 0.30, 0.85, 57.15 }, { 0.95, 1.60, 59.80 }, { 1.60, 2.20, 64.10 }, { 2.30, 2.90, 66.90 } };
constexpr double kVibratoHz = 5.5, kVibratoCents = 30.0;

double phraseMidi (double t)
{
    for (size_t i = 0; i < kPhrase.size(); ++i)
    {
        const auto& n = kPhrase[i];
        if (t < n.start - 0.03 || t >= n.end + 0.03) continue;
        double m = n.midi;
        // liaison legato C4 -> E4 : glissé de 40 ms centré sur la frontière
        if (i == 1 && t > n.end - 0.02) m += (kPhrase[2].midi - n.midi) * std::clamp ((t - (n.end - 0.02)) / 0.04, 0.0, 1.0);
        if (i == 2 && t < n.start + 0.02) m = kPhrase[1].midi + (n.midi - kPhrase[1].midi) * std::clamp ((t - (n.start - 0.02)) / 0.04, 0.0, 1.0);
        const double fade = std::clamp ((t - n.start) / 0.10, 0.0, 1.0);
        return m + fade * kVibratoCents / 100.0 * std::sin (2 * kPi * kVibratoHz * t);
    }
    return 0.0;
}
double phraseEnv (double t)
{
    for (size_t i = 0; i < kPhrase.size(); ++i)
    {
        const auto& n = kPhrase[i];
        const double a = i == 2 ? 1.0 : std::clamp ((t - n.start) / 0.01, 0.0, 1.0);        // pas d'attaque en legato
        const double r = i == 1 ? 1.0 : std::clamp ((n.end - t) / 0.01, 0.0, 1.0);
        if (t >= n.start && t < n.end) return std::min (a, r);
    }
    return 0.0;
}

std::vector<double> phrase (double fs, bool jitter = true)
{
    VoiceSpec vs;
    if (! jitter) { vs.jitter = 0.0; vs.shimmerDb = 0.0; }
    return makeRealVoice (fs, 3.2, [] (double t) { const double m = phraseMidi (t); return m > 0 ? noteHz (m) : 0.0; },
                          phraseEnv, [] (double) { return 0.0; }, vs);
}

const char* modeName (TrackingMode m) { return m == TrackingMode::Live ? "Live  " : "Studio"; }
} // namespace

//==================================================================================================================
static void testCaptureAndSegmentation()
{
    std::printf ("\n[1] Capture et segmentation : A3 +15 c | C4 −20 c ~legato~ E4 +10 c | G4 −10 c, vibrato ±30 c, timeline à 10 s\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            auto e = makeEngine (fs, 1, mode);
            NoteGraphModel model;
            playPass (*e, phrase (fs), fs, &model);
            const auto& b = model.getBlobs();
            std::printf ("  %5.1f k %s : %zu notes, %zu trames capturées (%.1f Ko)\n", fs / 1000, modeName (mode), b.size(),
                         model.getFrames().size(), (double) (model.getFrames().size() * sizeof (CaptureFrame)) / 1024.0);
            bool ok = b.size() == kPhrase.size();
            for (size_t i = 0; i < std::min (b.size(), kPhrase.size()); ++i)
            {
                const double s = b[i].startTimeSec - kTimelineStart, en = b[i].endTimeSec() - kTimelineStart;
                const double dc = (b[i].originalCenterPitch - kPhrase[i].midi) * 100.0;
                std::printf ("    note %zu : %.3f – %.3f s (attendu %.2f – %.2f) · centre %+6.2f c · cible %s · %zu points\n", i + 1,
                             s, en, kPhrase[i].start, kPhrase[i].end, dc,
                             std::to_string ((int) b[i].targetPitch).c_str(), b[i].pitchDeviationsCents.size());
                ok = ok && std::abs (s - kPhrase[i].start) < 0.030 && std::abs (en - kPhrase[i].end) < 0.030 && std::abs (dc) < 10.0
                        && std::abs (b[i].targetPitch - std::round (kPhrase[i].midi)) < 1.0e-9;
            }
            check (ok, "4 notes aux bons instants (±30 ms), centres à ±10 c, cibles = notes justes, liaison legato séparée");
        }
}

//==================================================================================================================
static void testEditAndReplay()
{
    std::printf ("\n[2] Édition et rejeu : 2e note (C4 −20 c, vibrato ±30 c) -> D4, vibrato 0 ; second passage au même instant\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            const auto x = phrase (fs);
            auto e = makeEngine (fs, 1, mode);
            NoteGraphModel model;
            playPass (*e, x, fs, &model);
            if (model.getBlobs().size() != kPhrase.size()) { check (false, "capture"); continue; }
            auto& n2 = model.getBlobs()[1];
            n2.targetPitch = 62.0;
            n2.vibratoDepth = 0.0;
            publish (*e, model);
            const auto y = playPass (*e, x, fs);
            const double lat = (double) e->getLatencySamples() / fs;

            const double t0 = kPhrase[1].start + 0.12, t1 = kPhrase[1].end - 0.08;
            const auto out = pitchTrack (y, fs, noteHz (62), t0 + lat, t1 + lat);
            const auto in  = pitchTrack (x, fs, noteHz (62), t0, t1);
            const auto [mean, vibOut] = meanAndComponent (out, kVibratoHz);
            const auto [meanIn, vibIn] = meanAndComponent (in, kVibratoHz);
            // les autres notes : cibles justes, vibrato naturel conservé
            const auto o3 = meanAndComponent (pitchTrack (y, fs, noteHz (64), kPhrase[2].start + 0.12 + lat, kPhrase[2].end - 0.08 + lat), kVibratoHz);
            std::printf ("  %5.1f k %s : note 2 -> D4 %+6.3f c, vibrato %5.2f c -> %4.2f c (−%.1f %%) · note 3 -> E4 %+5.2f c, vibrato %4.1f c conservé\n",
                         fs / 1000, modeName (mode), mean, vibIn, vibOut, 100.0 * (1.0 - vibOut / vibIn), o3.first, o3.second);
            check (std::abs (mean) < 0.1 && out.size() > 50, "note éditée : D4 à ±0,1 c");
            check (vibOut < 0.05 * vibIn, "vibrato réduit de plus de 95 %");
            check (std::abs (o3.first) < 1.0 && std::abs (o3.second - kVibratoCents) < 4.0, "note non éditée : juste, vibrato intact");
            (void) meanIn;
        }
}

//==================================================================================================================
static void testTransparencyAndClicks()
{
    std::printf ("\n[3] Transparence et frontières : voix A3 +40 c (protocole de l'étape 4, Preserve OFF) corrigée par une note éditée ;\n"
                 "    voyelle sombre en legato C4 -> E4 éditée (D4 droit puis E4 droit) : énergie > 6 kHz aux frontières\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            // Protocole de l'étape 4 : voix A3 +40 c (jitter 0), Preserve OFF ; ici corrigée par une note éditée (A3,
            // vibrato 0) au lieu de la correction automatique. Transmission de chaque harmonique (Blackman-Harris, 1,5 s).
            const double fin = 220.0 * std::exp2 (40.0 / 1200.0), fout = 220.0;
            VoiceSpec vs; vs.jitter = 0; vs.shimmerDb = 0; vs.breathDb = -80;
            const auto x = makeRealVoice (fs, 3.0, [&] (double t) { return t < 0.2 ? 0.0 : fin; },
                                          [] (double t) { return std::clamp ((t - 0.2) / 0.01, 0.0, 1.0); }, [] (double) { return 0.0; }, vs);
            auto e = makeEngine (fs, 1, mode, false);
            NoteGraphModel model;
            playPass (*e, x, fs, &model);
            if (model.getBlobs().size() != 1) { check (false, "capture du son"); continue; }
            model.getBlobs()[0].targetPitch = 57.0;
            model.getBlobs()[0].vibratoDepth = 0.0;
            publish (*e, model);
            const auto y = playPass (*e, x, fs);
            auto ea = makeEngine (fs, 1, mode, false);                // même correction, par la voie automatique
            const auto ya = playPass (*ea, x, fs);
            const size_t lat = (size_t) e->getLatencySamples(), a = (size_t) (1.2 * fs), len = (size_t) (1.5 * fs);
            double worst = 0.0, vsAuto = 0.0;
            int kmax = 0;
            for (int h = 1; h * fin < 19500; ++h, kmax = h)
            {
                const double d  = 10 * std::log10 (linePower (y, fs, h * fout, a + lat, len) / linePower (x, fs, h * fin, a, len));
                const double da = 10 * std::log10 (linePower (ya, fs, h * fout, a + lat, len) / linePower (x, fs, h * fin, a, len));
                worst = std::max (worst, std::abs (d));
                vsAuto = std::max (vsAuto, std::abs (d - da));
            }
            std::printf ("  %5.1f k %s : harmoniques 1 … %d (≤ 19,5 kHz) à ±%.3f dB · écart à la correction automatique ±%.3f dB",
                         fs / 1000, modeName (mode), kmax, worst, vsAuto);
            check (worst < 0.05, "transparence spectrale ±0,05 dB (rejeu d'une note éditée)");
            check (vsAuto < 0.01, "rejeu édité = correction automatique au même rapport (±0,01 dB)");

            // frontières : voyelle sombre (rien au-dessus de 6 kHz), legato C4 -> E4, les deux notes éditées
            auto f0At = [] (double t) { return t < 0.3 || t > 1.9 ? 0.0 : noteHz (t < 1.08 ? 59.8 : t < 1.12 ? 59.8 + 4.3 * (t - 1.08) / 0.04 : 64.1) * std::exp2 (0.25 * std::sin (2 * kPi * 5.5 * t) / 12.0); };
            auto v = makeVowel (fs, 2.2, [&] (double t) { const double f = f0At (t); return f > 0 ? f : 261.6; }, 0.3, 1.0, 2.0);
            for (size_t i = 0; i < v.size(); ++i)
            {
                const double t = (double) i / fs;
                v[i] *= std::clamp ((t - 0.3) / 0.02, 0.0, 1.0) * std::clamp ((1.9 - t) / 0.02, 0.0, 1.0);
            }
            auto e2 = makeEngine (fs, 1, mode, false);
            NoteGraphModel m2;
            playPass (*e2, v, fs, &m2);
            if (m2.getBlobs().size() != 2) { std::printf ("\n"); check (false, "legato segmenté en 2 notes"); continue; }
            m2.getBlobs()[0].targetPitch = 62.0; m2.getBlobs()[0].vibratoDepth = 0.0;
            m2.getBlobs()[1].targetPitch = 64.0; m2.getBlobs()[1].vibratoDepth = 0.0;
            publish (*e2, m2);
            const auto w = playPass (*e2, v, fs);
            const double l2 = (double) e2->getLatencySamples() / fs;
            const auto hf = highBandRms (w, fs);
            const double tb = m2.getBlobs()[1].startTimeSec - kTimelineStart;
            const double atBoundary = maxDb (hf, fs, tb - 0.03 + l2, tb + 0.03 + l2);
            const double interior = std::max (maxDb (hf, fs, 0.6 + l2, tb - 0.1 + l2), maxDb (hf, fs, tb + 0.1 + l2, 1.7 + l2));
            const double full = rmsDb (w, (size_t) ((0.6 + l2) * fs), (size_t) ((1.7 + l2) * fs));
            std::printf (" · frontière D4|E4 : > 6 kHz %+.1f dB vs notes %+.1f dB (voix %.1f dBFS)\n", atBoundary, interior, full);
            check (atBoundary < interior + 3.0, "aucun clic à la frontière des notes éditées");
        }
}

//==================================================================================================================
static void testSplitAndState()
{
    std::printf ("\n[4] Scission et sauvegarde : note legato tenue (C#4 ±30 c, 1,2 s) scindée en C4 puis E4, sérialisée,\n"
                 "    restaurée dans un nouveau modèle et un nouveau moteur : rendu identique au bit près\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            VoiceSpec vs; vs.seed = 9;
            const auto x = makeRealVoice (fs, 1.8, [] (double t) { return t < 0.3 || t > 1.5 ? 0.0 : noteHz (61.1 + 0.3 * std::sin (2 * kPi * 5.2 * t)); },
                                          [] (double t) { return std::clamp ((t - 0.3) / 0.01, 0.0, 1.0) * std::clamp ((1.5 - t) / 0.01, 0.0, 1.0); },
                                          [] (double) { return 0.0; }, vs);
            auto e = makeEngine (fs, 1, mode);
            NoteGraphModel model;
            playPass (*e, x, fs, &model);
            bool ok = model.getBlobs().size() == 1;
            const double cut = kTimelineStart + 0.9;
            ok = ok && model.split (0, cut);
            if (! ok) { check (false, "scission"); continue; }
            model.getBlobs()[0].targetPitch = 60.0; model.getBlobs()[0].vibratoDepth = 0.0;
            model.getBlobs()[1].targetPitch = 64.0; model.getBlobs()[1].vibratoDepth = 0.0;
            model.getBlobs()[1].transitionInRatio = 0.5;
            const auto bytes = model.serialize();

            NoteGraphModel restored;
            const bool loaded = restored.deserialize (bytes.data(), bytes.size());
            auto ea = makeEngine (fs, 1, mode), eb = makeEngine (fs, 1, mode);
            publish (*ea, model);
            publish (*eb, restored);
            const auto ya = playPass (*ea, x, fs), yb = playPass (*eb, x, fs);
            const double lat = (double) ea->getLatencySamples() / fs;
            const auto c4 = meanAndComponent (pitchTrack (ya, fs, noteHz (60), 0.45 + lat, 0.85 + lat), 5.2);
            const auto e4 = meanAndComponent (pitchTrack (ya, fs, noteHz (64), 1.05 + lat, 1.45 + lat), 5.2);
            std::printf ("  %5.1f k %s : %zu octets · C4 %+5.2f c, E4 %+5.2f c · rendu restauré %s\n", fs / 1000, modeName (mode),
                         bytes.size(), c4.first, e4.first, ya == yb ? "identique au bit près" : "DIFFÉRENT");
            check (loaded && restored.getBlobs().size() == 2, "état restauré : 2 notes");
            check (std::abs (c4.first) < 0.5 && std::abs (e4.first) < 0.5, "scission : C4 puis E4");
            check (ya == yb, "rendu après restauration identique au bit près");
        }
}

//==================================================================================================================
static void testRealtimeWithConcurrentEdits()
{
    std::printf ("\n[5] Temps réel : rejeu et capture pendant que l'interface modifie les notes en parallèle (thread séparé,\n"
                 "    publication d'un snapshot toutes les 2 ms) : allocations sur le thread audio\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            const auto x = phrase (fs);
            auto e = makeEngine (fs, 2, mode);
            NoteGraphModel model;
            {
                auto cap = makeEngine (fs, 1, mode);
                playPass (*cap, x, fs, &model);
            }
            publish (*e, model);
            std::atomic<bool> stop { false };
            std::atomic<int> edits { 0 };
            std::thread ui ([&]
            {
                NoteGraphModel m = model;
                int k = 0;
                while (! stop.load())
                {
                    auto& b = m.getBlobs();
                    if (! b.empty())
                    {
                        auto& n = b[(size_t) k % b.size()];
                        n.targetPitch = std::round (n.originalCenterPitch) + (k % 3) - 1;
                        n.vibratoDepth = (k % 5) * 0.5;
                        n.formantOffsetSemitones = (k % 4 == 0) ? 1.5 : 0.0;
                        n.transitionInRatio = (k % 2) * 0.6;
                    }
                    e->publishGraph (m.buildSnapshot());
                    e->collectGraphGarbage();
                    ++k; edits.fetch_add (1);
                    std::this_thread::sleep_for (std::chrono::milliseconds (2));
                }
            });
            auto l = x, r = x;
            gAllocations = 0;
            gCountAllocations = true;
            for (int pass = 0; pass < 2; ++pass)
            {
                e->setCapture (pass == 1);
                for (size_t pos = 0; pos < l.size(); pos += 256)
                {
                    const int n = (int) std::min<size_t> (256, l.size() - pos);
                    e->setTimeline (kTimelineStart + (double) pos / fs, true);
                    double* io[2] = { l.data() + pos, r.data() + pos };
                    e->process (io, 2, n);
                }
            }
            gCountAllocations = false;
            stop = true;
            ui.join();
            int captured = e->drainCapture ([] (const CaptureFrame&) {});
            e->collectGraphGarbage();
            bool finite = true;
            for (double v : l) finite = finite && std::isfinite (v);
            std::printf ("  %5.1f k %s : %d éditions publiées, %d trames capturées, %ld allocation(s) sur le thread audio, sortie %s\n",
                         fs / 1000, modeName (mode), edits.load(), captured, gAllocations.load(), finite ? "finie" : "NaN");
            check (gAllocations.load() == 0 && finite && edits.load() > 10, "aucune allocation, aucun verrou, sortie finie");
        }
}

//==================================================================================================================
static void testNeutral()
{
    std::printf ("\n[6] Sans notes ou hors lecture : la sortie est celle de l'étape 5A au bit près\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            const auto x = phrase (fs);
            auto a = makeEngine (fs, 1, mode), b = makeEngine (fs, 1, mode), c = makeEngine (fs, 1, mode);
            auto ya = x;
            for (size_t pos = 0; pos < ya.size(); pos += 512)
            {
                double* io[1] = { ya.data() + pos };
                a->process (io, 1, (int) std::min<size_t> (512, ya.size() - pos));
            }
            const auto yb = playPass (*b, x, fs);                      // timeline, aucune note
            NoteGraphModel m;
            { auto cap = makeEngine (fs, 1, mode); playPass (*cap, x, fs, &m); }
            publish (*c, m);
            c->setCapture (false);
            auto yc = x;                                                  // notes, mais transport arrêté
            for (size_t pos = 0; pos < yc.size(); pos += 512)
            {
                c->setTimeline (kTimelineStart + (double) pos / fs, false);
                double* io[1] = { yc.data() + pos };
                c->process (io, 1, (int) std::min<size_t> (512, yc.size() - pos));
            }
            std::printf ("  %5.1f k %s : timeline sans notes %s · notes, transport arrêté %s\n", fs / 1000, modeName (mode),
                         ya == yb ? "identique" : "DIFFÉRENT", ya == yc ? "identique" : "DIFFÉRENT");
            check (ya == yb && ya == yc, "aucune note active : chemin de l'étape 5A au bit près");
        }
}

#if AURAPITCH_STEP5B_PLUGIN
//==================================================================================================================
namespace
{
struct FakePlayHead final : juce::AudioPlayHead
{
    double seconds = 0.0, bpm = 120.0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setTimeInSeconds (seconds);
        p.setIsPlaying (playing);
        p.setBpm (bpm);
        p.setPpqPosition (seconds * bpm / 60.0);
        return p;
    }
};

std::unique_ptr<AuraPitchProcessor> makeProcessor (double fs, FakePlayHead& ph)
{
    std::unique_ptr<AuraPitchProcessor> p (dynamic_cast<AuraPitchProcessor*> (createPluginFilter()));
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    p->setBusesLayout (layout);
    p->setRateAndBufferSizeDetails (fs, 512);
    p->setPlayHead (&ph);
    p->prepareToPlay (fs, 512);
    if (auto* r = p->getState().getParameter (aura::params::retuneSpeed)) r->setValueNotifyingHost (r->convertTo0to1 (0.0f));
    return p;
}

std::vector<float> renderProcessor (AuraPitchProcessor& p, FakePlayHead& ph, const std::vector<double>& x, double fs, bool pump)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    std::vector<float> out (x.size());
    for (size_t pos = 0; pos < x.size(); pos += 512)
    {
        const int n = (int) std::min<size_t> (512, x.size() - pos);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i) { buf.setSample (0, i, (float) x[pos + (size_t) i]); buf.setSample (1, i, (float) x[pos + (size_t) i]); }
        ph.seconds = kTimelineStart + (double) pos / fs;
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i) out[pos + (size_t) i] = buf.getSample (0, i);
        if (pump) p.pumpGraph (false);
    }
    if (pump) p.pumpGraph (true);
    return out;
}
} // namespace

static void testProcessor()
{
    std::printf ("\n[7] Processeur JUCE : AudioPlayHead simulé (10 s, 120 BPM), CAPTURE, scission C4 | E4,\n"
                 "    getStateInformation -> nouvelle instance -> setStateInformation : rendu identique au bit près\n");
    for (double fs : { 48000.0, 384000.0 })
    {
        FakePlayHead ph;
        auto p1 = makeProcessor (fs, ph);
        VoiceSpec vs; vs.seed = 4;
        std::vector<double> x = makeRealVoice (fs, 1.8, [] (double t) { return t < 0.3 || t > 1.5 ? 0.0 : noteHz (61.1 + 0.3 * std::sin (2 * kPi * 5.2 * t)); },
                                               [] (double t) { return std::clamp ((t - 0.3) / 0.01, 0.0, 1.0) * std::clamp ((1.5 - t) / 0.01, 0.0, 1.0); },
                                               [] (double) { return 0.0; }, vs);
        p1->setGraphCapture (true);
        renderProcessor (*p1, ph, x, fs, true);
        p1->setGraphCapture (false);
        auto& model = p1->getGraphModel();
        const bool split = model.getBlobs().size() == 1 && model.split (0, kTimelineStart + 0.9);
        if (split)
        {
            model.getBlobs()[0].targetPitch = 60.0; model.getBlobs()[0].vibratoDepth = 0.0;
            model.getBlobs()[1].targetPitch = 64.0; model.getBlobs()[1].vibratoDepth = 0.0;
            model.getBlobs()[1].formantOffsetSemitones = 2.0;
        }
        p1->graphEdited();
        juce::MemoryBlock state;
        p1->getStateInformation (state);

        FakePlayHead ph2;
        auto p2 = makeProcessor (fs, ph2);
        p2->setStateInformation (state.getData(), (int) state.getSize());
        p1->prepareToPlay (fs, 512);
        p2->prepareToPlay (fs, 512);
        const auto y1 = renderProcessor (*p1, ph, x, fs, false);
        const auto y2 = renderProcessor (*p2, ph2, x, fs, false);
        std::vector<double> yd (y1.begin(), y1.end());
        const double lat = (double) p1->getLatencySamples() / fs;
        const auto e4 = meanAndComponent (pitchTrack (yd, fs, noteHz (64), 1.05 + lat, 1.45 + lat), 5.2);
        std::printf ("  %5.1f k : capture -> %zu note(s), scission %s, état %zu octets, notes restaurées %zu · E4 %+5.2f c · rendu %s\n",
                     fs / 1000, model.getBlobs().size(), split ? "ok" : "ÉCHEC", (size_t) state.getSize(),
                     p2->getGraphModel().getBlobs().size(), e4.first, y1 == y2 ? "identique au bit près" : "DIFFÉRENT");
        check (split && p2->getGraphModel().getBlobs().size() == 2, "processeur : capture, scission, état restauré");
        check (std::abs (e4.first) < 0.5, "processeur : la note éditée est rejouée (E4, formant +2 dt)");
        check (y1 == y2, "processeur : rendu après restauration identique au bit près");
    }
}

//==================================================================================================================
static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::ModifierKeys mods, int clicks = 1)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (src, pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now, clicks, false);
}

static void testGraphEditorUI()
{
    std::printf ("\n[8] GRAPH EDITOR (interface) : la phrase capturée par le processeur, éditée à la souris\n");
    using MK = juce::ModifierKeys;
    const double fs = 48000;
    FakePlayHead ph;
    auto p = makeProcessor (fs, ph);
    const auto x = phrase (fs);
    p->setGraphCapture (true);
    renderProcessor (*p, ph, x, fs, true);
    p->setGraphCapture (false);
    auto& model = p->getGraphModel();
    check (model.getBlobs().size() == 4, "capture par le processeur : 4 notes");
    if (model.getBlobs().size() != 4) return;

    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* editor = dynamic_cast<AuraPitchEditor*> (ed.get());
    editor->setGraphView (true);
    auto& ge = editor->getGraphEditor();
    ge.showRange (kTimelineStart, 3.2, 62.0, 18.0);
    auto centreOf = [&] (int i)
    {
        const auto& n = model.getBlobs()[(size_t) i];
        return juce::Point<float> (ge.xForTime (n.startTimeSec + 0.5 * n.durationSec), ge.yForMidi (n.targetPitch));
    };
    auto dragNote = [&] (int i, juce::Point<float> delta, MK mods, juce::Point<float> from = {})
    {
        const auto a = from.isOrigin() ? centreOf (i) : from;
        ge.mouseDown (mouseAt (ge, a, MK (MK::leftButtonModifier | mods.getRawFlags())));
        ge.mouseDrag (mouseAt (ge, a + delta, MK (MK::leftButtonModifier | mods.getRawFlags())));
        ge.mouseUp (mouseAt (ge, a + delta, mods));
    };
    const float rh = ge.rowHeight();
    auto& b = model.getBlobs();

    dragNote (1, { 0.0f, -2.0f * rh }, MK());
    const double moved = b[1].targetPitch;
    dragNote (1, { 0.0f, -0.37f * rh }, MK (MK::altModifier));
    const double fine = b[1].targetPitch;
    ge.mouseDoubleClick (mouseAt (ge, centreOf (1), MK (MK::leftButtonModifier), 2));
    const double quantized = b[1].targetPitch;
    dragNote (1, { 0.0f, 100.0f }, MK (MK::commandModifier));
    const double depth = b[1].vibratoDepth;
    dragNote (1, { 0.0f, -40.0f }, MK (MK::commandModifier | MK::altModifier));
    const double formant = b[1].formantOffsetSemitones;
    dragNote (1, { 0.0f, 40.0f }, MK (MK::commandModifier | MK::altModifier));
    const auto r2 = juce::Point<float> (ge.xForTime (b[2].startTimeSec) + 2.0f, ge.yForMidi (b[2].targetPitch));
    dragNote (2, { 40.0f, 0.0f }, MK(), r2);
    const double transition = b[2].transitionInRatio;
    std::printf ("  note 2 glissée de 2 rangées -> %.2f ; Alt +0,37 rangée -> %.2f ; double-clic -> %.2f ; Cmd ↓100 px -> vibrato %.0f %% ;\n"
                 "  Cmd+Alt ↑40 px -> formant %+.1f dt ; poignée gauche de la note 3, 40 px -> glissé %.0f %%\n",
                 moved, fine, quantized, depth * 100.0, formant, transition * 100.0);
    check (std::abs (moved - 62.0) < 1e-9, "glisser : cible au demi-ton (C4 -> D4)");
    check (std::abs (fine - 62.37) < 0.006, "Alt : cible au cent");
    check (std::abs (quantized - 62.0) < 1e-9, "double-clic : note quantifiée (0 c)");
    check (std::abs (depth) < 1e-9, "Cmd + glisser : vibrato 0 %");
    check (std::abs (formant - 2.0) < 0.06 && std::abs (b[1].formantOffsetSemitones) < 1e-9, "Cmd + Alt + glisser : formant de la note");
    check (std::abs (transition - 0.5) < 1e-9, "poignée gauche : glissé d'attaque");

    // clic droit dans la 4e note : scission
    const auto cut = juce::Point<float> (ge.xForTime (b[3].startTimeSec + 0.3), ge.yForMidi (b[3].targetPitch));
    ge.mouseDown (mouseAt (ge, cut, MK (MK::rightButtonModifier)));
    ge.mouseUp (mouseAt (ge, cut, MK()));
    check (model.getBlobs().size() == 5, "clic droit : scission (5 notes)");

    // lasso sur les deux premières notes
    const auto l0 = juce::Point<float> (ge.xForTime (kTimelineStart + 0.2), ge.plotArea().getY() + 2.0f);
    const auto l1 = juce::Point<float> (ge.xForTime (kTimelineStart + 1.2), ge.plotArea().getBottom() - 2.0f);
    ge.mouseDown (mouseAt (ge, l0, MK (MK::leftButtonModifier)));
    ge.mouseDrag (mouseAt (ge, l1, MK (MK::leftButtonModifier)));
    ge.mouseUp (mouseAt (ge, l1, MK()));
    const auto& sel = ge.getSelection();
    check (sel.size() == 5 && sel[0] && sel[1] && ! sel[2], "lasso : sélection des notes 1 et 2");

    const double span0 = ge.getViewSpan();
    juce::MouseWheelDetails w {}; w.deltaY = 0.5f;
    ge.mouseWheelMove (mouseAt (ge, ge.plotArea().getCentre(), MK (MK::commandModifier)), w);
    check (ge.getViewSpan() < span0, "Cmd + molette : zoom dans le temps");
    ge.showRange (kTimelineStart, 3.2, 62.0, 18.0);

    // les éditions s'entendent : note 2 en D4 droite, au rendu suivant
    p->prepareToPlay (fs, 512);
    const auto y = renderProcessor (*p, ph, x, fs, false);
    std::vector<double> yd (y.begin(), y.end());
    const double lat = (double) p->getLatencySamples() / fs;
    const auto o2 = meanAndComponent (pitchTrack (yd, fs, noteHz (62), kPhrase[1].start + 0.12 + lat, kPhrase[1].end - 0.08 + lat), kVibratoHz);
    std::printf ("  rendu après édition : note 2 -> D4 %+5.2f c, vibrato %.2f c\n", o2.first, o2.second);
    check (std::abs (o2.first) < 0.5 && o2.second < 2.0, "les éditions de l'interface sont rejouées");

    ge.followButton.setToggleState (false, juce::dontSendNotification);
    {
        // un bloc lu à 11,9 s : la tête de lecture se place au milieu de la phrase
        juce::AudioBuffer<float> buf (2, 512);
        buf.clear();
        juce::MidiBuffer midi;
        ph.seconds = kTimelineStart + 1.9;
        p->processBlock (buf, midi);
    }
    ge.showRange (kTimelineStart + 0.1, 3.0, 62.0, 18.0);
    ge.tick (1.0 / 60.0);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aurapitch_graph_editor.png");
    file.deleteFile();
    juce::FileOutputStream os (file);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("  capture : %s\n", file.getFullPathName().toRawUTF8());
    check (img.isValid(), "rendu du GRAPH EDITOR");
    ed.reset();
}
#endif

int main (int argc, char** argv)
{
#if AURAPITCH_STEP5B_PLUGIN
    juce::ScopedJuceInitialiser_GUI juceInit;
#endif
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&] (const char* k) { return only.empty() || only == k; };
    if (want ("capture"))  testCaptureAndSegmentation();
    if (want ("replay"))   testEditAndReplay();
    if (want ("transp"))   testTransparencyAndClicks();
    if (want ("split"))    testSplitAndState();
    if (want ("rt"))       testRealtimeWithConcurrentEdits();
    if (want ("neutral"))  testNeutral();
#if AURAPITCH_STEP5B_PLUGIN
    if (want ("plugin"))   testProcessor();
    if (want ("ui"))       testGraphEditorUI();
#endif
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

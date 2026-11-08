// AuraPitch — banc de l'étape 5A : Transition (Flex), MIDI Target, Auto-Key, Throat, Doubler, transparence, charge CPU.
// Code de sortie 0 si tout passe.
#include "VocalRealism.h"

#include "../Source/DSP/AuraPitchEngine.h"

#include <chrono>
#include <complex>
#include <memory>
#include <string>

using namespace aura::pitch;
using namespace test;

namespace
{
struct TimedMidi { double seconds; MidiTargetEvent ev; };

std::unique_ptr<AuraPitchEngine> makeEngine (double fs, int ch, TrackingMode mode, AuraPitchParams p = {})
{
    auto e = std::make_unique<AuraPitchEngine>();
    e->prepare (fs, ch);
    p.mode = mode;
    e->setParameters (p);
    e->reset();
    return e;
}

AuraPitchParams hardTune (ScaleType scale = ScaleType::Chromatic, int key = 0)
{
    AuraPitchParams p;
    p.retuneMs = 0; p.humanize = 0; p.scale = scale; p.key = key;
    return p;
}

/** Traite par blocs ; MIDI injecté à l'échantillon ; paramètres modifiables en cours de route ; trames FIFO. */
std::vector<std::vector<double>> run (AuraPitchEngine& eng, std::vector<std::vector<double>> io, double fs,
                                      const std::vector<TimedMidi>& midi = {},
                                      std::function<void (double, AuraPitchEngine&)> perBlock = {},
                                      std::vector<PitchFrame>* frames = nullptr, int block = 256)
{
    const size_t len = io[0].size();
    size_t next = 0;
    for (size_t pos = 0; pos < len; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, len - pos);
        if (perBlock) perBlock ((double) pos / fs, eng);
        while (next < midi.size() && (size_t) (midi[next].seconds * fs) < pos + (size_t) n)
        {
            eng.addMidiEvent (midi[next].ev, (int) ((size_t) (midi[next].seconds * fs) - std::min (pos, (size_t) (midi[next].seconds * fs))));
            ++next;
        }
        double* p[2] = { io[0].data() + pos, io.size() > 1 ? io[1].data() + pos : nullptr };
        eng.process (p, (int) io.size(), n);
        eng.getPitchFifo().drain ([&] (const PitchFrame& f) { if (frames) frames->push_back (f); });
    }
    return io;
}

const char* modeName (TrackingMode m) { return m == TrackingMode::Live ? "Live  " : "Studio"; }
double noteHz (double m) { return 440.0 * std::exp2 ((m - 69.0) / 12.0); }
auto one  = [] (double) { return 1.0; };
auto none = [] (double) { return 0.0; };

/** Mélodie : suite de notes MIDI (durées en s), petit désaccord par note, vibrato sur les notes longues,
    40 ms de consonne / silence entre les notes. */
std::vector<double> melody (double fs, const std::vector<std::pair<int, double>>& notes, unsigned seed)
{
    std::vector<double> starts;
    double t = 0.2;
    for (auto& n : notes) { starts.push_back (t); t += n.second; }
    const double total = t + 0.3;
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> det (-18.0, 18.0);
    std::vector<double> detune;
    for (size_t i = 0; i < notes.size(); ++i) detune.push_back (det (rng));

    auto index = [&] (double tt) -> int
    {
        for (size_t i = notes.size(); i-- > 0;)
            if (tt >= starts[i]) return tt < starts[i] + notes[i].second ? (int) i : -1;
        return -1;
    };
    auto f0 = [&] (double tt)
    {
        const int i = index (tt);
        if (i < 0) return 0.0;
        const double local = tt - starts[(size_t) i];
        const double vib = notes[(size_t) i].second > 0.45 && local > 0.2 ? 25.0 * std::sin (2 * kPi * 5.5 * local) : 0.0;
        return noteHz (notes[(size_t) i].first + (detune[(size_t) i] + vib) / 100.0);
    };
    auto env = [&] (double tt)
    {
        const int i = index (tt);
        if (i < 0) return 0.0;
        const double local = tt - starts[(size_t) i], rest = notes[(size_t) i].second - local;
        return std::clamp (std::min (local, rest - 0.04) / 0.02, 0.0, 1.0);
    };
    VoiceSpec vs; vs.seed = seed;
    return makeRealVoice (fs, total, f0, env, none, vs);
}
} // namespace

//==================================================================================================================
static void testAutoKey()
{
    std::printf ("\n[1] Auto-Key : mélodies synthétiques (voix réaliste, désaccord ±18 c, vibrato), écoute de toute la phrase\n");
    // Si mineur (Bandit) : tonique B3, notes de la gamme naturelle, tonique et quinte appuyées.
    const std::vector<std::pair<int, double>> bMinor {
        { 59, 0.55 }, { 62, 0.30 }, { 66, 0.45 }, { 64, 0.25 }, { 62, 0.25 }, { 61, 0.30 }, { 59, 0.60 }, { 57, 0.25 },
        { 59, 0.35 }, { 66, 0.50 }, { 67, 0.25 }, { 66, 0.30 }, { 64, 0.25 }, { 62, 0.30 }, { 61, 0.25 }, { 59, 0.70 },
        { 62, 0.30 }, { 64, 0.25 }, { 66, 0.45 }, { 71, 0.50 }, { 69, 0.25 }, { 67, 0.25 }, { 66, 0.40 }, { 64, 0.25 },
        { 62, 0.30 }, { 61, 0.25 }, { 59, 0.80 } };
    const std::vector<std::pair<int, double>> cMajor {
        { 60, 0.55 }, { 64, 0.30 }, { 67, 0.45 }, { 65, 0.25 }, { 64, 0.25 }, { 62, 0.30 }, { 60, 0.60 }, { 59, 0.25 },
        { 60, 0.35 }, { 67, 0.50 }, { 69, 0.25 }, { 67, 0.30 }, { 65, 0.25 }, { 64, 0.30 }, { 62, 0.25 }, { 60, 0.70 },
        { 64, 0.30 }, { 65, 0.25 }, { 67, 0.45 }, { 72, 0.50 }, { 71, 0.25 }, { 69, 0.25 }, { 67, 0.40 }, { 65, 0.25 },
        { 64, 0.30 }, { 62, 0.25 }, { 60, 0.80 } };
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    struct Case { const char* name; const std::vector<std::pair<int, double>>* notes; int key; bool minor; };
    for (const Case& c : { Case { "Si mineur", &bMinor, 11, true }, Case { "Do majeur", &cMajor, 0, false } })
        for (double fs : { 48000.0, 384000.0 })
            for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
            {
                auto x = melody (fs, *c.notes, 7);
                auto e = makeEngine (fs, 1, mode);
                e->startKeyDetection();
                run (*e, { x }, fs);
                const auto r = e->getKeyDetection();
                std::printf ("  %5.1f k %s %-9s : %s %s, confiance %5.1f %% (%.1f s voisées)\n", fs / 1000, modeName (mode), c.name,
                             r.key >= 0 ? names[r.key] : "?", r.minor ? "Minor" : "Major", 100 * r.confidence, r.seconds);
                check (r.key == c.key && r.minor == c.minor && r.confidence > 0.90, "Auto-Key : bonne tonalité, confiance > 90 %");
            }

    // Écoute courte : la confiance doit rester basse (pas de verdict prématuré).
    {
        const double fs = 48000;
        std::vector<std::pair<int, double>> shortB (bMinor.begin(), bMinor.begin() + 3);
        auto e = makeEngine (fs, 1, TrackingMode::Live);
        e->startKeyDetection();
        run (*e, { melody (fs, shortB, 3) }, fs);
        const auto r = e->getKeyDetection();
        std::printf ("  écoute courte (3 notes, %.1f s) : confiance %.1f %%\n", r.seconds, 100 * r.confidence);
        check (r.confidence < 0.9, "Auto-Key : 3 notes ne suffisent pas à une confiance de 90 %");
        e->stopKeyDetection();
        run (*e, { std::vector<double> (4800, 0.0) }, fs);
        check (! e->isKeyDetectionListening(), "Auto-Key : arrêt de l'écoute");
    }
}

//==================================================================================================================
static void testMidiTarget()
{
    std::printf ("\n[2] MIDI Target : voix en A3, NoteOn C4 à 0,5 s, Pitch Bend +1 (+2 dt) à 2,0 s, NoteOff à 3,0 s (gamme : Chromatic)\n");
    for (double fs : { 48000.0, 96000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            auto p = hardTune();
            p.midiTarget = true;
            auto e = makeEngine (fs, 1, mode, p);
            VoiceSpec vs; vs.jitter = 0.0; vs.shimmerDb = 0.0; vs.breathDb = -60.0;
            auto x = makeRealVoice (fs, 4.0, [] (double) { return 220.0 * std::exp2 (12.0 / 1200.0); }, one, none, vs);
            const std::vector<TimedMidi> midi {
                { 0.5, { MidiTargetEvent::NoteOn, 60, 0.0 } },
                { 2.0, { MidiTargetEvent::PitchBend, 0, 1.0 } },
                { 3.0, { MidiTargetEvent::NoteOff, 60, 0.0 } } };
            bool litDuring = false;
            auto y = run (*e, { x }, fs, midi, [&] (double t, AuraPitchEngine& eng)
            {
                if (t > 1.0 && t < 1.1) litDuring = ((eng.getHeldMidiNotes()[0] >> 60) & 1u) != 0;
            })[0];
            const double lat = (double) e->getLatencySamples() / fs;
            const auto c4 = measurePitch (y, fs, noteHz (60), 0.9 + lat, 1.9 + lat);
            const auto d4 = measurePitch (y, fs, noteHz (62), 2.3 + lat, 2.9 + lat);
            const auto a3 = measurePitch (y, fs, 220.0, 3.3 + lat, 3.9 + lat);
            std::printf ("  %5.1f k %s : C4 %+6.2f c (max %.2f) · bend -> D4 %+6.2f c · NoteOff -> A3 %+6.2f c\n",
                         fs / 1000, modeName (mode), c4.meanCents, c4.maxCents, d4.meanCents, a3.meanCents);
            check (std::abs (c4.meanCents) < 1.0 && c4.maxCents < 3.0 && c4.voicedRatio > 0.95, "MIDI : voix A3 corrigée exactement à C4");
            check (std::abs (d4.meanCents) < 1.0, "MIDI : Pitch Bend +2 dt");
            check (std::abs (a3.meanCents) < 1.0, "MIDI : NoteOff -> retour à la gamme");
            check (litDuring, "MIDI : notes tenues visibles par l'interface");
        }

    // Accord tenu : la voix va à la note de l'accord la plus proche.
    {
        const double fs = 48000;
        auto p = hardTune (); p.midiTarget = true;
        auto e = makeEngine (fs, 1, TrackingMode::Live, p);
        auto x = makeRealVoice (fs, 2.0, [] (double) { return noteHz (64.6); }, one, none);
        auto y = run (*e, { x }, fs, { { 0.1, { MidiTargetEvent::NoteOn, 60, 0 } }, { 0.1, { MidiTargetEvent::NoteOn, 67, 0 } },
                                       { 0.1, { MidiTargetEvent::NoteOn, 64, 0 } } })[0];
        const auto s = measurePitch (y, fs, noteHz (64), 0.6, 1.9);
        std::printf ("  accord C4-E4-G4, voix E4 +60 c -> E4 %+5.2f c\n", s.meanCents);
        check (std::abs (s.meanCents) < 1.5, "MIDI : accord -> note tenue la plus proche");
    }
}

//==================================================================================================================
static void testTransition()
{
    std::printf ("\n[3] Transition (Flex) : glissando A3 -> C4 en 150 ms puis tenue, Retune 0 (hard-tune), chromatique\n");
    const double fs = 48000;
    auto f0 = [] (double t) { const double m = t < 0.5 ? 57.0 : t < 0.65 ? 57.0 + 3.0 * (t - 0.5) / 0.15 : 60.0; return noteHz (m + 0.1); };
    VoiceSpec vs; vs.jitter = 0.0; vs.shimmerDb = 0.0;
    auto x = makeRealVoice (fs, 1.5, f0, one, none, vs);
    for (double trans : { 0.0, 120.0 })
    {
        auto p = hardTune(); p.transitionMs = trans;
        auto e = makeEngine (fs, 1, TrackingMode::Live, p);
        std::vector<PitchFrame> frames;
        auto y = run (*e, { x }, fs, {}, {}, &frames)[0];
        double glideFollow = 0, settled = 0; int ng = 0;
        for (const auto& f : frames)
        {
            if (! f.voiced) continue;
            const double t = (double) f.samplePosition / fs;
            if (t > 0.52 && t < 0.63) { glideFollow = std::max (glideFollow, (double) std::abs (f.outputMidi - f.inputMidi) * 100.0); ++ng; }
        }
        const auto end = measurePitch (y, fs, noteHz (60), 0.65 + trans / 1000.0 + 0.06, 1.45);
        settled = end.meanCents;
        std::printf ("  Transition %3.0f ms : écart sortie / voix pendant le glissé max %5.1f c · tenue C4 %+5.2f c (max %.1f)\n",
                     trans, glideFollow, settled, end.maxCents);
        check (ng > 20, "Transition : trames dans le glissé");
        if (trans == 0.0) check (glideFollow > 30.0, "Transition 0 : escalier (hard-tune de l'étape 4)");
        else              check (glideFollow < 15.0, "Transition 120 ms : le glissé passe (sortie ≈ voix)");
        check (std::abs (settled) < 1.0 && end.maxCents < 4.0, "Transition : la note est verrouillée après la transition");
    }
}

//==================================================================================================================
namespace
{
/** Voyelle source-filtre (modèle de production de la parole) : source glottique en 1/k, conduit vocal = cascade de
    résonateurs tous-pôles (F1 650 / 80 Hz, F2 1150 / 90, F3 2500 / 120, F4 3400 / 180, F5 4500 / 250). */
const std::array<std::pair<double, double>, 5> kTract { { { 650, 80 }, { 1150, 90 }, { 2500, 120 }, { 3400, 180 }, { 4500, 250 } } };

double tractGain (double f, double fs)
{
    double g = 1.0;
    for (auto [fk, bw] : kTract)
    {
        const double r = std::exp (-kPi * bw / fs), th = 2 * kPi * fk / fs, w = 2 * kPi * f / fs;
        const std::complex<double> z = std::polar (1.0, -w);
        g /= std::abs (1.0 - 2.0 * r * std::cos (th) * z + r * r * z * z);
    }
    return g;
}

std::vector<double> sourceFilterVowel (double fs, double seconds, double f0)
{
    const size_t n = (size_t) (fs * seconds);
    std::vector<double> y (n, 0.0);
    const int K = (int) (std::min (18000.0, 0.45 * fs) / f0);
    const double ref = tractGain (f0, fs);
    for (int k = 1; k <= K; ++k)
    {
        const double a = 0.05 * tractGain (k * f0, fs) / ref / k;
        const double w = 2 * kPi * k * f0 / fs, ph = 0.7 * k * k;
        for (size_t i = 0; i < n; ++i) y[i] += a * std::sin (w * (double) i + ph);
    }
    return y;
}

/** Déformation idéale de l'enveloppe : E(w⁻¹(f)) / E(f), en dB. */
double idealThroatDb (double f, double tau, double fs)
{
    double lo = 0, hi = 30000;                       // w est monotone : inversion par dichotomie
    for (int it = 0; it < 80; ++it) { const double mid = 0.5 * (lo + hi); (ThroatModel::warp (mid, tau) < f ? lo : hi) = mid; }
    return 20 * std::log10 (tractGain (0.5 * (lo + hi), fs) / tractGain (f, fs));
}
} // namespace

static void testThroat()
{
    std::printf ("\n[4] Throat : voyelle source-filtre (F1 650, F2 1150, F3 2500, F4 3400, F5 4500 Hz), F0 196 Hz juste,\n"
                 "    + sons purs à 7, 9 et 12 kHz (−45 dB). Déplacement des formants mesuré sur les harmoniques ; écart à la\n"
                 "    déformation idéale E(w⁻¹ f) / E(f) ; sons aigus inchangés.\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
            for (double tau : { 0.8, 0.9, 1.1, 1.2 })
            {
                const double f0 = 196.0;
                auto p = hardTune(); p.throat = tau;
                auto e = makeEngine (fs, 1, mode, p);
                auto x = sourceFilterVowel (fs, 1.4, f0);
                for (double ft : { 7000.0, 9000.0, 12000.0 })
                    for (size_t i = 0; i < x.size(); ++i) x[i] += 0.0056 * std::sin (2 * kPi * ft * (double) i / fs);
                auto y = run (*e, { x }, fs)[0];
                const size_t lat = (size_t) e->getLatencySamples(), len = (size_t) (0.3 * fs);
                const size_t a0 = (size_t) (0.9 * fs);
                double sq = 0, h1 = 0; int nh = 0;
                for (int k = 1; k * f0 <= 3300; ++k)
                {
                    const double f = k * f0;
                    const double d = toneDb (y, fs, f, a0 + lat, len) - toneDb (x, fs, f, a0, len) - idealThroatDb (f, tau, fs);
                    if (k == 1) h1 = d;
                    sq += d * d; ++nh;
                }
                const double rms = std::sqrt (sq / nh);
                double hf = 0;
                for (double ft : { 7000.0, 9000.0, 12000.0 })
                    hf = std::max (hf, std::abs (toneDb (y, fs, ft, a0 + lat, len) - toneDb (x, fs, ft, a0, len)));
                const double shift = formantShiftFromHarmonics (x, f0, 0.9, y, f0, 0.9 + (double) lat / fs, fs);
                const double expect = -12.0 * std::log2 (tau);
                std::printf ("  %5.1f k %s τ %3.0f %% : formants %+5.2f dt (attendu %+5.2f) · enveloppe ≤ 3,3 kHz : écart RMS %.2f dB, "
                             "fondamentale %+.2f dB · 7 / 9 / 12 kHz ±%.2f dB\n", fs / 1000, modeName (mode), tau * 100, shift, expect, rms, h1, hf);
                check (std::abs (shift - expect) < 0.4, "Throat : formants déplacés de −12·log2(τ) dt (±0,4)");
                check (rms < 3.5, "Throat : enveloppe ≤ 3,3 kHz à 3,5 dB RMS de la déformation idéale");
                check (std::abs (h1) < 1.0, "Throat : graves conservés (fondamentale à ±1 dB de l'idéal)");
                check (hf < 0.5, "Throat : haut du spectre (7, 9, 12 kHz) intact à ±0,5 dB");
            }
}

//==================================================================================================================
static void testDoubler()
{
    std::printf ("\n[5] Doubler 100 %%, stéréo : voix doublées extraites (sortie − moteur sans doubleur), hauteur et retard\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            const double f0 = 220.0;
            VoiceSpec vs; vs.jitter = 0.0; vs.shimmerDb = 0.0; vs.breathDb = -70.0;
            auto x = makeRealVoice (fs, 2.0, [] (double) { return 220.0; }, [] (double t) { return t < 0.5 ? 0.0 : 1.0; }, none, vs);
            auto pd = hardTune(); pd.doubler = 1.0;
            auto ed = makeEngine (fs, 2, mode, pd);
            auto er = makeEngine (fs, 2, mode, hardTune());
            auto yd = run (*ed, { x, x }, fs);
            auto yr = run (*er, { x, x }, fs);
            const double g = 0.7071, norm = 1.0 / std::sqrt (1.0 + g * g);
            std::vector<double> dl (x.size()), dr (x.size());
            for (size_t i = 0; i < x.size(); ++i)
            {
                dl[i] = (yd[0][i] / norm - yr[0][i]) / g;
                dr[i] = (yd[1][i] / norm - yr[1][i]) / g;
            }
            const double lat = (double) er->getLatencySamples() / fs;
            const auto pl = measurePitch (dl, fs, f0, 1.0 + lat, 1.9 + lat);
            const auto pr = measurePitch (dr, fs, f0, 1.0 + lat, 1.9 + lat);
            // retard : attaque (enveloppe à −20 dB du régime) de la voix doublée vs voix principale
            auto onset = [&] (const std::vector<double>& s)
            {
                const size_t w = (size_t) (0.002 * fs);
                const double ref = rmsDb (s, (size_t) ((1.0 + lat) * fs), (size_t) ((1.5 + lat) * fs));
                for (size_t i = (size_t) (0.3 * fs); i + w < s.size(); i += w / 4)
                    if (rmsDb (s, i, i + w) > ref - 20.0) return (double) i / fs;
                return 0.0;
            };
            const double t0 = onset (yr[0]), tl = onset (dl), tr = onset (dr);
            std::printf ("  %5.1f k %s : gauche %+5.2f c, +%4.1f ms · droite %+5.2f c, +%4.1f ms\n",
                         fs / 1000, modeName (mode), pl.meanCents, 1000 * (tl - t0), pr.meanCents, 1000 * (tr - t0));
            check (std::abs (pl.meanCents + 7.0) < 1.0 && std::abs (pr.meanCents - 7.0) < 1.0, "Doubler : ±7 c");
            check (std::abs (1000 * (tl - t0) - 9.0) < 2.5 && std::abs (1000 * (tr - t0) - 14.0) < 2.5, "Doubler : +9 ms / +14 ms");
        }

    // Mono : sans effet
    {
        const double fs = 48000;
        auto x = makeRealVoice (fs, 1.0, [] (double) { return 230.0; }, one, none);
        auto p = hardTune(); p.doubler = 1.0;
        auto a = run (*makeEngine (fs, 1, TrackingMode::Live, p), { x }, fs)[0];
        auto b = run (*makeEngine (fs, 1, TrackingMode::Live, hardTune()), { x }, fs)[0];
        check (a == b, "Doubler : sortie mono inchangée");
    }
}

//==================================================================================================================
static void testTransparency()
{
    std::printf ("\n[6] Transparence : Throat 100 %% et Doubler 0 %% = chemin de l'étape 4, au bit près\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 352800.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            auto x = makeRealVoice (fs, 1.6, [] (double t) { return 200.0 * std::exp2 (0.4 * std::sin (2 * kPi * 0.8 * t) / 12.0); }, one, none);
            std::vector<double> r (x.size());
            for (size_t i = 0; i < x.size(); ++i) r[i] = 0.8 * x[i];
            auto ref = run (*makeEngine (fs, 2, mode, hardTune (ScaleType::Major)), { x, r }, fs);
            // Réglages explicites (100 %, 0 %) : même sortie au bit près
            auto p = hardTune (ScaleType::Major); p.throat = 1.0; p.doubler = 0.0; p.transitionMs = 0.0; p.midiTarget = false;
            auto same = run (*makeEngine (fs, 2, mode, p), { x, r }, fs);
            // Excursion (Throat 120 %, Doubler 100 % pendant 0,5 s) puis retour : aucun résidu une fois revenu
            auto e = makeEngine (fs, 2, mode, hardTune (ScaleType::Major));
            auto back = run (*e, { x, r }, fs, {}, [&] (double t, AuraPitchEngine& eng)
            {
                auto q = hardTune (ScaleType::Major);
                q.mode = mode;
                if (t < 0.5) { q.throat = 1.2; q.doubler = 1.0; }
                eng.setParameters (q);
            });
            const size_t from = (size_t) (0.75 * fs);
            bool identical = true;
            for (int ch = 0; ch < 2; ++ch)
                for (size_t i = from; i < x.size(); ++i)
                    identical = identical && back[(size_t) ch][i] == ref[(size_t) ch][i];
            std::printf ("  %5.1f k %s : réglages neutres %s · après excursion %s\n", fs / 1000, modeName (mode),
                         same == ref ? "identiques" : "DIFFÉRENTS", identical ? "identiques" : "DIFFÉRENTS");
            check (same == ref, "Throat 100 % / Doubler 0 % : sortie identique au bit près");
            check (identical, "après une excursion Throat / Doubler : retour au bit près");
        }
}

//==================================================================================================================
static void testCpu()
{
    std::printf ("\n[7] Charge CPU, stéréo, voix corrigée, Throat 115 %% + Doubler 60 %% (vs réglages neutres)\n");
    for (double fs : { 48000.0, 96000.0, 192000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            auto x = makeRealVoice (fs, 3.0, [] (double t) { return 205.0 * std::exp2 (0.3 * std::sin (2 * kPi * 5.0 * t) / 12.0); }, one, none);
            auto measure = [&] (AuraPitchParams p)
            {
                auto e = makeEngine (fs, 2, mode, p);
                const auto t0 = std::chrono::steady_clock::now();
                run (*e, { x, x }, fs, {}, {}, nullptr, 512);
                return 100.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 3.0;
            };
            auto p = hardTune (ScaleType::Major);
            const double base = measure (p);
            p.throat = 1.15; p.doubler = 0.6;
            const double full = measure (p);
            std::printf ("  %5.1f k %s : %5.1f %% -> %5.1f %%\n", fs / 1000, modeName (mode), base, full);
            check (full < 35.0, "charge < 35 % d'un cœur avec Throat + Doubler (runners CI ≈ 2× plus lents)");
        }
}

//==================================================================================================================
static void testRealtime()
{
    std::printf ("\n[8] Temps réel : toutes les fonctions 5A actives (Throat, Doubler, Transition, MIDI, Auto-Key), automation\n"
                 "    à chaque bloc, Live <-> Studio : allocations dans process() / setParameters() / addMidiEvent()\n");
    for (double fs : { 44100.0, 96000.0, 384000.0 })
    {
        auto e = makeEngine (fs, 2, TrackingMode::Live);
        auto x = melody (fs, { { 57, 0.4 }, { 60, 0.3 }, { 64, 0.5 }, { 62, 0.4 } }, 5);
        std::vector<double> r (x);
        e->startKeyDetection();
        const int block = 333;
        gAllocations = 0;
        gCountAllocations = true;
        int k = 0;
        for (size_t pos = 0; pos < x.size(); pos += (size_t) block, ++k)
        {
            auto p = hardTune();
            p.throat = 0.9 + 0.2 * (k % 7) / 6.0; p.doubler = (k % 5) / 4.0; p.transitionMs = 40.0 * (k % 3);
            p.midiTarget = k % 2 == 0; p.mode = (k / 40) % 2 == 0 ? TrackingMode::Live : TrackingMode::Studio;
            e->setParameters (p);
            e->addMidiEvent ({ k % 4 == 0 ? MidiTargetEvent::NoteOn : MidiTargetEvent::NoteOff, 55 + k % 12, 0.0 }, 17);
            e->addMidiEvent ({ MidiTargetEvent::PitchBend, 0, std::sin (k * 0.3) }, 100);
            double* io[2] = { x.data() + pos, r.data() + pos };
            e->process (io, 2, (int) std::min<size_t> ((size_t) block, x.size() - pos));
        }
        gCountAllocations = false;
        bool finite = true;
        for (double v : x) finite = finite && std::isfinite (v);
        std::printf ("  %5.1f k : %ld allocation(s), sortie %s\n", fs / 1000, gAllocations.load(), finite ? "finie" : "NaN / Inf");
        check (gAllocations.load() == 0 && finite, "aucune allocation sur le thread audio, sortie finie");
    }
}

int main(int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&] (const char* k) { return only.empty() || only == k; };
    if (want ("key")) testAutoKey();
    if (want ("midi")) testMidiTarget();
    if (want ("trans")) testTransition();
    if (want ("throat")) testThroat();
    if (want ("dbl")) testDoubler();
    if (want ("transp")) testTransparency();
    if (want ("cpu")) testCpu();
    if (want ("rt")) testRealtime();
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

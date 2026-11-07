// AuraPitch — banc d'essai des modules DSP de l'étape 1 (sans JUCE).
//   ScaleQuantizer / CorrectionCurve, PitchDetector (44,1 … 384 kHz), LockFreePitchFifo.
// Code de sortie 0 si toutes les vérifications passent.
#include "../Source/DSP/ScaleQuantizer.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/LockFreePitchFifo.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>
#include <vector>

//--- compteur d'allocations (thread courant) ----------------------------------------------------------------------
static std::atomic<long> gAllocations { 0 };
static thread_local bool gCountAllocations = false;
void* operator new (std::size_t n)
{
    if (gCountAllocations) gAllocations.fetch_add (1);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

using namespace aura::pitch;

static int gChecks = 0, gFailures = 0;
static void check (bool ok, const char* what)
{
    ++gChecks;
    if (! ok) { ++gFailures; std::printf ("  ÉCHEC : %s\n", what); }
}

constexpr double kPi = 3.14159265358979323846;

//--- signaux ------------------------------------------------------------------------------------------------------
/** Voyelle /a/ synthétique : harmoniques en 1/k² (source glottique) × 3 formants (700, 1220, 2600 Hz).
    f0 (t) arbitraire (vibrato) : la phase est intégrée, chaque harmonique vaut sin(k·Φ) (récurrence de Tchebychev). */
template <typename F0Fn>
static std::vector<double> makeVowel (double fs, double seconds, F0Fn f0At, double amplitude = 0.5, bool sawtooth = false)
{
    const int n = (int) (fs * seconds);
    std::vector<double> out ((size_t) n);
    const double fMax = std::min (16000.0, 0.45 * fs);
    auto formant = [] (double f)
    {
        auto res = [f] (double fc, double bw) { const double d = (f - fc) / (0.5 * bw); return 1.0 / std::sqrt (1.0 + d * d); };
        return 0.15 + res (700, 110) + 0.6 * res (1220, 120) + 0.3 * res (2600, 160);
    };
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f0 = f0At ((double) i / fs);
        phase += 2.0 * kPi * f0 / fs;
        if (phase > 2.0 * kPi * 1.0e6) phase = std::fmod (phase, 2.0 * kPi);
        const int K = std::min (80, (int) (fMax / f0));
        const double c2 = 2.0 * std::cos (phase);
        double sPrev = 0.0, s = std::sin (phase), acc = 0.0, norm = 0.0;
        for (int k = 1; k <= K; ++k)
        {
            const double a = sawtooth ? 1.0 / k : formant (k * f0) / ((double) k * k);
            acc += a * s; norm += a;
            const double sNext = c2 * s - sPrev; sPrev = s; s = sNext;
        }
        out[(size_t) i] = amplitude * acc / norm * 1.6;
    }
    return out;
}

static std::vector<double> makeNoise (double fs, double seconds, double lowHz, double highHz, double rmsTarget, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    const int n = (int) (fs * seconds);
    std::vector<double> out ((size_t) n);
    // Passe-haut / passe-bas du 2e ordre (RBJ), 2 cellules chacun.
    struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    auto design = [fs] (double fc, bool hp)
    {
        const double w = 2.0 * kPi * std::min (fc, 0.45 * fs) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * 0.7071);
        const double a0 = 1.0 + al;
        Bq q;
        if (hp) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
        else    { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0;  q.b2 = q.b0; }
        q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
        return q;
    };
    Bq h1 = design (lowHz, true), h2 = h1, l1 = design (highHz, false), l2 = l1;
    double e = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double x = g (rng);
        if (lowHz > 0)  x = h2.run (h1.run (x));
        if (highHz > 0) x = l2.run (l1.run (x));
        out[(size_t) i] = x; e += x * x;
    }
    const double k = rmsTarget / std::sqrt (e / n);
    for (auto& x : out) x *= k;
    return out;
}

struct TrackStats { double medianCents = 0, maxCents = 0, rmsCents = 0, voicedRatio = 0; int frames = 0; };

/** Passe le signal par blocs de 512 dans le détecteur ; compare chaque estimation à la F0 vraie à frameCentre. */
template <typename F0Fn>
static TrackStats track (PitchDetector& det, const std::vector<double>& sig, double fs, F0Fn trueF0, double skipSeconds = 0.15)
{
    det.reset();
    std::vector<double> errs;
    int frames = 0, voiced = 0;
    for (size_t pos = 0; pos < sig.size(); pos += 512)
    {
        const int n = (int) std::min<size_t> (512, sig.size() - pos);
        det.process (sig.data() + pos, n, [&] (const PitchEstimate& e, int)
        {
            const double t = (double) e.frameCentre / fs;
            if (t < skipSeconds) return;
            ++frames;
            if (! e.voiced) return;
            ++voiced;
            errs.push_back (std::abs (1200.0 * std::log2 (e.frequencyHz / trueF0 (t))));
        });
    }
    TrackStats s;
    s.frames = frames;
    s.voicedRatio = frames ? (double) voiced / frames : 0.0;
    if (! errs.empty())
    {
        double sq = 0;
        for (double x : errs) { s.maxCents = std::max (s.maxCents, x); sq += x * x; }
        s.rmsCents = std::sqrt (sq / (double) errs.size());
        std::nth_element (errs.begin(), errs.begin() + errs.size() / 2, errs.end());
        s.medianCents = errs[errs.size() / 2];
    }
    return s;
}

static double voicedRatioOf (PitchDetector& det, const std::vector<double>& sig)
{
    det.reset();
    int frames = 0, voiced = 0;
    det.process (sig.data(), (int) sig.size(), [&] (const PitchEstimate& e, int) { ++frames; voiced += e.voiced ? 1 : 0; });
    return frames ? (double) voiced / frames : 0.0;
}

//==================================================================================================================
static void testQuantizer()
{
    std::printf ("\n[1] ScaleQuantizer\n");
    ScaleQuantizer q;
    q.setHysteresisCents (0);

    check (ScaleQuantizer::rotate (ScaleQuantizer::intervalsOf (ScaleType::Major), 0) == 0b101010110101, "C majeur");
    // G majeur = G A B C D E F# -> pc 7 9 11 0 2 4 6
    {
        const auto m = ScaleQuantizer::rotate (ScaleQuantizer::intervalsOf (ScaleType::Major), 7);
        const int expect[] = { 0, 2, 4, 6, 7, 9, 11 };
        ScaleQuantizer::Mask e = 0; for (int pc : expect) e |= (ScaleQuantizer::Mask) (1u << pc);
        check (m == e, "G majeur = G A B C D E F#");
    }

    q.setKey (0); q.setScale (ScaleType::Major);
    check (q.quantize (61.4).targetNote == 62, "C maj : 61,4 -> D (62)");
    check (q.quantize (60.9).targetNote == 60, "C maj : 60,9 -> C (60)");
    check (q.quantize (64.6).targetNote == 65, "C maj : 64,6 -> F (65)");
    check (std::abs (q.quantize (69.25).deviationCents - 25.0) < 1e-9, "déviation +25 cents");

    q.setScale (ScaleType::Minor); q.setKey (9);                // A mineur = C majeur
    check (q.getTargetMask() == ScaleQuantizer::rotate (ScaleQuantizer::intervalsOf (ScaleType::Major), 0), "A mineur = C majeur");
    q.setScale (ScaleType::Pentatonic); q.setKey (0);           // C D E G A
    check (q.quantize (65.2).targetNote == 64, "C penta : 65,2 -> E");
    check (q.quantize (65.6).targetNote == 67, "C penta : 65,6 -> G");

    // Hystérésis 12 cents : 60,55 reste sur 60 si on vient de 60, passe à 61 au-delà de 60,56.
    q.setScale (ScaleType::Chromatic); q.setHysteresisCents (12); q.reset();
    check (q.quantize (60.20).targetNote == 60, "hyst : départ 60");
    check (q.quantize (60.55).targetNote == 60, "hyst : 60,55 reste 60");
    check (q.quantize (60.57).targetNote == 61, "hyst : 60,57 passe à 61");
    check (q.quantize (60.45).targetNote == 61, "hyst : 60,45 reste 61");
    check (q.quantize (60.43).targetNote == 60, "hyst : 60,43 revient à 60");

    // Référence 432 Hz : 432 Hz est exactement A4.
    q.setReferenceA4 (432.0); q.reset();
    auto r = q.quantizeHz (432.0);
    check (r.targetNote == 69 && std::abs (r.deviationCents) < 1e-9, "A4 = 432 Hz");
    q.setReferenceA4 (400.0);
    check (q.getReferenceA4() == 415.0, "référence bornée à 415 Hz");
    q.setReferenceA4 (440.0);

    // Custom : C active, D bypass, le reste retiré.
    std::array<NoteState, 12> st; st.fill (NoteState::Removed);
    st[0] = NoteState::Active; st[2] = NoteState::Bypass;
    q.setCustomStates (st); q.setScale (ScaleType::Custom); q.reset();
    check (q.quantize (61.9).bypassed, "Custom : près de D (bypass) -> aucune correction");
    check (q.quantize (64.0).targetNote == 60, "Custom : E -> C (seule note active)");
    st.fill (NoteState::Removed); q.setCustomStates (st);
    check (q.quantize (64.0).bypassed, "Custom vide -> bypass");

    // Copie de la gamme courante dans le clavier
    q.setScale (ScaleType::Major); q.setKey (2); q.copyCurrentScaleToCustom(); q.setScale (ScaleType::Custom);
    check (q.getTargetMask() == ScaleQuantizer::rotate (ScaleQuantizer::intervalsOf (ScaleType::Major), 2), "copie D majeur -> Custom");
    check (q.quantize (std::nan ("")).bypassed, "NaN -> bypass");
}

static void testCorrectionCurve()
{
    std::printf ("\n[2] CorrectionCurve (Retune Speed, Humanize)\n");
    check (CorrectionCurve::retuneMsFromNormalised (0) == 0.0, "retune 0 -> 0 ms");
    check (std::abs (CorrectionCurve::retuneMsFromNormalised (1) - 400.0) < 1e-9, "retune 1 -> 400 ms");
    check (std::abs (CorrectionCurve::normalisedFromRetuneMs (CorrectionCurve::retuneMsFromNormalised (0.37)) - 0.37) < 1e-12, "courbe inversible");
    std::printf ("  courbe : 0,25 -> %.1f ms · 0,5 -> %.1f ms · 0,75 -> %.1f ms\n",
                 CorrectionCurve::retuneMsFromNormalised (0.25), CorrectionCurve::retuneMsFromNormalised (0.5),
                 CorrectionCurve::retuneMsFromNormalised (0.75));

    ScaleQuantizer q; q.setScale (ScaleType::Major); q.setKey (0);
    const double dt = 0.0015;

    // Hard-tune : sortie = cible à chaque trame, vibrato écrasé.
    auto runVibrato = [&] (double retuneMs, double humanize, double& depthOut, double& meanErr)
    {
        CorrectionCurve c; c.setRetuneMs (retuneMs); c.setHumanize (humanize); q.reset();
        double lo = 1e9, hi = -1e9, sum = 0; int n = 0;
        for (int i = 0; i < 1500; ++i)
        {
            const double t = i * dt;
            const double in = 60.30 + 0.5 * std::sin (2 * kPi * 5.5 * t);
            const double out = in + c.process (q.quantize (in), true, dt);
            if (t > 1.0) { lo = std::min (lo, out); hi = std::max (hi, out); sum += out; ++n; }
        }
        depthOut = 0.5 * (hi - lo);
        meanErr  = sum / n - 60.0;
    };
    double depth, mean;
    runVibrato (0, 0, depth, mean);
    std::printf ("  hard-tune      : vibrato ±50 c -> ±%.3f c, note moyenne %+.3f c\n", depth * 100, mean * 100);
    check (depth < 1e-9 && std::abs (mean) < 1e-9, "retune 0 ms : sortie exactement sur la note");
    runVibrato (0, 1.0, depth, mean);
    std::printf ("  humanize 100 %% : vibrato ±50 c -> ±%.1f c, note moyenne %+.2f c\n", depth * 100, mean * 100);
    check (depth > 0.40 && std::abs (mean) < 0.05, "humanize : vibrato conservé (> 80 %), note moyenne juste (< 5 c)");

    // Attaque : retune 50 ms -> 95 % de la correction en ~50 ms ; humanize ne ralentit pas l'attaque.
    for (double hz : { 0.0, 1.0 })
    {
        CorrectionCurve c; c.setRetuneMs (50); c.setHumanize (hz); q.reset();
        double t95 = -1;
        for (int i = 0; i < 400; ++i)
        {
            const double shift = c.process (q.quantize (60.4), true, dt);
            if (t95 < 0 && shift <= -0.4 * 0.95) t95 = (i + 1) * dt;
        }
        std::printf ("  retune 50 ms, humanize %.0f %% : 95 %% de la correction en %.1f ms\n", hz * 100, t95 * 1000);
        check (t95 > 0.040 && t95 < 0.060, "retune 50 ms : attaque corrigée en 50 ms ±10");
    }

    // Transposition + fine tune, retour à 0 sur silence (après le maintien de 30 ms).
    CorrectionCurve c; c.setTranspose (-12); c.setFineTuneCents (25); q.reset();
    c.process (q.quantize (60.4), true, dt);
    check (std::abs (c.getShiftSemitones() - (-0.4 - 12 + 0.25)) < 1e-9, "transpose −12 + fine +25 c");
    ScaleQuantizer::Result none;
    for (int i = 0; i < 15; ++i) c.process (none, false, dt);
    check (std::abs (c.getCorrectionSemitones() + 0.4) < 1e-12, "trou non voisé de 22 ms : correction maintenue");
    for (int i = 0; i < 100; ++i) c.process (none, false, dt);
    check (std::abs (c.getCorrectionSemitones()) < 0.4 * 0.01, "silence : correction relâchée");
}

static void testDecimator()
{
    std::printf ("\n[3] Demi-bande ÷2\n");
    for (double f : { 0.10, 0.18, 0.32, 0.40, 0.48 })
    {
        detail::HalfBandDecimator d; d.prepare();
        double sq = 0; int n = 0;
        for (int i = 0; i < 40000; ++i)
        {
            double y;
            if (d.push (std::sin (2 * kPi * f * i), y) && i > 200) { sq += y * y; ++n; }
        }
        const double peak = std::sqrt (2.0 * sq / n);          // amplitude (RMS × √2)
        std::printf ("  %.2f fs : %8.2f dB\n", f, 20 * std::log10 (peak + 1e-30));
        if (f <= 0.18) check (std::abs (20 * std::log10 (peak)) < 0.001, "bande passante plate (< 0,001 dB)");
        else           check (20 * std::log10 (peak + 1e-30) < -85.0, "réjection > 85 dB dès 0,32 fs");
    }
}

static void testDetectorAccuracy()
{
    std::printf ("\n[4] Précision F0 (voyelle /a/ synthétique, InputType Instrument), erreur max | médiane en cents\n");
    const double rates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };
    const double freqs[] = { 55.0, 82.41, 110.0, 146.83, 196.0, 261.63, 392.0, 523.25, 783.99, 1046.5, 1174.66 };

    std::printf ("  %-9s", "fs \\ F0");
    for (double f : freqs) std::printf ("%9.0f", f);
    std::printf ("\n");

    double worst = 0;
    for (double fs : rates)
    {
        PitchDetector det; det.prepare (fs); det.setInputType (InputType::Instrument);
        std::printf ("  %-9.1f", fs / 1000);
        for (double f0 : freqs)
        {
            auto sig = makeVowel (fs, 0.5, [f0] (double) { return f0; });
            auto s = track (det, sig, fs, [f0] (double) { return f0; });
            std::printf ("%5.2f|%-3.2f", s.maxCents, s.medianCents);
            worst = std::max (worst, s.maxCents);
            check (s.voicedRatio > 0.99, "voyelle détectée voisée (> 99 % des trames)");
            check (s.maxCents < 1.0, "erreur < 1 cent");
        }
        std::printf ("\n");
    }
    std::printf ("  pire erreur, 88 cas : %.3f cent\n", worst);

    for (double fs : rates)
    {
        PitchDetector a; a.prepare (fs);
        std::printf ("  fs %6.1f k -> analyse %5.0f Hz, raffinement %5.0f Hz (÷%d)\n",
                     fs / 1000, a.getAnalysisRate(), a.getRefineRate(), a.getDecimationFactor());
    }
}

static void testDetectorRobustness()
{
    std::printf ("\n[5] Robustesse\n");
    for (double fs : { 48000.0, 384000.0 })
    {
        PitchDetector det; det.prepare (fs); det.setInputType (InputType::AltoTenor);

        // Vibrato ±50 cents à 5,5 Hz : l'instant frameCentre doit être exact (sinon l'erreur suit le vibrato).
        auto f0v = [] (double t) { return 220.0 * std::exp2 (0.5 / 12.0 * std::sin (2 * kPi * 5.5 * t)); };
        auto v = track (det, makeVowel (fs, 1.0, f0v), fs, f0v);
        std::printf ("  %5.1f k  vibrato ±50 c : erreur RMS %.2f c, max %.2f c (latence d'analyse %.1f ms)\n",
                     fs / 1000, v.rmsCents, v.maxCents, det.getAnalysisLatencySamples() / fs * 1000);
        check (v.rmsCents < 2.0 && v.voicedRatio > 0.98, "vibrato suivi (RMS < 2 c)");

        // Glissando d'une octave en 1 s
        auto f0g = [] (double t) { return 150.0 * std::exp2 (std::min (t, 1.0)); };
        auto g = track (det, makeVowel (fs, 1.0, f0g), fs, f0g);
        std::printf ("            glissando 150 -> 300 Hz : erreur RMS %.2f c\n", g.rmsCents);
        check (g.rmsCents < 3.0 && g.voicedRatio > 0.98, "glissando suivi (RMS < 3 c)");

        // Sifflante 4–10 kHz, souffle large bande, silence : jamais voisé.
        const double sib = voicedRatioOf (det, makeNoise (fs, 0.6, 4000, 10000, 0.1, 1));
        const double wht = voicedRatioOf (det, makeNoise (fs, 0.6, 0, 0, 0.1, 2));
        const double brt = voicedRatioOf (det, makeNoise (fs, 0.6, 100, 0, 0.1, 3));
        std::vector<double> zero ((size_t) (fs * 0.5), 0.0);
        const double sil = voicedRatioOf (det, zero);
        std::printf ("            trames voisées : sifflante %.1f %% · bruit blanc %.1f %% · souffle %.1f %% · silence %.1f %%\n",
                     sib * 100, wht * 100, brt * 100, sil * 100);
        check (sib < 0.01, "sifflante 's' non voisée");
        check (wht < 0.01, "bruit blanc non voisé");
        check (brt < 0.01, "souffle non voisé");
        check (sil == 0.0, "silence non voisé");

        // Voyelle + souffle à 20 dB sous la voix
        auto voice = makeVowel (fs, 0.8, [] (double) { return 196.0; }, 0.3);
        double ev = 0; for (double x : voice) ev += x * x;
        auto breath = makeNoise (fs, 0.8, 200, 12000, 0.1 * std::sqrt (ev / (double) voice.size()), 4);
        for (size_t i = 0; i < voice.size(); ++i) voice[i] += breath[i];
        auto nz = track (det, voice, fs, [] (double) { return 196.0; });
        std::printf ("            voyelle + souffle (SNR 20 dB) : %.1f %% voisé, médiane %.2f c, max %.2f c\n",
                     nz.voicedRatio * 100, nz.medianCents, nz.maxCents);
        check (nz.voicedRatio > 0.95 && nz.medianCents < 1.0, "voyelle bruitée : voisée, médiane < 1 c");

        // Synthé en dents de scie (spectre très brillant) : reste voisé grâce à sa clarté.
        det.setInputType (InputType::Instrument);
        auto saw = track (det, makeVowel (fs, 0.5, [] (double) { return 1000.0; }, 0.5, true), fs, [] (double) { return 1000.0; });
        std::printf ("            dents de scie 1 kHz : %.1f %% voisé, max %.2f c\n", saw.voicedRatio * 100, saw.maxCents);
        check (saw.voicedRatio > 0.99 && saw.maxCents < 1.0, "dents de scie brillantes : voisées et justes");
    }

    // Erreurs d'octave : basse masculine et soprano avec leur InputType.
    PitchDetector det; det.prepare (96000);
    det.setInputType (InputType::LowMale);
    auto lm = track (det, makeVowel (96000, 0.6, [] (double) { return 65.41; }), 96000, [] (double) { return 65.41; });
    det.setInputType (InputType::Soprano);
    auto sp = track (det, makeVowel (96000, 0.6, [] (double) { return 987.77; }), 96000, [] (double) { return 987.77; });
    std::printf ("  96 k  Low Male C2 : max %.2f c · Soprano B5 : max %.2f c\n", lm.maxCents, sp.maxCents);
    check (lm.maxCents < 1.0 && lm.voicedRatio > 0.99, "Low Male C2 sans erreur d'octave");
    check (sp.maxCents < 1.0 && sp.voicedRatio > 0.99, "Soprano B5 sans erreur d'octave");

    // Entrée float = entrée double
    {
        auto sig = makeVowel (48000, 0.3, [] (double) { return 330.0; });
        std::vector<float> sf (sig.begin(), sig.end());
        PitchDetector a, b; a.prepare (48000); b.prepare (48000);
        double fa = 0, fb = 0;
        a.process (sig.data(), (int) sig.size(), [&] (const PitchEstimate& e, int) { fa = e.frequencyHz; });
        b.process (sf.data(), (int) sf.size(), [&] (const PitchEstimate& e, int) { fb = e.frequencyHz; });
        check (std::abs (1200 * std::log2 (fa / fb)) < 0.01, "entrée float = entrée double (< 0,01 c)");
    }
    // NaN en entrée : ignoré
    {
        PitchDetector d; d.prepare (48000);
        auto sig = makeVowel (48000, 0.4, [] (double) { return 330.0; });
        sig[1000] = std::nan ("");
        d.process (sig.data(), (int) sig.size());
        check (d.getLatest().voiced && std::abs (1200 * std::log2 (d.getLatest().frequencyHz / 330.0)) < 1.0, "NaN en entrée sans effet durable");
    }
}

static void testDetectorRealtime()
{
    std::printf ("\n[6] Temps réel : allocations et charge CPU (InputType Instrument = pire cas, saut 1,5 ms)\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0, 352800.0, 384000.0 })
    {
        PitchDetector det; det.prepare (fs); det.setInputType (InputType::Instrument);
        auto sig = makeVowel (fs, 3.0, [] (double t) { return 110.0 * std::exp2 (t / 3.0); });
        std::vector<float> sf (sig.begin(), sig.end());

        int estimates = 0;
        gAllocations = 0; gCountAllocations = true;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos < sf.size(); pos += 256)
        {
            const int n = (int) std::min<size_t> (256, sf.size() - pos);
            det.process (sf.data() + pos, n, [&] (const PitchEstimate&, int) { ++estimates; });
            if ((pos / 256) % 100 == 50) det.setInputType ((pos / 256) % 200 == 50 ? InputType::LowMale : InputType::Instrument);
        }
        const auto t1 = std::chrono::steady_clock::now();
        gCountAllocations = false;

        const double secs = std::chrono::duration<double> (t1 - t0).count();
        std::printf ("  %5.1f kHz : %.2f %% d'un cœur, %d estimations/s, allocations %ld\n",
                     fs / 1000, secs / 3.0 * 100, estimates / 3, gAllocations.load());
        check (gAllocations.load() == 0, "0 allocation dans process() / setInputType()");
        check (secs / 3.0 < 0.05, "charge < 5 % d'un cœur");
    }
}

static void testFifo()
{
    std::printf ("\n[7] LockFreePitchFifo (producteur / consommateur concurrents)\n");
    static LockFreeSpscFifo<PitchFrame, 64> fifo;
    constexpr int kFrames = 2000000;
    std::atomic<bool> done { false };
    std::int64_t expected = 0; bool ordered = true; long received = 0;

    std::thread producer ([&]
    {
        gAllocations = 0; gCountAllocations = true;
        for (int i = 0; i < kFrames; ++i)
        {
            PitchFrame f; f.samplePosition = i;
            while (! fifo.push (f)) std::this_thread::yield();   // le vrai producteur abandonne ; ici on veut tout
        }
        gCountAllocations = false;
        done = true;
    });
    while (! done || fifo.getNumReady() > 0)
        fifo.drain ([&] (const PitchFrame& f) { ordered &= (f.samplePosition == expected++); ++received; });
    producer.join();
    check (ordered && received == kFrames, "2 000 000 trames reçues, dans l'ordre");
    check (gAllocations.load() == 0, "push sans allocation");

    LockFreeSpscFifo<PitchFrame, 4> small;
    PitchFrame f;
    for (int i = 0; i < 6; ++i) small.push (f);
    check (small.getNumReady() == 4 && small.getNumDropped() == 2, "file pleine : trames perdues comptées");
}

int main()
{
    std::printf ("AuraPitch — banc DSP étape 1\n");
    testQuantizer();
    testCorrectionCurve();
    testDecimator();
    testDetectorAccuracy();
    testDetectorRobustness();
    testDetectorRealtime();
    testFifo();
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

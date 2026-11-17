// AuraPitch — banc du lot 1 (cœur DSP) : transparence aux grands rapports, résidu Live sur vibrato fort, latences.
//
//   [1] transp    transmission harmonique par harmonique jusqu'à 19,5 kHz, de +40 c à +12 dt (Preserve OFF : la sortie
//                 doit être l'entrée transposée), voix stable ; puis voix réelle (jitter) par tiers d'octave
//   [2] vibrato   Live, Retune 0 : vibrato de ±30 / ±60 / ±100 c aplati, résidu crête et RMS mesuré sur la sortie
//   [3] latence   retard du détecteur par InputType, latences Live / Studio, temps d'accroche sur une attaque
//   [4] cpu       coût d'un moteur stéréo, Live et Studio
//   [6] alias     repliement d'une sinusoïde transposée au-delà de fs / 2 (noyau standard, et Ultra avec le lot 1)
//   Avec AURAPITCH_LOT1 (moteur du lot 1) :
//   [5] ultra     mode Ultra : mêmes mesures de transparence, latence inchangée, bascule à chaud sans clic
//
// Le banc tourne aussi sur le moteur d'avant le lot 1 (sans AURAPITCH_LOT1) : c'est la colonne « avant ».
// Argument facultatif : nom d'un test.
#ifndef AURAPITCH_LOT1
 #define AURAPITCH_LOT1 1          // 0 : compilé contre le moteur d'avant le lot 1 (colonne « avant »)
#endif
#include "VocalRealism.h"

#include "../Source/DSP/AuraPitchEngine.h"

#include <chrono>
#include <memory>
#include <string>

using namespace aura::pitch;
using namespace test;

static std::vector<std::vector<double>> runEngine (AuraPitchEngine& eng, std::vector<std::vector<double>> io, int block = 256)
{
    const size_t len = io[0].size();
    for (size_t pos = 0; pos < len; pos += (size_t) block)
    {
        double* p[2] = { io[0].data() + pos, io.size() > 1 ? io[1].data() + pos : nullptr };
        eng.process (p, (int) io.size(), (int) std::min<size_t> ((size_t) block, len - pos));
    }
    return io;
}

static const char* modeName (TrackingMode m) { return m == TrackingMode::Live ? "Live  " : "Studio"; }
static auto one  = [] (double) { return 1.0; };
static auto none = [] (double) { return 0.0; };

struct Setup
{
    TrackingMode mode = TrackingMode::Live;
    double transpose = 0.0;
    bool preserve = false;
    bool ultra = false;
    InputType type = InputType::AltoTenor;
    ScaleType scale = ScaleType::Chromatic;
};

static std::unique_ptr<AuraPitchEngine> makeEngine (double fs, int ch, const Setup& s)
{
    auto e = std::make_unique<AuraPitchEngine>();
    e->prepare (fs, ch);
    AuraPitchParams p;
    p.mode = s.mode; p.retuneMs = 0; p.scale = s.scale; p.key = 0; p.formantPreserve = s.preserve;
    p.transpose = s.transpose; p.inputType = s.type;
#if AURAPITCH_LOT1
    p.ultra = s.ultra;
#endif
    e->setParameters (p);
    e->reset();
    return e;
}

//==================================================================================================================
struct Flatness { double energy = 0, energyHz = 0, nominal = 0, cents = 0; };

/** Chaque harmonique de sortie (h·fout ≤ 19,5 kHz) comparée à l'harmonique d'entrée de même rang.
    energy  : pire écart d'énergie de la raie, mesurée à la fréquence que la sortie a réellement (dB) ;
    cents   : écart de la hauteur de sortie à la note visée (trouvé sur l'harmonique la plus haute, pas de 0,005 Hz) ;
    nominal : pire écart mesuré exactement à h·fout. Une raie fine (fenêtre de 1,5 s) perd 0,05 dB dès que la sortie
              est décalée de 0,007 cent à 19 kHz : ce chiffre mesure la justesse, pas le timbre. */
static Flatness harmonicFlatness (const std::vector<double>& x, const std::vector<double>& y, double fs, double fin,
                                  double fout, size_t latency)
{
    Flatness f;
    const size_t a = latency + (size_t) (1.0 * fs), len = (size_t) (1.5 * fs);
    int top = 0;
    for (int h = 1; h * fout < 19500 && h * fin < 19900; ++h) top = h;
    double bestP = 0, bestD = 0;
    for (double d = -0.6; d <= 0.6; d += 0.005)
    {
        const double p = linePower (y, fs, top * fout + d, a, len);
        if (p > bestP) { bestP = p; bestD = d; }
    }
    const double scale = 1.0 + bestD / (top * fout);
    f.cents = 1200 * std::log2 (scale);
    for (int h = 1; h <= top; ++h)
    {
        if (h * fout < 100) continue;
        const double pin = linePower (x, fs, h * fin, (size_t) fs, len);
        const double e = 10 * std::log10 (linePower (y, fs, h * fout * scale, a, len) / pin);
        const double n = 10 * std::log10 (linePower (y, fs, h * fout, a, len) / pin);
        if (std::abs (e) > std::abs (f.energy)) { f.energy = e; f.energyHz = h * fout; }
        if (std::abs (n) > std::abs (f.nominal)) f.nominal = n;
    }
    return f;
}

static void testTransparency (bool ultra)
{
    std::printf ("\n[%d] Transparence%s : voix stable A3 (harmoniques jusqu'à 20 kHz), Preserve OFF, Retune 0.\n"
                 "    Par décalage : pire écart d'énergie d'une harmonique jusqu'à 19,5 kHz (dB, à sa fréquence réelle) ·\n"
                 "    écart de hauteur de la sortie (cents) · pire écart de la raie mesurée à la fréquence nominale (dB).\n",
                 ultra ? 5 : 1, ultra ? " en mode Ultra" : "");
    VoiceSpec vs; vs.jitter = 0; vs.shimmerDb = 0; vs.breathDb = -80;
    struct Shift { const char* name; double detuneCents, transpose; };
    const Shift shifts[] = { { "-40 c", 40, 0 }, { "+2 dt", 0, 2 }, { "+5 dt", 0, 5 }, { "+7 dt", 0, 7 }, { "+12 dt", 0, 12 },
                             { "+19 dt", 0, 19 }, { "-5 dt", 0, -5 }, { "-12 dt", 0, -12 } };
    for (double fs : { 44100.0, 48000.0, 96000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            std::printf ("  %5.1f k %s :", fs / 1000, modeName (mode));
            for (const auto& s : shifts)
            {
                if (fs > 48000 && (s.transpose == 2 || s.transpose == 7 || s.transpose == -5 || s.transpose == 19)) continue;
                if (fs < 48000 && (s.transpose == 2 || s.transpose == -5 || s.transpose == 19)) continue;
                const double fin = 220 * std::exp2 (s.detuneCents / 1200.0), fout = 220 * std::exp2 (s.transpose / 12.0);
                auto x = makeRealVoice (fs, 3.0, [&] (double) { return fin; }, one, none, vs);
                Setup su; su.mode = mode; su.transpose = s.transpose; su.ultra = ultra;
                auto e = makeEngine (fs, 1, su);
                auto y = runEngine (*e, { x })[0];
                const auto f = harmonicFlatness (x, y, fs, fin, fout, (size_t) e->getLatencySamples());
                std::printf ("  %s %+.3f dB · %+.3f c · %+.2f dB |", s.name, f.energy, f.cents, f.nominal);
                check (std::abs (f.energy) < 0.02, "énergie de chaque harmonique à ±0,02 dB jusqu'à 19,5 kHz");
                check (std::abs (f.cents) < 0.02, "hauteur de sortie à ±0,02 cent de la note visée");
                check (std::abs (f.nominal) < 0.1, "raie mesurée à la fréquence nominale à ±0,1 dB");
            }
            std::printf ("\n");
        }

    if (ultra) return;
    std::printf ("  Voix réelle (jitter 0,1 %%, shimmer, souffle −40 dB), +5 dt, 48 kHz : énergie par tiers d'octave de la sortie\n"
                 "  comparée à la bande d'entrée correspondante (f / r), niveau moyen 1,2–4 kHz retiré :\n");
    for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
    {
        const double fs = 48000, r = std::exp2 (5.0 / 12.0);
        VoiceSpec real; real.breathDb = -40;
        auto x = makeRealVoice (fs, 4.0, [] (double) { return 220.0; }, one, none, real);
        Setup su; su.mode = mode; su.transpose = 5;
        auto e = makeEngine (fs, 1, su);
        auto y = runEngine (*e, { x })[0];
        const size_t a = (size_t) e->getLatencySamples() + (size_t) fs, len = (size_t) (2.5 * fs);
        int order = 10; while ((size_t (1) << (order + 1)) <= len) ++order;
        const int N = 1 << order;
        RealFFT fft; fft.prepare (order);
        std::vector<double> fr ((size_t) N), re ((size_t) N / 2 + 1), im ((size_t) N / 2 + 1), px ((size_t) N / 2 + 1), py ((size_t) N / 2 + 1);
        auto spec = [&] (const std::vector<double>& s, size_t start, std::vector<double>& p)
        {
            for (int i = 0; i < N; ++i) fr[(size_t) i] = s[start + (size_t) i] * (0.5 - 0.5 * std::cos (2 * kPi * i / N));
            fft.forward (fr.data(), re.data(), im.data());
            for (size_t k = 0; k < p.size(); ++k) p[k] = re[k] * re[k] + im[k] * im[k];
        };
        spec (x, (size_t) fs, px); spec (y, a, py);
        auto band = [&] (const std::vector<double>& p, double lo, double hi)
        {
            double s = 0;
            for (size_t k = (size_t) std::ceil (lo * N / fs); k <= (size_t) std::floor (hi * N / fs) && k < p.size(); ++k) s += p[k];
            return s;
        };
        std::vector<std::pair<double, double>> bands;
        double ref = 0; int nref = 0;
        for (double fc = 1000.0; fc <= 20000.0 * 1.001; fc *= std::pow (2.0, 1.0 / 3.0))
        {
            const double lo = fc / std::pow (2.0, 1.0 / 6.0), hi = std::min (19900.0, fc * std::pow (2.0, 1.0 / 6.0));
            const double d = 10 * std::log10 (band (py, lo, hi) / band (px, lo / r, hi / r));
            bands.push_back ({ fc, d });
            if (fc > 1200 && fc < 4100) { ref += d; ++nref; }      // sous 1,2 kHz un tiers d'octave ne contient qu'une ou deux raies
        }
        ref /= nref;
        double worst = 0;
        std::printf ("    %s :", modeName (mode));
        for (auto& [f, d] : bands)
            if (f > 4100) { std::printf (" %.1f:%+.2f", f / 1000, d - ref); worst = std::max (worst, std::abs (d - ref)); }
        std::printf (" (kHz:dB)\n");
        check (worst < 0.1, "voix réelle +5 dt : ±0,1 dB par tiers d'octave de 5 à 20 kHz");
    }
}

//==================================================================================================================
/** Repliement : une sinusoïde dont l'image transposée dépasse fs / 2 doit disparaître. Niveau de ce qui se replie
    dans la bande audible, relatif à l'entrée (tête de lecture seule, rapport imposé). */
static double aliasDb (double fs, double toneHz, double semitones, bool ultra)
{
    SpliceShifter sp;
    sp.prepare (fs, 1, 0.002);
#if AURAPITCH_LOT1
    sp.setUltra (ultra);
#else
    (void) ultra;
#endif
    const double r = std::exp2 (semitones / 12.0);
    std::vector<double> x ((size_t) (2.0 * fs)), y (x.size());
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5 * std::sin (2 * kPi * toneHz * (double) i / fs);
    sp.setPitch (fs / 220.0, true);
    sp.setRatio (r);
    const double* in[2] = { x.data(), nullptr }; double* out[2] = { y.data(), nullptr };
    sp.process (in, out, (int) x.size());
    const double image = toneHz * r, alias = fs - image;                 // image > fs / 2 : se replie à fs − image
    double best = 0;
    for (double d = -40; d <= 40; d += 1.0)                              // raccords : la raie est étalée, on prend le maximum
        best = std::max (best, linePower (y, fs, alias + d, (size_t) (0.5 * fs), (size_t) fs));
    return 10 * std::log10 (best / linePower (x, fs, toneHz, (size_t) (0.5 * fs), (size_t) fs) + 1e-30);
}

static void testAlias()
{
    std::printf ("\n[6] Repliement vers le haut : sinusoïde dont l'image dépasse fs / 2, niveau replié dans la bande audible\n");
    struct Case { double fs, tone, semis; };
    for (const auto& c : { Case { 48000, 19000, 7 }, Case { 48000, 14500, 12 }, Case { 48000, 9500, 19 }, Case { 44100, 13500, 12 } })
    {
        const double r = std::exp2 (c.semis / 12.0);
        const double std_ = aliasDb (c.fs, c.tone, c.semis, false);
        std::printf ("  %4.1f k, %5.0f Hz à +%2.0f dt (image %4.1f k -> repli à %4.1f k) : %6.1f dB", c.fs / 1000, c.tone, c.semis,
                     c.tone * r / 1000, (c.fs - c.tone * r) / 1000, std_);
        check (std_ < -75.0, "repliement < −75 dB (noyau standard)");
#if AURAPITCH_LOT1
        const double ultra = aliasDb (c.fs, c.tone, c.semis, true);
        std::printf (" · Ultra %6.1f dB", ultra);
        check (ultra < -105.0, "repliement < −105 dB (Ultra)");
#endif
        std::printf ("\n");
    }
}

//==================================================================================================================
struct Residual { double peak = 0, rms = 0, mean = 0; int frames = 0; };

static Residual outputResidual (const std::vector<double>& y, double fs, double targetHz, double t0, double t1)
{
    PitchDetector det; det.prepare (fs);
    det.setRange ({ targetHz * 0.6, targetHz * 1.9 });          // fenêtre courte : ≈ 3,3 périodes de la note
    Residual r;
    double sum = 0, sum2 = 0;
    det.process (y.data(), (int) y.size(), [&] (const PitchEstimate& e, int)
    {
        const double t = (double) e.frameCentre / fs;
        if (t < t0 || t > t1 || ! e.voiced) return;
        const double c = 1200 * std::log2 (e.frequencyHz / targetHz);
        ++r.frames; sum += c; sum2 += c * c;
        r.peak = std::max (r.peak, std::abs (c));
    });
    if (r.frames > 0) { r.mean = sum / r.frames; r.rms = std::sqrt (sum2 / r.frames); }
    return r;
}

static void testVibrato()
{
    std::printf ("\n[2] Vibrato fort aplati (Retune 0, 5,8 Hz) : résidu de hauteur sur la sortie, crête / RMS en cents.\n"
                 "    Voix réelle (jitter 0,1 %%, souffle −30 dB). La même voix sans vibrato donne le plancher de mesure.\n");
    struct Note { const char* name; double hz; InputType type; };
    const Note notes[] = { { "A3", 220.0, InputType::AltoTenor }, { "A4", 440.0, InputType::AltoTenor }, { "A2", 110.0, InputType::LowMale } };
    for (double fs : { 48000.0, 384000.0 })
        for (const auto& note : notes)
        {
            if (fs > 48000 && &note != &notes[0]) continue;
            for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
            {
                std::printf ("  %5.1f k %s %s :", fs / 1000, modeName (mode), note.name);
                double floorPeak = 0.0;
                for (double depth : { 0.0, 30.0, 60.0, 100.0 })
                {
                    VoiceSpec vs;
                    auto f0 = [&] (double t) { return note.hz * std::exp2 ((12.0 + depth * std::sin (2 * kPi * 5.8 * t)) / 1200.0); };
                    auto x = makeRealVoice (fs, 3.0, f0, one, none, vs);
                    Setup su; su.mode = mode; su.type = note.type; su.scale = ScaleType::Major;   // A dans Do majeur : voisines à ±2 dt
                    auto e = makeEngine (fs, 1, su);
                    auto y = runEngine (*e, { x })[0];
                    const double L = e->getLatencySamples() / fs;
                    const auto r = outputResidual (y, fs, note.hz, 1.0 + L, 2.7 + L);
                    std::printf ("  ±%3.0f c -> %4.1f / %4.1f", depth, r.peak, r.rms);
                    if (depth == 0.0) floorPeak = r.peak;
                    // Le RMS est la mesure robuste ; la crête (une trame de 3,3 périodes sur 1,7 s) borne les accidents. Sans
                    // vibrato, le jitter de la voix laisse déjà ≈ 1 c RMS et 3 à 5 c de crête en Live : c'est le plancher.
                    if (mode == TrackingMode::Live && depth > 0)
                    {
                        check (r.rms < (depth <= 30 ? 1.5 : depth <= 60 ? 2.5 : 3.5), "Live : résidu RMS < 1,5 c (±30 c), < 2,5 c (±60 c), < 3,5 c (±100 c)");
                        check (r.peak < floorPeak + (depth <= 30 ? 2.0 : depth <= 60 ? 4.0 : 8.0),
                               "Live : crête au plus 2 c (±30 c), 4 c (±60 c), 8 c (±100 c) au-dessus du plancher sans vibrato");
                    }
                    if (mode == TrackingMode::Studio)
                    {
                        check (r.rms < (depth <= 60 ? 1.0 : 2.0), "Studio : résidu RMS < 1 c (< 2 c à ±100 c)");
                        check (r.peak < (depth <= 60 ? 5.0 : 10.0), "Studio : résidu crête < 5 c (< 10 c à ±100 c)");
                    }
                }
                std::printf ("\n");
            }
        }
}

//==================================================================================================================
static void testLatency()
{
    std::printf ("\n[3] Latences\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0, 352800.0, 384000.0 })
    {
        PitchDetector det; det.prepare (fs);
        std::printf ("  %5.1f k : fenêtre longue du détecteur (retard, ms)", fs / 1000);
        for (auto t : { InputType::Soprano, InputType::AltoTenor, InputType::LowMale, InputType::Instrument })
        {
            det.setInputType (t);
            std::printf (" %5.2f", 1000.0 * det.getAnalysisLatencySamples() / fs);
        }
        AuraPitchEngine e; e.prepare (fs, 2);
        std::printf (" · latence Live %d éch. (%.2f ms) · Studio %d éch. (%.1f ms)\n", e.getLatencySamples (TrackingMode::Live),
                     1000.0 * e.getLatencySamples (TrackingMode::Live) / fs, e.getLatencySamples (TrackingMode::Studio),
                     1000.0 * e.getLatencySamples (TrackingMode::Studio) / fs);
        check (std::abs (1000.0 * e.getLatencySamples (TrackingMode::Live) / fs - 2.0) < 0.02, "Live : 2 ms à toutes les fréquences");
    }

#if AURAPITCH_LOT1
    std::printf ("  Suivi rapide (fenêtre de 2,5 périodes de la note tenue) : retard de la hauteur utilisée en Live\n");
    for (double fs : { 48000.0, 384000.0 })
        for (double hz : { 110.0, 220.0, 440.0, 880.0 })
        {
            PitchDetector det; det.prepare (fs);
            det.setInputType (hz < 150 ? InputType::LowMale : InputType::AltoTenor);
            VoiceSpec vs; vs.jitter = 0; vs.shimmerDb = 0; vs.breathDb = -60;
            auto x = makeRealVoice (fs, 0.6, [&] (double) { return hz; }, one, none, vs);
            double fastDelay = 0, slowDelay = 0, err = 0; int n = 0;
            det.process (x.data(), (int) x.size(), [&] (const PitchEstimate& e, int i)
            {
                if (i < (int) (0.3 * fs) || ! e.fastValid) return;
                fastDelay += (double) (i - e.fastCentre); slowDelay += (double) (i - e.frameCentre);
                err = std::max (err, std::abs (1200 * std::log2 (e.fastFrequencyHz / hz))); ++n;
            });
            std::printf ("    %5.1f k %4.0f Hz : %5.2f ms (fenêtre longue %5.2f ms), erreur max %.3f c\n", fs / 1000, hz,
                         1000 * fastDelay / std::max (n, 1) / fs, 1000 * slowDelay / std::max (n, 1) / fs, err);
            check (n > 50 && fastDelay / std::max (n, 1) / fs < std::max (0.0045, 1.6 / hz) && err < 0.5,
                   "suivi rapide : retard ≤ 1,6 période (4,5 ms au plus court), juste à 0,5 c");
        }
#endif

    std::printf ("  Attaque A4 +40 c (montée 2 ms), Retune 0 : dernière trame de sortie à plus de 5 c de la note\n");
    for (double fs : { 48000.0, 384000.0 })
    {
        std::vector<double> x ((size_t) (fs * 1.0), 0.0);
        auto note = makeVowel (fs, 0.5, [] (double) { return 440.0 * std::exp2 (40.0 / 1200); });
        for (size_t i = 0; i < note.size(); ++i) x[(size_t) (0.3 * fs) + i] = note[i] * std::min (1.0, (double) i / (0.002 * fs));
        double settle[2] {};
        for (int m = 0; m < 2; ++m)
        {
            Setup su; su.mode = m ? TrackingMode::Studio : TrackingMode::Live; su.preserve = true;
            auto e = makeEngine (fs, 1, su);
            auto out = runEngine (*e, { x }, 128);
            const double onset = 0.3 + e->getLatencySamples() / fs;
            PitchDetector det; det.prepare (fs); det.setInputType (InputType::Soprano);
            double last = -1;
            det.process (out[0].data(), (int) out[0].size(), [&] (const PitchEstimate& est, int)
            {
                const double t = (double) est.frameCentre / fs - onset;
                if (t < 0 || t > 0.15 || ! est.voiced) return;
                if (std::abs (1200 * std::log2 (est.frequencyHz / 440.0)) > 5.0) last = t;
            });
            settle[m] = std::max (0.0, last) * 1000;
        }
        std::printf ("    %5.1f k : Live %.1f ms · Studio %.1f ms\n", fs / 1000, settle[0], settle[1]);
        check (settle[1] < 6.0, "Studio : correction calée sur l'attaque");
        check (settle[0] < 14.0, "Live : note juste moins de 14 ms après l'attaque");
    }
}

//==================================================================================================================
static void testCpu (bool ultra)
{
    std::printf ("\n[4] CPU%s (stéréo, voix corrigée en continu, %% d'un cœur)\n", ultra ? " en mode Ultra" : "");
    for (double fs : { 48000.0, 96000.0, 384000.0 })
    {
        VoiceSpec vs;
        auto v = makeRealVoice (fs, 4.0, [] (double t) { return 220.0 * std::exp2 ((30 + 25 * std::sin (2 * kPi * 5 * t)) / 1200); }, one, none, vs);
        std::printf ("  %5.1f k :", fs / 1000);
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            Setup su; su.mode = mode; su.preserve = true; su.ultra = ultra;
            auto e = makeEngine (fs, 2, su);
            std::vector<std::vector<double>> io { v, v };
            long allocs = 0;
            const auto t0 = std::chrono::steady_clock::now();
            const size_t len = v.size();
            for (size_t pos = 0; pos < len; pos += 256)
            {
                double* p[2] = { io[0].data() + pos, io[1].data() + pos };
                gAllocations = 0; gCountAllocations = true;
                e->process (p, 2, (int) std::min<size_t> (256, len - pos));
                gCountAllocations = false; allocs += gAllocations.load();
            }
            const double cpu = 100.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / 4.0;
            std::printf ("  %s %5.2f %% (%ld alloc.)", modeName (mode), cpu, allocs);
            check (allocs == 0, "aucune allocation dans process()");
            check (cpu < cpuBudget (ultra ? 40.0 : 20.0), "budget CPU");
        }
        std::printf ("\n");
    }
}

//==================================================================================================================
#if AURAPITCH_LOT1
static void testUltra()
{
    testTransparency (true);

    std::printf ("  Latence : identique avec et sans Ultra (pas de changement de latence au bounce)\n");
    for (double fs : { 44100.0, 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            Setup a; a.mode = mode; Setup b = a; b.ultra = true;
            auto e1 = makeEngine (fs, 2, a), e2 = makeEngine (fs, 2, b);
            check (e1->getLatencySamples() == e2->getLatencySamples(), "Ultra : latence inchangée");
        }

    std::printf ("  Bascule à chaud (toutes les 250 ms) pendant une voix corrigée : énergie > 6 kHz d'une voyelle sombre\n");
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            auto x = makeVowel (fs, 3.0, [] (double) { return 220.0 * std::exp2 (35.0 / 1200); }, 0.3, 1.0, 2.0);
            auto run = [&] (bool toggle)
            {
                Setup su; su.mode = mode; su.preserve = true;
                auto e = makeEngine (fs, 1, su);
                auto p = e->getParameters();
                std::vector<double> y = x;
                long allocs = 0;
                for (size_t pos = 0; pos < y.size(); pos += 256)
                {
                    p.ultra = toggle && ((int) ((double) pos / fs / 0.25) % 2 == 1);
                    double* io[1] = { y.data() + pos };
                    gAllocations = 0; gCountAllocations = true;
                    e->setParameters (p);
                    e->process (io, 1, (int) std::min<size_t> (256, y.size() - pos));
                    gCountAllocations = false; allocs += gAllocations.load();
                }
                check (allocs == 0, "bascule Ultra : aucune allocation");
                const double L = e->getLatencySamples() / fs;
                return maxDb (highBandRms (y, fs), fs, 0.6 + L, 2.8 + L);
            };
            const double steady = run (false), toggled = run (true);
            std::printf ("    %5.1f k %s : sans bascule %.1f dB · avec %.1f dB\n", fs / 1000, modeName (mode), steady, toggled);
            check (toggled < steady + 3.0, "bascule Ultra à chaud : aucun clic");
        }
    testCpu (true);
}
#endif

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&] (const char* k) { return only.empty() || only == k; };
    std::printf ("AuraPitch — banc du lot 1 (cœur DSP)%s\n", AURAPITCH_LOT1 ? "" : " — moteur d'avant le lot 1");
    if (want ("transp"))  testTransparency (false);
    if (want ("vibrato")) testVibrato();
    if (want ("latence")) testLatency();
    if (want ("cpu"))     testCpu (false);
    if (want ("alias"))   testAlias();
#if AURAPITCH_LOT1
    if (want ("ultra"))   testUltra();
#endif
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

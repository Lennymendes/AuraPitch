// AuraPitch — banc d'essai de l'étape 2 : FFT, enveloppe, PSOLA (Live), vocodeur Studio/DXD, moteur complet.
// Code de sortie 0 si toutes les vérifications passent.
#include "TestSupport.h"

#include "../Source/DSP/AuraPitchEngine.h"

#include <chrono>
#include <functional>
#include <thread>

using namespace aura::pitch;
using namespace test;

static const double kRates[] = { 44100, 48000, 96000, 192000, 352800, 384000 };

//==================================================================================================================
static void testFftAndEnvelope()
{
    std::printf ("\n[1] FFT réelle et enveloppe vraie\n");
    for (int order : { 6, 10, 12, 15 })
    {
        RealFFT f; f.prepare (order);
        const int n = 1 << order;
        std::vector<double> x ((size_t) n), re ((size_t) n / 2 + 1), im ((size_t) n / 2 + 1), y ((size_t) n);
        std::mt19937 r (order); std::normal_distribution<double> g;
        for (auto& v : x) v = g (r);
        f.forward (x.data(), re.data(), im.data());
        double err = 0;
        for (int k = 0; k <= n / 2; k += std::max (1, n / 64))
        {
            long double a = 0, b = 0;
            for (int i = 0; i < n; ++i) { a += x[(size_t) i] * std::cos (2 * kPi * (double) k * i / n); b -= x[(size_t) i] * std::sin (2 * kPi * (double) k * i / n); }
            err = std::max (err, (double) std::hypot (a - re[(size_t) k], b - im[(size_t) k]) / std::sqrt ((double) n));
        }
        f.inverse (re.data(), im.data(), y.data());
        double rt = 0;
        for (int i = 0; i < n; ++i) rt = std::max (rt, std::abs (y[(size_t) i] - x[(size_t) i]));
        std::printf ("  N = %5d : écart à la DFT %.1e, aller-retour %.1e\n", n, err, rt);
        check (err < 1e-11 && rt < 1e-13, "FFT réelle exacte");
    }

    // Enveloppe vraie : écart à l'enveloppe synthétique exacte (formants × pente −6 dB/oct.), 300–4000 Hz.
    for (double fs : { 44100.0, 48000.0, 384000.0 })
        for (double f0 : { 110.0, 220.0, 330.0 })
        {
            auto v = makeVowel (fs, 0.3, [f0] (double) { return f0; });
            auto fm = measureFormants (v, fs, 0.1, f0);
            Formants truth = fm;
            for (size_t k = 0; k < truth.envDb.size(); ++k)
            {
                const double f = std::max (1.0, (double) k * truth.binHz);
                auto res = [f] (double fc, double bw) { const double d = (f - fc) / (0.5 * bw); return 1.0 / std::sqrt (1.0 + d * d); };
                truth.envDb[k] = 20 * std::log10 ((0.15 + res (700, 110) + 0.6 * res (1220, 120) + 0.3 * res (2600, 160)) / (f / f0));
            }
            const double dist = envelopeDistanceDb (truth, fm);
            std::printf ("  fs %6.1f k, F0 %3.0f Hz : enveloppe vraie à %.2f dB RMS de l'enveloppe exacte, décalage mesuré %+.2f dt\n",
                         fs / 1000, f0, dist, formantShiftSemitones (truth, fm));
            check (dist < (f0 < 300 ? 2.0 : 3.0), "enveloppe vraie fidèle (< 2 dB RMS ; 3 dB à 330 Hz)");
            // Au-delà de ~250 Hz, l'enveloppe n'est échantillonnée qu'aux harmoniques : la position d'un formant tombé
            // entre deux harmoniques n'est plus observable (limite physique, pas de l'estimateur).
            if (f0 < 250) check (std::abs (formantShiftSemitones (truth, fm)) < 0.35, "formants à leur place (< 0,35 dt)");
        }
}

//==================================================================================================================
static std::vector<double> runPsola (double fs, const std::vector<double>& x, double f0, double semis, double formantSemis,
                                     bool preserve, int* latencyOut = nullptr)
{
    PSOLAShifter p; p.prepare (fs, 1);
    std::vector<double> y (x.size());
    const double* in[1] = { x.data() };
    double* out[1] = { y.data() };
    p.setPitch (fs / f0, true);
    p.setShift (std::exp2 (semis / 12), std::exp2 (formantSemis / 12), preserve);
    p.process (in, out, (int) x.size());
    if (latencyOut) *latencyOut = p.getLatencySamples();
    return y;
}

static void testPsola()
{
    std::printf ("\n[2] PSOLAShifter (Live)\n");

    // a. Identité au rapport 1 : sortie = entrée retardée de D, au sample près, stéréo, toutes fréquences.
    for (double fs : kRates)
    {
        auto l = makeVowel (fs, 0.6, [] (double t) { return 180.0 * std::exp2 (0.3 * std::sin (2 * kPi * 4 * t)); });
        auto r = makeVowel (fs, 0.6, [] (double) { return 247.0; }, 0.2);
        PSOLAShifter p; p.prepare (fs, 2);
        std::vector<double> yl (l.size()), yr (l.size());
        const double* in[2] = { l.data(), r.data() };
        double* out[2] = { yl.data(), yr.data() };
        for (size_t pos = 0; pos < l.size(); pos += 480)
        {
            const int n = (int) std::min<size_t> (480, l.size() - pos);
            p.setPitch (fs / (180.0 * std::exp2 (0.3 * std::sin (2 * kPi * 4 * pos / fs))), true);
            p.setShift (1.0, 1.0, true);
            const double* i2[2] = { in[0] + pos, in[1] + pos };
            double* o2[2] = { out[0] + pos, out[1] + pos };
            p.process (i2, o2, n);
        }
        const int D = p.getLatencySamples();
        double e = 0;
        for (size_t i = (size_t) D; i < l.size(); ++i)
            e = std::max ({ e, std::abs (yl[i] - l[i - (size_t) D]), std::abs (yr[i] - r[i - (size_t) D]) });
        std::printf ("  %5.1f k : rapport 1, F0 vibrée ±30 c -> écart à l'entrée retardée de %d éch. (%.2f ms) : %.1e\n", fs / 1000, D, D / fs * 1000, e);
        check (e < 1e-12, "PSOLA au rapport 1 = entrée retardée de D (stéréo, F0 variable)");
    }

    // b. Justesse de la note de sortie
    std::printf ("  justesse (voyelle 220 Hz, F0 mesurée par le PitchDetector), écart moyen | max en cents, niveau :\n");
    for (double fs : { 48000.0, 384000.0 })
    {
        auto x = makeVowel (fs, 0.8, [] (double) { return 220.0; });
        std::printf ("   %5.1f k :", fs / 1000);
        std::vector<double> levels[2];
        for (double st : { 1.0, -1.0, 3.0, -3.0, 7.0, -7.0, 12.0, -12.0 })
        {
            auto y = runPsola (fs, x, 220, st, 0, true);
            auto s = measurePitch (y, fs, 220 * std::exp2 (st / 12), 0.2, 0.75);
            const double level = rmsDb (y, (size_t) (0.2 * fs), y.size()) - rmsDb (x, (size_t) (0.2 * fs), x.size());
            levels[st < 0 ? 0 : 1].push_back (level);
            std::printf (" %+3.0f:%+.2f|%.2f", st, s.meanCents, s.maxCents);
            check (std::abs (s.meanCents) < 0.3 && s.maxCents < 1.5 && s.voicedRatio > 0.99, "PSOLA : note de sortie juste (moyenne < 0,3 c, max < 1,5 c)");
        }
        std::printf ("\n      niveau RMS en descente (−1, −3, −7, −12 dt) :");
        for (double l : levels[0]) std::printf (" %+.2f dB", l);
        std::printf (" · en montée (+1, +3, +7, +12) :");
        for (double l : levels[1]) std::printf (" %+.2f dB", l);
        std::printf ("\n");
        for (double l : levels[0]) check (std::abs (l) <= 0.5, "PSOLA en descente : niveau RMS constant à ±0,5 dB");
    }

    // c. Formants
    {
        const double fs = 48000;
        auto x = makeVowel (fs, 0.8, [] (double) { return 165.0; });
        auto in = measureFormants (x, fs, 0.4, 165);
        auto keep = runPsola (fs, x, 165, 5, 0, true);
        auto chip = runPsola (fs, x, 165, 5, 0, false);
        auto fsh  = runPsola (fs, x, 165, 0, 4, true);
        const double up = 165 * std::exp2 (5.0 / 12);
        auto mk = measureFormants (keep, fs, 0.4, up);
        const double gk = formantShiftFromHarmonics (x, 165, 0.4, keep, up, 0.4, fs);
        const double gc = formantShiftFromHarmonics (x, 165, 0.4, chip, up, 0.4, fs);
        const double gf = formantShiftFromHarmonics (x, 165, 0.4, fsh, 165, 0.4, fs);
        const double pf = measurePitch (fsh, fs, 165, 0.2, 0.75).meanCents;
        std::printf ("  formants (voyelle 165 Hz), déplacement mesuré sur les harmoniques :\n");
        std::printf ("    +5 dt Preserve ON  : %+.2f dt (attendu 0), écart d'enveloppe %.1f dB\n", gk, envelopeDistanceDb (in, mk));
        std::printf ("    +5 dt Preserve OFF : %+.2f dt (attendu +5)\n", gc);
        std::printf ("    FormantShift +4 dt : %+.2f dt (attendu +4), hauteur %+.2f c\n", gf, pf);
        check (std::abs (gk) < 0.5 && envelopeDistanceDb (in, mk) < 3.0, "PSOLA Preserve ON : formants en place (< 0,5 dt, < 3 dB RMS)");
        check (std::abs (gc - 5) < 0.7, "PSOLA Preserve OFF : formants suivent la hauteur");
        check (std::abs (gf - 4) < 0.7, "PSOLA FormantShift +4 dt");
        check (std::abs (pf) < 0.5, "PSOLA FormantShift seul : hauteur inchangée");
    }

    // d. Transitions voisé / non voisé : aucun clic (fondu d'une période vers l'entrée retardée)
    for (double fs : { 48000.0, 384000.0 })
    {
        auto x = makeVowel (fs, 1.2, [] (double) { return 196.0; }, 0.3, 1.0, 2.0);
        PSOLAShifter p; p.prepare (fs, 1);
        std::vector<double> y (x.size()), hard (x.size());
        const int blk = (int) (0.001 * fs);
        for (size_t pos = 0; pos < x.size(); pos += (size_t) blk)
        {
            const int n = (int) std::min<size_t> ((size_t) blk, x.size() - pos);
            const bool voiced = ((int) (pos / (0.12 * fs))) % 2 == 0;
            p.setPitch (fs / 196.0, voiced);
            p.setShift (std::exp2 (3.0 / 12), 1.0, true);
            const double* in[1] = { x.data() + pos };
            double* out[1] = { y.data() + pos };
            p.process (in, out, n);
        }
        // Référence « sans fondu » : bascule franche entre la sortie PSOLA et l'entrée retardée.
        auto shifted = runPsola (fs, x, 196, 3, 0, true);
        const int D = p.getLatencySamples();
        for (size_t i = 0; i < x.size(); ++i)
            hard[i] = ((int) (i / (0.12 * fs))) % 2 == 0 ? shifted[i] : (i >= (size_t) D ? x[i - (size_t) D] : 0.0);
        auto hy = highBandRms (y, fs), hh = highBandRms (hard, fs);
        double steady = std::max (maxDb (hy, fs, 0.03, 0.11), maxDb (hy, fs, 0.15, 0.23));
        double worst = -300, worstHard = -300;
        for (int k = 1; k < 9; ++k)
        {
            worst = std::max (worst, maxDb (hy, fs, k * 0.12 - 0.01, k * 0.12 + 0.03));
            worstHard = std::max (worstHard, maxDb (hh, fs, k * 0.12 - 0.01, k * 0.12 + 0.03));
        }
        std::printf ("  %5.1f k : bascules V/UV toutes les 120 ms, énergie > 6 kHz : régime %.1f dB, transitions %.1f dB "
                     "(bascule franche : %.1f dB)\n", fs / 1000, steady, worst, worstHard);
        check (worst < steady + 3.0, "PSOLA : transitions V/UV sans clic (< +3 dB au-dessus du régime)");
        check (worstHard > worst + 10.0, "le détecteur de clics voit une bascule franche (+10 dB)");
    }
}

//==================================================================================================================
static std::vector<double> runStudio (double fs, const std::vector<double>& x, double f0, double semis, double formantSemis,
                                      bool preserve, int& latency, StudioDxdShifter* use = nullptr, bool voiced = true)
{
    StudioDxdShifter local;
    StudioDxdShifter& s = use ? *use : local;
    if (! use) s.prepare (fs, 1);
    s.reset();
    std::vector<double> d (x.size());
    const double* in[1] = { x.data() };
    double* out[1] = { d.data() };
    s.process (in, out, (int) x.size(), [&] (std::int64_t, std::int64_t)
    {
        FrameControl c; c.ratio = std::exp2 (semis / 12); c.formantFactor = std::exp2 (formantSemis / 12);
        c.preserve = preserve; c.voiced = voiced; c.f0 = f0; return c;
    });
    latency = s.getLatencySamples();
    std::vector<double> y (x.size(), 0.0);
    for (size_t i = (size_t) latency; i < x.size(); ++i) y[i] = x[i - (size_t) latency] + d[i];
    return y;
}

static void testStudio()
{
    std::printf ("\n[3] StudioDxdShifter (Studio/DXD)\n");

    // a. Découpe multicadence : alignement au sample près, vocodeur neutre exact
    for (double fs : kRates)
    {
        StudioDxdShifter s; s.prepare (fs, 2);
        auto v = makeVowel (fs, 1.2, [] (double) { return 150.0; });
        auto nz = makeNoise (fs, 1.2, 0.1, 7);
        std::vector<double> d0 (v.size()), d1 (v.size());
        const double* in[2] = { v.data(), nz.data() };
        double* out[2] = { d0.data(), d1.data() };
        s.process (in, out, (int) v.size(), [] (std::int64_t, std::int64_t) { FrameControl c; c.voiced = false; return c; });
        double neutral = 0;
        for (size_t i = 0; i < v.size(); ++i) neutral = std::max ({ neutral, std::abs (d0[i]), std::abs (d1[i]) });

        double residual = -300;
        if (s.getDecimationFactor() > 1)
        {
            s.reset(); s.setMuteVocoderForTest (true);
            s.process (in, out, (int) v.size(), [] (std::int64_t, std::int64_t) { FrameControl c; return c; });
            s.setMuteVocoderForTest (false);
            const int L = s.getLatencySamples();
            double e = 0, ex = 0;
            for (size_t i = (size_t) L + (size_t) (0.1 * fs); i < v.size(); ++i) { const double w = v[i - (size_t) L] + d0[i]; e += w * w; ex += v[i] * v[i]; }
            residual = 10 * std::log10 (e / ex + 1e-300);
        }
        std::printf ("  %5.1f k : ÷%d -> vocodeur %5.0f Hz, FFT %d, latence %6d éch. (%.1f ms) · neutre max|Δ| %.1e · "
                     "bande vocale retirée : %.1f dB\n", fs / 1000, s.getDecimationFactor(), s.getVocoderRate(), s.getFftSize(),
                     s.getLatencySamples(), s.getLatencySamples() / fs * 1000, neutral, residual);
        check (neutral < 1e-12, "vocodeur neutre : Δ = 0 (sortie = entrée retardée)");
        if (s.getDecimationFactor() > 1)
            check (residual < -120, "voie vocale alignée au sample près sur la voie directe (résidu < −120 dB)");
    }

    // b. Justesse
    std::printf ("  justesse (voyelle 220 Hz), écart moyen | max en cents :\n");
    for (double fs : { 48000.0, 96000.0, 384000.0 })
    {
        StudioDxdShifter s; s.prepare (fs, 1);
        auto x = makeVowel (fs, 1.4, [] (double) { return 220.0; });
        std::printf ("   %5.1f k :", fs / 1000);
        for (double st : { 1.0, -1.0, 7.0, -7.0, 12.0, -12.0 })
        {
            int L = 0;
            auto y = runStudio (fs, x, 220, st, 0, true, L, &s);
            const double t0 = L / fs + 0.15;
            auto m = measurePitch (y, fs, 220 * std::exp2 (st / 12), t0, 1.35);
            std::printf (" %+3.0f:%+.2f|%.2f", st, m.meanCents, m.maxCents);
            check (std::abs (m.meanCents) < 0.1 && m.maxCents < 0.5 && m.voicedRatio > 0.99, "Studio : note de sortie juste (moyenne < 0,1 c, max < 0,5 c)");
        }
        std::printf ("\n");
    }

    // c. Formants (vrais) : Preserve ON / OFF, FormantShift
    for (double fs : { 48000.0, 384000.0 })
    {
        StudioDxdShifter s; s.prepare (fs, 1);
        auto x = makeVowel (fs, 1.4, [] (double) { return 165.0; });
        int L = 0;
        auto in = measureFormants (x, fs, 0.5, 165);
        auto keep = runStudio (fs, x, 165, 5, 0, true, L, &s);
        auto chip = runStudio (fs, x, 165, 5, 0, false, L, &s);
        auto fsh  = runStudio (fs, x, 165, 0, 4, true, L, &s);
        const double up = 165 * std::exp2 (5.0 / 12), t = L / fs + 0.4;
        auto mk = measureFormants (keep, fs, t, up);
        const double gk = formantShiftFromHarmonics (x, 165, 0.5, keep, up, t, fs);
        const double gc = formantShiftFromHarmonics (x, 165, 0.5, chip, up, t, fs);
        const double gf = formantShiftFromHarmonics (x, 165, 0.5, fsh, 165, t, fs);
        const double pf = measurePitch (fsh, fs, 165, t, 1.35).meanCents;
        std::printf ("  %5.1f k formants : +5 dt Preserve ON %+.2f dt (écart %.1f dB) · Preserve OFF %+.2f dt · FormantShift +4 : %+.2f dt, hauteur %+.2f c\n",
                     fs / 1000, gk, envelopeDistanceDb (in, mk), gc, gf, pf);
        check (std::abs (gk) < 0.5 && envelopeDistanceDb (in, mk) < 3.0, "Studio Preserve ON : formants en place");
        check (std::abs (gc - 5) < 0.7, "Studio Preserve OFF : formants suivent la hauteur");
        check (std::abs (gf - 4) < 0.7, "Studio FormantShift +4 dt");
        check (std::abs (pf) < 0.2, "Studio FormantShift seul : hauteur inchangée");
    }

    // d. Air du DXD : un 30 kHz superposé à la voix traverse intact pendant la correction
    {
        const double fs = 384000;
        auto x = makeVowel (fs, 1.2, [] (double) { return 220.0; });
        for (size_t i = 0; i < x.size(); ++i) x[i] += 0.01 * std::sin (2 * kPi * 30000.0 * i / fs);
        int L = 0;
        auto y = runStudio (fs, x, 220, 2, 0, true, L);
        // Amplitude et phase du 30 kHz par moindres carrés (sin / cos) sur 0,5 s, entrée retardée vs sortie
        auto fit = [&] (const std::vector<double>& s, size_t a, size_t b)
        {
            double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
            for (size_t i = a; i < b; ++i)
            {
                const double sn = std::sin (2 * kPi * 30000.0 * i / fs), cs = std::cos (2 * kPi * 30000.0 * i / fs);
                ss += sn * sn; sc += sn * cs; cc += cs * cs; ys += s[i] * sn; yc += s[i] * cs;
            }
            const double det = ss * cc - sc * sc;
            return std::pair<double, double> { (ys * cc - yc * sc) / det, (yc * ss - ys * sc) / det };
        };
        const size_t a = (size_t) L + (size_t) (0.3 * fs), b = a + (size_t) (0.5 * fs);
        std::vector<double> xd (x.size(), 0.0);
        for (size_t i = (size_t) L; i < x.size(); ++i) xd[i] = x[i - (size_t) L];
        auto [ia, ib] = fit (xd, a, b);
        auto [oa, ob] = fit (y, a, b);
        const double gainDb = 20 * std::log10 (std::hypot (oa, ob) / std::hypot (ia, ib));
        const double phaseDeg = (std::atan2 (ob, oa) - std::atan2 (ib, ia)) * 180 / kPi;
        auto m = measurePitch (y, fs, 220 * std::exp2 (2.0 / 12), L / fs + 0.15, 1.15);
        std::printf ("  384 k, voix +2 dt + air à 30 kHz : 30 kHz %+.4f dB, %+.3f° · voix %+.2f c\n", gainDb, phaseDeg, m.meanCents);
        check (std::abs (gainDb) < 0.01 && std::abs (phaseDeg) < 0.1, "air du DXD intact (±0,01 dB, ±0,1°)");
        check (std::abs (m.meanCents) < 0.1, "voix corrigée sous l'air");
    }

    // e. Transitoires : remise en phase sur l'attaque (forme d'onde des 4 premières ms comparée à l'entrée)
    {
        const double fs = 48000;
        std::vector<double> x ((size_t) (fs * 2.0), 0.0);
        std::vector<size_t> onsets;
        for (int b = 0; b < 6; ++b)
        {
            const size_t s0 = (size_t) ((0.2 + 0.3 * b) * fs);
            onsets.push_back (s0);
            for (size_t i = 0; i < (size_t) (0.25 * fs); ++i)
            {
                const double t = (double) i / fs;
                x[s0 + i] += 0.5 * std::exp (-t / 0.04) * (std::sin (2 * kPi * 600 * t) + 0.5 * std::sin (2 * kPi * 1800 * t + 0.3)
                                                           + 0.3 * std::sin (2 * kPi * 3100 * t + 1.0));
            }
        }
        for (auto res : { StudioDxdShifter::Resolution::Fast, StudioDxdShifter::Resolution::Fine })
        {
            double corr[2] {}, pre[2] {};
            std::int64_t transients = 0;
            for (int rst = 0; rst < 2; ++rst)
            {
                StudioDxdShifter s; s.prepare (fs, 1); s.setResolution (res); s.reset();
                s.getCore().setTransientResetEnabled (rst == 1);
                int L = 0;
                auto y = runStudio (fs, x, 600, 1, 0, true, L, &s);
                double e0 = 0, e1 = 0;
                for (size_t o : onsets)
                {
                    const size_t a = o + (size_t) L;
                    for (size_t i = a - (size_t) (0.02 * fs); i < a; ++i) e0 += y[i] * y[i];
                    for (size_t i = a; i < a + (size_t) (0.02 * fs); ++i) e1 += y[i] * y[i];
                    double sxy = 0, sxx = 0, syy = 0;
                    for (size_t i = 0; i < (size_t) (0.004 * fs); ++i) { sxy += y[a + i] * x[o + i]; sxx += x[o + i] * x[o + i]; syy += y[a + i] * y[a + i]; }
                    corr[rst] += sxy / std::sqrt (sxx * syy) / (double) onsets.size();
                }
                pre[rst] = 10 * std::log10 (e0 / e1);
                if (rst == 1) transients = s.getCore().getTransientCount();
            }
            std::printf ("  salves percussives +1 dt, FFT %s : forme de l'attaque (corrélation 4 ms) %.2f sans remise en phase, "
                         "%.2f avec · pré-écho %.1f dB · %lld attaques détectées\n", res == StudioDxdShifter::Resolution::Fast ? "2048" : "4096",
                         corr[0], corr[1], pre[1], (long long) transients);
            check (transients >= 6, "toutes les attaques détectées");
            check (corr[1] > corr[0] + 0.25, "remise en phase : forme de l'attaque mieux conservée");
            check (pre[1] < -20.0, "pré-écho < −20 dB");
        }
    }

    // f. Transitions voisé / non voisé
    {
        const double fs = 48000;
        auto x = makeVowel (fs, 2.0, [] (double) { return 196.0; }, 0.3, 1.0, 2.0);
        StudioDxdShifter s; s.prepare (fs, 1);
        std::vector<double> d (x.size());
        const double* in[1] = { x.data() };
        double* out[1] = { d.data() };
        s.process (in, out, (int) x.size(), [&] (std::int64_t c, std::int64_t)
        {
            FrameControl fc; fc.ratio = std::exp2 (3.0 / 12); fc.f0 = 196;
            fc.voiced = ((int) (c / (0.25 * fs))) % 2 == 0;
            return fc;
        });
        const int L = s.getLatencySamples();
        std::vector<double> y (x.size(), 0.0);
        for (size_t i = (size_t) L; i < x.size(); ++i) y[i] = x[i - (size_t) L] + d[i];
        auto hy = highBandRms (y, fs);
        const double t0 = L / fs;
        const double steady = std::max (maxDb (hy, fs, t0 + 0.08, t0 + 0.17), maxDb (hy, fs, t0 + 0.33, t0 + 0.42));
        double worst = -300;
        for (int k = 1; k < 6; ++k) worst = std::max (worst, maxDb (hy, fs, t0 + k * 0.25 - 0.06, t0 + k * 0.25 + 0.06));
        std::printf ("  bascules V/UV toutes les 250 ms : énergie > 6 kHz, régime %.1f dB, transitions %.1f dB\n", steady, worst);
        check (worst < steady + 3.0, "Studio : transitions V/UV sans clic");
    }
}

//==================================================================================================================
template <typename T = double>
static std::vector<std::vector<double>> runEngine (AuraPitchEngine& eng, const std::vector<std::vector<double>>& in, int block,
                                                   const std::function<void (AuraPitchParams&, size_t)>& automate = nullptr)
{
    const size_t len = in[0].size();
    const int nc = (int) in.size();
    std::vector<std::vector<T>> buf ((size_t) nc, std::vector<T> (len));
    for (int ch = 0; ch < nc; ++ch) for (size_t i = 0; i < len; ++i) buf[(size_t) ch][i] = (T) in[(size_t) ch][i];
    for (size_t pos = 0; pos < len; pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, len - pos);
        if (automate) { AuraPitchParams p = eng.getParameters(); automate (p, pos); eng.setParameters (p); }
        T* ptr[2] = { buf[0].data() + pos, nc > 1 ? buf[1].data() + pos : nullptr };
        eng.process (ptr, nc, n);
    }
    std::vector<std::vector<double>> out ((size_t) nc, std::vector<double> (len));
    for (int ch = 0; ch < nc; ++ch) for (size_t i = 0; i < len; ++i) out[(size_t) ch][i] = (double) buf[(size_t) ch][i];
    return out;
}

static void testEngine()
{
    std::printf ("\n[4] AuraPitchEngine\n");

    // a. Latence annoncée et alignement Dry / Wet : sur un signal non voisé, sortie = entrée retardée, quel que soit Mix.
    for (double fs : { 44100.0, 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
            for (double mix : { 1.0, 0.5 })
            {
                AuraPitchEngine eng; eng.prepare (fs, 2);
                AuraPitchParams p; p.mode = mode; p.mix = mix; p.formantShift = 3; p.transpose = 5;
                eng.setParameters (p); eng.reset();
                const double secs = mode == TrackingMode::Studio ? 0.6 : 0.3;
                std::vector<std::vector<double>> in { makeNoise (fs, secs, 0.1, 1), makeNoise (fs, secs, 0.1, 2) };
                auto out = runEngine (eng, in, 512);
                const int L = eng.getLatencySamples();
                double e = 0;
                for (int ch = 0; ch < 2; ++ch)
                    for (size_t i = (size_t) L; i < in[0].size(); ++i) e = std::max (e, std::abs (out[(size_t) ch][i] - in[(size_t) ch][i - (size_t) L]));
                std::printf ("  %5.1f k %-6s Mix %3.0f %% : latence %6d éch. (%5.1f ms), souffle non voisé -> écart à l'entrée retardée %.1e\n",
                             fs / 1000, mode == TrackingMode::Live ? "Live" : "Studio", mix * 100, L, L / fs * 1000, e);
                check (e < 1e-12, "non voisé : sortie = Dry retardé de la latence annoncée, Mix sans peigne");
            }

    // b. Hard-tune : A3 chanté 30 cents trop haut, Retune 0 -> 220 Hz
    for (double fs : { 48000.0, 384000.0 })
        for (auto mode : { TrackingMode::Live, TrackingMode::Studio })
        {
            AuraPitchEngine eng; eng.prepare (fs, 1);
            AuraPitchParams p; p.mode = mode; p.retuneMs = 0; p.scale = ScaleType::Major; p.key = 0;
            eng.setParameters (p); eng.reset();
            auto x = makeVowel (fs, 1.5, [] (double t) { return 220.0 * std::exp2 ((30.0 + 8 * std::sin (2 * kPi * 5 * t)) / 1200); });
            auto out = runEngine (eng, { x }, 256);
            const double t0 = eng.getLatencySamples() / fs + 0.2;
            auto m = measurePitch (out[0], fs, 220.0, t0, 1.45);
            std::printf ("  %5.1f k %-6s : A3 +30 c ±8 c de vibrato, Retune 0 -> sortie %+.2f c (max %.2f c)\n",
                         fs / 1000, mode == TrackingMode::Live ? "Live" : "Studio", m.meanCents, m.maxCents);
            check (std::abs (m.meanCents) < 1.0 && m.maxCents < 6.0, "hard-tune : sortie sur la note");
        }

    // c. Attaque : la correction Studio tombe sur l'attaque, Live la rattrape après le délai du détecteur
    {
        const double fs = 48000;
        std::vector<double> x ((size_t) (fs * 1.0), 0.0);
        auto note = makeVowel (fs, 0.5, [] (double) { return 440.0 * std::exp2 (40.0 / 1200); });
        for (size_t i = 0; i < note.size(); ++i) x[(size_t) (0.3 * fs) + i] = note[i] * std::min (1.0, i / (0.002 * fs));
        double settle[2] {};
        for (int m = 0; m < 2; ++m)
        {
            AuraPitchEngine eng; eng.prepare (fs, 1);
            AuraPitchParams p; p.mode = m ? TrackingMode::Studio : TrackingMode::Live; p.retuneMs = 0; p.inputType = InputType::AltoTenor;
            eng.setParameters (p); eng.reset();
            auto out = runEngine (eng, { x }, 128);
            const double onset = 0.3 + eng.getLatencySamples() / fs;
            // Erreur instantanée de la sortie : détecteur Soprano (fenêtre courte, 6,9 ms)
            PitchDetector det; det.prepare (fs); det.setInputType (InputType::Soprano);
            double last = -1;
            det.process (out[0].data(), (int) out[0].size(), [&] (const PitchEstimate& e, int)
            {
                const double t = (double) e.frameCentre / fs - onset;
                if (t < 0 || t > 0.15 || ! e.voiced) return;
                if (std::abs (1200 * std::log2 (e.frequencyHz / 440.0)) > 5.0) last = t;
            });
            settle[m] = std::max (0.0, last) * 1000;
        }
        std::printf ("  attaque A4 +40 c, Retune 0 : sortie juste (< 5 c) %.1f ms après l'attaque en Live, %.1f ms en Studio\n",
                     settle[0], settle[1]);
        check (settle[1] < 6.0, "Studio : correction calée sur l'attaque (< 6 ms, limite de mesure)");
        check (settle[0] > settle[1], "Live : correction après le délai du détecteur");
    }

    // d. Changement de mode à chaud, FIFO, float = double
    {
        const double fs = 96000;
        AuraPitchEngine eng; eng.prepare (fs, 2);
        AuraPitchParams p; p.retuneMs = 10; eng.setParameters (p); eng.reset();
        auto x = makeVowel (fs, 1.2, [] (double) { return 207.65 * std::exp2 (20.0 / 1200); });
        auto out = runEngine (eng, { x, x }, 480, [&] (AuraPitchParams& q, size_t pos) { q.mode = pos > (size_t) (0.5 * fs) ? TrackingMode::Studio : TrackingMode::Live; });
        bool finite = true;
        for (auto& c : out) for (double v : c) finite &= std::isfinite (v);
        int frames = 0, voiced = 0; double sumIn = 0;
        eng.getPitchFifo().drain ([&] (const PitchFrame& f) { ++frames; if (f.voiced) { ++voiced; sumIn += f.inputMidi; } });
        std::printf ("  Live -> Studio à 0,5 s : latence %d -> %d éch., mode actif %s ; FIFO : %d trames, hauteur entrante moyenne %.2f (attendu 56,20)\n",
                     eng.getLatencySamples (TrackingMode::Live), eng.getLatencySamples (TrackingMode::Studio),
                     eng.getActiveMode() == TrackingMode::Studio ? "Studio" : "Live", frames, voiced ? sumIn / voiced : 0.0);
        check (finite && eng.getActiveMode() == TrackingMode::Studio, "changement de mode à chaud");
        check (frames > 700 && std::abs (sumIn / std::max (voiced, 1) - 56.20) < 0.05, "FIFO : trames de hauteur publiées");

        AuraPitchEngine a, b; a.prepare (48000, 1); b.prepare (48000, 1);
        auto v = makeVowel (48000, 0.6, [] (double) { return 233.0; });
        auto od = runEngine<double> (a, { v }, 256);
        auto of = runEngine<float> (b, { v }, 256);
        double e = 0;
        for (size_t i = 0; i < v.size(); ++i) e = std::max (e, std::abs (od[0][i] - of[0][i]));
        std::printf ("  E/S float vs double : écart max %.1e\n", e);
        check (e < 1e-5, "E/S float = E/S double (à la précision float)");
    }
}

//==================================================================================================================
static void testRealtime()
{
    std::printf ("\n[5] Temps réel : charge d'un cœur, stéréo, voix corrigée en continu (Retune 10 ms, Formant +2, automation)\n");
    std::printf ("      fs     Live    Studio   allocations\n");
    for (double fs : kRates)
    {
        double load[2] {};
        long allocs = 0;
        for (int m = 0; m < 2; ++m)
        {
            AuraPitchEngine eng; eng.prepare (fs, 2);
            AuraPitchParams p; p.mode = m ? TrackingMode::Studio : TrackingMode::Live; p.retuneMs = 10; p.formantShift = 2;
            p.scale = ScaleType::Minor; p.key = 9;
            eng.setParameters (p); eng.reset();
            const double secs = 2.0;
            auto l = makeVowel (fs, secs, [] (double t) { return 180.0 * std::exp2 (std::sin (2 * kPi * 0.5 * t)); });
            std::vector<float> L = toFloat (l), R = toFloat (l);
            const int block = 512;
            gAllocations = 0; gCountAllocations = true;
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t pos = 0; pos < L.size(); pos += block)
            {
                const int n = (int) std::min<size_t> (block, L.size() - pos);
                p.mix = (pos / block) % 50 < 25 ? 1.0 : 0.8;
                p.humanize = (pos / block) % 70 < 35 ? 0.0 : 0.5;
                eng.setParameters (p);
                float* ptr[2] = { L.data() + pos, R.data() + pos };
                eng.process (ptr, 2, n);
            }
            const double s = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            gCountAllocations = false;
            load[m] = s / secs * 100;
            allocs += gAllocations.load();
        }
        std::printf ("  %6.1f k  %5.1f %%  %6.1f %%   %ld\n", fs / 1000, load[0], load[1], allocs);
        check (allocs == 0, "0 allocation dans process() / setParameters()");
        check (load[0] < 25 && load[1] < 25, "charge < 25 % d'un cœur (stéréo)");
    }
}

//==================================================================================================================
static void testConcurrentUi()
{
    std::printf ("\n[6] Thread audio + thread interface (FIFO de hauteur lue pendant le traitement)\n");
    const double fs = 384000;
    AuraPitchEngine eng; eng.prepare (fs, 2);
    AuraPitchParams p; p.mode = TrackingMode::Live; eng.setParameters (p); eng.reset();
    auto x = makeVowel (fs, 1.0, [] (double t) { return 200.0 * std::exp2 (0.5 * std::sin (2 * kPi * t)); });
    std::vector<float> L = toFloat (x), R = toFloat (x);
    std::atomic<bool> done { false };
    long received = 0;
    std::int64_t lastPos = -1;
    bool ordered = true;
    std::thread ui ([&]
    {
        while (! done.load())
        {
            eng.getPitchFifo().drain ([&] (const PitchFrame& f) { ordered &= f.samplePosition > lastPos; lastPos = f.samplePosition; ++received; });
            std::this_thread::sleep_for (std::chrono::microseconds (500));
        }
        eng.getPitchFifo().drain ([&] (const PitchFrame& f) { ordered &= f.samplePosition > lastPos; lastPos = f.samplePosition; ++received; });
    });
    for (size_t pos = 0; pos < L.size(); pos += 256)
    {
        float* ptr[2] = { L.data() + pos, R.data() + pos };
        eng.process (ptr, 2, (int) std::min<size_t> (256, L.size() - pos));
    }
    done = true;
    ui.join();
    std::printf ("  %ld trames reçues par l'interface, dans l'ordre : %s, perdues : %llu\n", received, ordered ? "oui" : "non",
                 (unsigned long long) eng.getPitchFifo().getNumDropped());
    check (received > 600 && ordered && eng.getPitchFifo().getNumDropped() == 0, "FIFO audio -> interface sans perte ni désordre");
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);     // journal complet même en cas de plantage (CI)
    std::printf ("AuraPitch — banc DSP étape 2\n");
    testFftAndEnvelope();
    testPsola();
    testStudio();
    testEngine();
    testRealtime();
    testConcurrentUi();
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

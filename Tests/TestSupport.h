// AuraPitch — outils communs du banc de l'étape 2 : vérifications, signaux, mesures.
#pragma once

#include "../Source/DSP/FormantProcessor.h"
#include "../Source/DSP/PitchDetector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

//--- compteur d'allocations (thread courant) ------------------------------------------------------------------------
inline std::atomic<long> gAllocations { 0 };
inline thread_local bool gCountAllocations = false;
void* operator new (std::size_t n)
{
    if (gCountAllocations) gAllocations.fetch_add (1);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace test
{
inline int gChecks = 0, gFailures = 0;
inline void check (bool ok, const char* what)
{
    ++gChecks;
    if (! ok) { ++gFailures; std::printf ("  ÉCHEC : %s\n", what); }
}

constexpr double kPi = 3.14159265358979323846;

/** Voyelle /a/ synthétique : harmoniques en 1/k (−6 dB/octave : source glottique −12 dB + rayonnement des lèvres
    +6 dB), formants 700 / 1220 / 2600 Hz, F0 arbitraire. formantScale déplace les formants ; tilt = 2 donne une
    source très sombre (presque rien au-dessus de 6 kHz) pour les tests de clics. */
template <typename F0Fn>
std::vector<double> makeVowel (double fs, double seconds, F0Fn f0At, double amplitude = 0.3, double formantScale = 1.0,
                               double tilt = 1.0)
{
    const int n = (int) (fs * seconds);
    std::vector<double> out ((size_t) n);
    const double fMax = std::min (16000.0, 0.45 * fs);
    auto formant = [formantScale] (double f)
    {
        auto res = [f] (double fc, double bw) { const double d = (f - fc) / (0.5 * bw); return 1.0 / std::sqrt (1.0 + d * d); };
        return 0.15 + res (700 * formantScale, 110) + 0.6 * res (1220 * formantScale, 120) + 0.3 * res (2600 * formantScale, 160);
    };
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f0 = f0At ((double) i / fs);
        phase += 2.0 * kPi * f0 / fs;
        if (phase > 2.0 * kPi * 1.0e6) phase = std::fmod (phase, 2.0 * kPi);
        const int K = std::min (80, (int) (fMax / f0));
        const double c2 = 2.0 * std::cos (phase);
        double sPrev = 0.0, s = std::sin (phase), acc = 0.0;
        for (int k = 1; k <= K; ++k)
        {
            acc += formant (k * f0) / std::pow ((double) k, tilt) * s;
            const double sNext = c2 * s - sPrev; sPrev = s; s = sNext;
        }
        out[(size_t) i] = 0.5 * amplitude * acc;
    }
    return out;
}

inline std::vector<double> makeNoise (double fs, double seconds, double rms, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, rms);
    std::vector<double> out ((size_t) (fs * seconds));
    for (auto& x : out) x = g (rng);
    return out;
}

/** F0 mesurée par le PitchDetector (étape 1) : moyenne et écart max (cents) par rapport à expectedHz, sur les
    trames voisées dont le centre est dans [t0, t1] secondes. */
struct PitchStats { double meanCents = 0, maxCents = 0, voicedRatio = 0; int frames = 0; };
inline PitchStats measurePitch (const std::vector<double>& y, double fs, double expectedHz, double t0, double t1,
                                aura::pitch::InputType type = aura::pitch::InputType::Instrument)
{
    aura::pitch::PitchDetector det;
    det.prepare (fs);
    det.setInputType (type);
    PitchStats s;
    int voiced = 0;
    double sum = 0;
    det.process (y.data(), (int) y.size(), [&] (const aura::pitch::PitchEstimate& e, int)
    {
        const double t = (double) e.frameCentre / fs;
        if (t < t0 || t > t1) return;
        ++s.frames;
        if (! e.voiced) return;
        ++voiced;
        const double c = 1200.0 * std::log2 (e.frequencyHz / expectedHz);
        sum += c;
        s.maxCents = std::max (s.maxCents, std::abs (c));
    });
    s.voicedRatio = s.frames ? (double) voiced / s.frames : 0.0;
    s.meanCents = voiced ? sum / voiced : 0.0;
    return s;
}

/** Formants mesurés sur un extrait stable : FFT ≈ 85 ms, enveloppe vraie initialisée sur les maxima harmoniques
    (même méthode que le vocodeur). F1 = maximum de l'enveloppe dans 450–1000 Hz, F2 dans 1000–1800 Hz. */
struct Formants { double f1 = 0, f2 = 0; std::vector<double> envDb; double binHz = 0; };
inline Formants measureFormants (const std::vector<double>& y, double fs, double startSec, double f0)
{
    int order = 8;
    while ((1 << (order + 1)) <= (int) (0.12 * fs)) ++order;
    const int n = 1 << order, bins = n / 2 + 1;
    aura::pitch::RealFFT fft; fft.prepare (order);
    aura::pitch::SpectralEnvelope env; env.prepare (order);
    std::vector<double> frame ((size_t) n), re ((size_t) bins), im ((size_t) bins), mag ((size_t) bins), lm ((size_t) bins), e ((size_t) bins);
    const size_t start = (size_t) (startSec * fs);
    for (int i = 0; i < n; ++i)
        frame[(size_t) i] = y[start + (size_t) i] * (0.5 - 0.5 * std::cos (2 * kPi * i / n));
    fft.forward (frame.data(), re.data(), im.data());
    double maxMag = 0;
    for (int k = 0; k < bins; ++k) { mag[(size_t) k] = std::hypot (re[(size_t) k], im[(size_t) k]); maxMag = std::max (maxMag, mag[(size_t) k]); }
    const int w = std::max (2, (int) (0.4 * f0 * n / fs));
    std::vector<int> pk;
    for (int k = 1; k < bins - 1; ++k)
    {
        if (mag[(size_t) k] < maxMag * 1e-5) continue;
        bool isMax = true;
        for (int j = std::max (0, k - w); j <= std::min (bins - 1, k + w) && isMax; ++j)
            if (mag[(size_t) j] > mag[(size_t) k] || (j < k && mag[(size_t) j] >= mag[(size_t) k])) isMax = false;
        if (isMax) pk.push_back (k);
    }
    auto L = [&] (int k) { return std::log (mag[(size_t) k] + 1e-30); };
    for (int k = 0; k <= pk.front(); ++k) lm[(size_t) k] = L (pk.front());
    for (int k = pk.back(); k < bins; ++k) lm[(size_t) k] = L (pk.back());
    for (size_t p = 0; p + 1 < pk.size(); ++p)
        for (int k = pk[p]; k <= pk[p + 1]; ++k)
            lm[(size_t) k] = L (pk[p]) + (L (pk[p + 1]) - L (pk[p])) * (double) (k - pk[p]) / (double) (pk[p + 1] - pk[p]);
    env.trueEnvelope (lm.data(), e.data(), env.orderFor (f0, fs), 8, 0.5);

    Formants f;
    f.binHz = fs / n;
    f.envDb.resize ((size_t) bins);
    for (int k = 0; k < bins; ++k) f.envDb[(size_t) k] = 8.685889638 * e[(size_t) k];
    auto argmax = [&] (double lo, double hi)
    {
        int best = (int) (lo / f.binHz);
        for (int k = (int) (lo / f.binHz); k <= (int) (hi / f.binHz); ++k)
            if (f.envDb[(size_t) k] > f.envDb[(size_t) best]) best = k;
        return best * f.binHz;
    };
    f.f1 = argmax (450, 1000);
    f.f2 = argmax (1000, 1800);
    return f;
}

/** Facteur de déplacement des formants entre deux enveloppes (demi-tons) : γ qui minimise l'écart RMS entre
    out(f) et in(f / γ) sur 300–3500 Hz (niveaux moyens retirés), pas de 5 cents de −8 à +8 demi-tons. */
inline double formantShiftSemitones (const Formants& in, const Formants& out)
{
    auto at = [] (const Formants& f, double hz)
    {
        const double x = hz / f.binHz;
        const size_t i = (size_t) x;
        if (i + 1 >= f.envDb.size()) return f.envDb.back();
        return f.envDb[i] + (x - (double) i) * (f.envDb[i + 1] - f.envDb[i]);
    };
    double best = 0, bestErr = 1e300;
    for (int c = -800; c <= 800; c += 5)
    {
        const double g = std::exp2 (c / 1200.0);
        std::vector<double> a, b;
        for (double f = 300; f <= 3500; f *= 1.01) { a.push_back (at (out, f)); b.push_back (at (in, f / g)); }
        double ma = 0, mb = 0;
        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
        ma /= (double) a.size(); mb /= (double) b.size();
        double e = 0;
        for (size_t i = 0; i < a.size(); ++i) { const double d = (a[i] - ma) - (b[i] - mb); e += d * d; }
        if (e < bestErr) { bestErr = e; best = c / 100.0; }
    }
    return best;
}

/** Amplitude (dB) d'une composante de fréquence f, DFT fenêtrée de Hann sur [start, start + n). */
inline double toneDb (const std::vector<double>& y, double fs, double f, size_t start, size_t n)
{
    double c = 0, s = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * kPi * (double) i / (double) n);
        c += w * y[start + i] * std::cos (2 * kPi * f * (double) i / fs);
        s += w * y[start + i] * std::sin (2 * kPi * f * (double) i / fs);
    }
    return 20 * std::log10 (4 * std::hypot (c, s) / (double) n + 1e-300);
}

/** Déplacement des formants (demi-tons) mesuré sur les harmoniques elles-mêmes : l'enveloppe d'entrée est
    interpolée (dB, axe log-fréquence) entre ses harmoniques ; on cherche γ tel que l'amplitude de chaque harmonique
    de sortie (300–3500 Hz) suive l'enveloppe d'entrée lue en f / γ (écart de niveau global retiré). */
inline double formantShiftFromHarmonics (const std::vector<double>& x, double f0In, double startIn,
                                         const std::vector<double>& y, double f0Out, double startOut, double fs)
{
    const size_t n = (size_t) (0.2 * fs);
    std::vector<double> fi, ai, fo, ao;
    for (int k = 1; k * f0In < 5000; ++k) { fi.push_back (k * f0In); ai.push_back (toneDb (x, fs, k * f0In, (size_t) (startIn * fs), n)); }
    for (int k = 1; k * f0Out < 3500; ++k) if (k * f0Out >= 300) { fo.push_back (k * f0Out); ao.push_back (toneDb (y, fs, k * f0Out, (size_t) (startOut * fs), n)); }
    auto env = [&] (double f)
    {
        if (f <= fi.front()) return ai.front();
        for (size_t i = 0; i + 1 < fi.size(); ++i)
            if (f <= fi[i + 1])
            {
                const double t = std::log (f / fi[i]) / std::log (fi[i + 1] / fi[i]);
                return ai[i] + t * (ai[i + 1] - ai[i]);
            }
        return ai.back();
    };
    double best = 0, bestErr = 1e300;
    for (int c = -800; c <= 800; c += 5)
    {
        const double g = std::exp2 (c / 1200.0);
        double mean = 0;
        for (size_t i = 0; i < fo.size(); ++i) mean += ao[i] - env (fo[i] / g);
        mean /= (double) fo.size();
        double e = 0;
        for (size_t i = 0; i < fo.size(); ++i) { const double d = ao[i] - env (fo[i] / g) - mean; e += d * d; }
        if (e < bestErr) { bestErr = e; best = c / 100.0; }
    }
    return best;
}

/** Distance RMS (dB) entre deux enveloppes sur 300–4000 Hz, niveaux moyens retirés. */
inline double envelopeDistanceDb (const Formants& a, const Formants& b)
{
    std::vector<double> da, db;
    for (double f = 300; f <= 4000; f += 25)
    {
        da.push_back (a.envDb[(size_t) std::lround (f / a.binHz)]);
        db.push_back (b.envDb[(size_t) std::lround (f / b.binHz)]);
    }
    double ma = 0, mb = 0;
    for (size_t i = 0; i < da.size(); ++i) { ma += da[i]; mb += db[i]; }
    ma /= (double) da.size(); mb /= (double) db.size();
    double s = 0;
    for (size_t i = 0; i < da.size(); ++i) { const double d = (da[i] - ma) - (db[i] - mb); s += d * d; }
    return std::sqrt (s / (double) da.size());
}

/** Détecteur de clics : énergie au-dessus de 6 kHz (passe-haut RBJ du 4e ordre), RMS sur 0,5 ms glissantes.
    Renvoie le maximum sur [t0, t1] (dB). La voyelle de test n'a presque rien au-dessus de 6 kHz : un clic y apparaît
    comme un pic large bande. */
inline std::vector<double> highBandRms (const std::vector<double>& y, double fs)
{
    struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                double run (double x) { const double o = b0 * x + z1; z1 = b1 * x - a1 * o + z2; z2 = b2 * x - a2 * o; return o; } };
    const double w = 2 * kPi * 6000.0 / fs, c = std::cos (w), al = std::sin (w) / (2 * 0.7071), a0 = 1 + al;
    Bq q { (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0 };
    Bq q2 = q;
    std::vector<double> h (y.size());
    for (size_t i = 0; i < y.size(); ++i) h[i] = q2.run (q.run (y[i]));
    const int win = std::max (8, (int) (0.0005 * fs));
    std::vector<double> rms (y.size(), 0.0);
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i)
    {
        acc += h[i] * h[i];
        if (i >= (size_t) win) acc -= h[i - (size_t) win] * h[i - (size_t) win];
        rms[i] = std::sqrt (std::max (0.0, acc) / win);
    }
    return rms;
}
inline double maxDb (const std::vector<double>& rms, double fs, double t0, double t1)
{
    double m = 1e-30;
    for (size_t i = (size_t) (t0 * fs); i < std::min (rms.size(), (size_t) (t1 * fs)); ++i) m = std::max (m, rms[i]);
    return 20 * std::log10 (m);
}

/** Copie en float (conversion explicite, sans avertissement MSVC C4244). */
inline std::vector<float> toFloat (const std::vector<double>& v)
{
    std::vector<float> f (v.size());
    for (size_t i = 0; i < v.size(); ++i) f[i] = (float) v[i];
    return f;
}

inline double rmsDb (const std::vector<double>& y, size_t a, size_t b)
{
    double s = 0;
    for (size_t i = a; i < b; ++i) s += y[i] * y[i];
    return 10 * std::log10 (s / (double) (b - a) + 1e-300);
}
} // namespace test

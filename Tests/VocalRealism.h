// AuraPitch — signaux vocaux réalistes et mesures « à l'oreille » pour le banc de l'étape 4.
//   * makeRealVoice : source glottique à jitter (période) et shimmer (amplitude), harmoniques jusqu'à 20 kHz
//     (pente −6 dB/oct. + formants), souffle large bande, consonnes sourdes, attaques ;
//   * ghostRejectionDb : énergie aux harmoniques de la note corrigée / énergie aux harmoniques de la note chantée
//     (voix fantôme, effet « 2 voix ») ;
//   * thirdOctaveDeviation : écart de niveau sortie / entrée par tiers d'octave, 100 Hz – 20 kHz (« voile »).
#pragma once

#include "TestSupport.h"

#include <functional>

namespace test
{
struct VoiceSpec
{
    double jitter     = 0.001;    // écart-type relatif de la période (0,1 %)
    double shimmerDb  = 0.3;      // écart-type de l'amplitude (dB), lent
    double breathDb   = -30.0;    // souffle (bruit 1–16 kHz) relatif à la voix, RMS
    double amplitude  = 0.25;
    double maxHz      = 20000.0;
    unsigned seed     = 1;
};

/** Voix : f0At (t) en Hz (0 = silence / consonne), envAt (t) = amplitude 0 … 1 de la voix voisée,
    noiseAt (t) = amplitude 0 … 1 d'une consonne sourde (bruit 3–12 kHz). */
inline std::vector<double> makeRealVoice (double fs, double seconds, const std::function<double (double)>& f0At,
                                          const std::function<double (double)>& envAt,
                                          const std::function<double (double)>& noiseAt, const VoiceSpec& spec = {})
{
    const size_t n = (size_t) (fs * seconds);
    std::vector<double> voice (n, 0.0), breath (n, 0.0), cons (n, 0.0);
    std::mt19937 rng (spec.seed);
    std::normal_distribution<double> g (0.0, 1.0);

    auto formant = [] (double f)
    {
        auto res = [f] (double fc, double bw) { const double d = (f - fc) / (0.5 * bw); return 1.0 / std::sqrt (1.0 + d * d); };
        return 0.12 + res (650, 90) + 0.7 * res (1150, 110) + 0.35 * res (2500, 160) + 0.2 * res (3400, 220);
    };

    // Jitter et shimmer : bruits passe-bas (≈ 30 Hz et 4 Hz)
    double jit = 0.0, shim = 0.0, phase = 0.0;
    const double aj = std::exp (-2 * kPi * 30.0 / fs), as = std::exp (-2 * kPi * 4.0 / fs);
    const double kj = spec.jitter * std::sqrt (1 - aj * aj), ks = spec.shimmerDb / 8.6859 * std::sqrt (1 - as * as);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        jit  = aj * jit + kj * g (rng);
        shim = as * shim + ks * g (rng);
        const double f0 = f0At (t);
        const double env = envAt (t);
        if (f0 <= 0.0 || env <= 0.0) { continue; }
        phase += 2 * kPi * f0 * (1.0 + jit) / fs;
        if (phase > 2 * kPi * 1e6) phase = std::fmod (phase, 2 * kPi);
        const int K = std::min (400, (int) (std::min (spec.maxHz, 0.45 * fs) / f0));
        const double c2 = 2 * std::cos (phase);
        double sPrev = 0.0, s = std::sin (phase), acc = 0.0;
        for (int k = 1; k <= K; ++k)
        {
            acc += formant (k * f0) / (double) k * s;
            const double sn = c2 * s - sPrev; sPrev = s; s = sn;
        }
        voice[i] = spec.amplitude * 0.5 * acc * env * std::exp (shim);
    }

    // Souffle : bruit 1–16 kHz suivant l'enveloppe voisée ; consonnes : bruit 3–12 kHz
    auto band = [&] (double lo, double hi, unsigned seed)
    {
        std::vector<double> b (n);
        std::mt19937 r (seed);
        std::normal_distribution<double> gg (0.0, 1.0);
        struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
                    double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
        auto mk = [&] (double fc, bool hp)
        {
            const double w = 2 * kPi * std::min (fc, 0.45 * fs) / fs, c = std::cos (w), al = std::sin (w) / (2 * 0.7071), a0 = 1 + al;
            Bq q;
            if (hp) { q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = q.b0; }
            else    { q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0;  q.b2 = q.b0; }
            q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
            return q;
        };
        Bq h1 = mk (lo, true), h2 = h1, l1 = mk (hi, false), l2 = l1;
        double e = 0;
        for (size_t i = 0; i < n; ++i) { b[i] = l2.run (l1.run (h2.run (h1.run (gg (r))))); e += b[i] * b[i]; }
        const double k = 1.0 / std::sqrt (e / (double) n + 1e-30);
        for (auto& v : b) v *= k;
        return b;
    };
    double ev = 0; size_t nv = 0;
    for (size_t i = 0; i < n; ++i) if (voice[i] != 0.0) { ev += voice[i] * voice[i]; ++nv; }
    const double voiceRms = std::sqrt (ev / (double) std::max<size_t> (nv, 1));
    auto br = band (1000, 16000, spec.seed + 11), cn = band (3000, 12000, spec.seed + 23);
    std::vector<double> out (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        out[i] = voice[i] + voiceRms * std::pow (10.0, spec.breathDb / 20) * envAt (t) * br[i]
               + voiceRms * 0.8 * noiseAt (t) * cn[i];
    }
    return out;
}

/** Puissance d'une raie (fenêtre de Blackman-Harris 4 termes, −92 dB de lobes secondaires). */
inline double linePower (const std::vector<double>& y, double fs, double f, size_t start, size_t len)
{
    double c = 0, s = 0, wsum = 0;
    const double span = (double) std::max<size_t> (len, 2) - 1.0;
    for (size_t i = 0; i < len; ++i)
    {
        const double u = 2 * kPi * (double) i / span;
        const double w = 0.35875 - 0.48829 * std::cos (u) + 0.14128 * std::cos (2 * u) - 0.01168 * std::cos (3 * u);
        const double ph = 2 * kPi * f * (double) i / fs;
        c += w * y[start + i] * std::cos (ph);
        s += w * y[start + i] * std::sin (ph);
        wsum += w;
    }
    return (c * c + s * s) / std::max (wsum * wsum, 1e-300);
}

/** Rejet de la voix fantôme : Σ puissance aux harmoniques de fOut / Σ puissance aux harmoniques de fIn (dB). */
inline double ghostRejectionDb (const std::vector<double>& y, double fs, double fIn, double fOut, size_t start, size_t len, int harmonics = 8)
{
    double pin = 0, pout = 0;
    for (int h = 1; h <= harmonics; ++h)
    {
        pin  += linePower (y, fs, h * fIn, start, len);
        pout += linePower (y, fs, h * fOut, start, len);
    }
    return 10 * std::log10 (pout / (pin + 1e-300));
}

/** Écart (dB) sortie / référence par tiers d'octave de 100 Hz à 20 kHz (FFT de Hann de toute la plage). */
inline std::vector<std::pair<double, double>> thirdOctaveDeviation (const std::vector<double>& ref, size_t refStart,
                                                                     const std::vector<double>& y, size_t yStart,
                                                                     size_t len, double fs)
{
    int order = 10;
    while ((size_t (1) << (order + 1)) <= len) ++order;
    const int N = 1 << order;
    aura::pitch::RealFFT fft; fft.prepare (order);
    std::vector<double> fr ((size_t) N), re ((size_t) N / 2 + 1), im ((size_t) N / 2 + 1), pr ((size_t) N / 2 + 1), py ((size_t) N / 2 + 1);
    auto spectrum = [&] (const std::vector<double>& s, size_t start, std::vector<double>& p)
    {
        for (int i = 0; i < N; ++i) fr[(size_t) i] = s[start + (size_t) i] * (0.5 - 0.5 * std::cos (2 * kPi * i / N));
        fft.forward (fr.data(), re.data(), im.data());
        for (size_t k = 0; k < p.size(); ++k) p[k] = re[k] * re[k] + im[k] * im[k];
    };
    spectrum (ref, refStart, pr);
    spectrum (y, yStart, py);
    std::vector<std::pair<double, double>> bands;
    for (double fc = 100.0; fc <= 20000.0 * 1.001; fc *= std::pow (2.0, 1.0 / 3.0))
    {
        const double lo = fc / std::pow (2.0, 1.0 / 6.0), hi = fc * std::pow (2.0, 1.0 / 6.0);
        double a = 0, b = 0;
        for (size_t k = (size_t) std::ceil (lo * N / fs); k <= (size_t) std::floor (hi * N / fs) && k < pr.size(); ++k) { a += pr[k]; b += py[k]; }
        bands.push_back ({ fc, 10 * std::log10 ((b + 1e-300) / (a + 1e-300)) });
    }
    return bands;
}
} // namespace test

// AuraKey — NextDSP · AuraSuite
// ChromaAnalyzer.h : chromagramme polyphonique à partir du flux décimé (≈ 11 kHz).
//
//   * Trames de 8192 points (≈ 0,74 s, résolution 1,35 Hz : deux demi-tons sont séparés dès 75 Hz), pas de 2048
//     (≈ 0,19 s), fenêtre de Hann, FFT réelle double de l'étape 2 (aura::pitch::RealFFT).
//   * Pics spectraux seulement (maximum local, au-dessus de −60 dB du maximum de la trame et de 1,6 × la moyenne locale
//     sur ±12 cases) : le bruit large bande (batterie, souffle) n'étale pas le chromagramme. Fréquence et amplitude
//     affinées par interpolation parabolique du log-module.
//   * Chaque pic (75 Hz … 2 kHz) ajoute √amplitude au chromagramme fin de 36 cases (tiers de demi-ton), réparti
//     linéairement entre les deux cases voisines ; l'écart de chaque pic au demi-ton le plus proche alimente une moyenne
//     circulaire (accord du morceau, ex. A = 432 Hz ou un enregistrement accordé à +30 c).
//   * fold() replie les 36 cases sur les 12 classes de hauteur en tenant compte de l'accord estimé.
//   * Poids de la trame = durée × niveau (−60 dBFS -> 0, −20 dBFS et plus -> 1), comme l'Auto-Key de l'étape 5A.
//
// Temps réel : prepare() alloue ; push() n'alloue jamais (une FFT de 8192 points toutes les 2048 entrées décimées).
#pragma once

#include "FFT.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace aura::key
{

struct ChromaFrame
{
    std::array<float, 36> fine {};   // normalisé (somme 1) ; vide si aucun pic
    double weight  = 0.0;            // durée × niveau
    double dt      = 0.0;            // durée représentée par la trame (pas d'analyse)
    double time    = 0.0;            // centre de la fenêtre, en secondes depuis reset()
    double levelDb = -200.0;
    double tuneX = 0.0, tuneY = 0.0, tuneW = 0.0;   // somme pondérée de e^{j2π·écart}, et des poids
};

class ChromaAnalyzer
{
public:
    static constexpr double kMinHz = 75.0, kMaxHz = 2000.0;

    void prepare (double analysisRate)
    {
        fa = analysisRate;
        const int order = fa >= 8000.0 ? 13 : 12;
        n = 1 << order;
        hop = n / 4;
        fft.prepare (order);
        window.resize ((size_t) n);
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * (i + 0.5) / n);
        ring.assign ((size_t) (2 * n), 0.0);
        frame.assign ((size_t) n, 0.0);
        re.assign ((size_t) n / 2 + 1, 0.0);
        im.assign ((size_t) n / 2 + 1, 0.0);
        mag.assign ((size_t) n / 2 + 1, 0.0);
        prefix.assign ((size_t) n / 2 + 2, 0.0);
        kLo = std::max (2, (int) std::floor (kMinHz * n / fa));
        kHi = std::min (n / 2 - 2, (int) std::ceil (kMaxHz * n / fa));
        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0);
        pos = 0; sinceHop = 0; filled = 0;
        samples = 0;
    }

    double getAnalysisRate() const noexcept { return fa; }
    double getHopSeconds() const noexcept   { return hop / fa; }
    int getFrameSize() const noexcept       { return n; }

    /** `onFrame (const ChromaFrame&)` est appelé à chaque pas d'analyse. */
    template <typename Fn>
    void push (const double* x, int count, Fn&& onFrame) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            ring[(size_t) pos] = x[i];
            ring[(size_t) (pos + n)] = x[i];
            if (++pos == n) pos = 0;
            ++samples;
            filled = std::min (n, filled + 1);
            if (++sinceHop == hop)
            {
                sinceHop = 0;
                analyse();
                onFrame (out);
            }
        }
    }

    /** Repli des 36 cases sur 12 classes de hauteur (0 = C), décalé de l'accord estimé (en demi-tons). */
    static std::array<double, 12> fold (const std::array<float, 36>& fine, double tuningSemis) noexcept
    {
        std::array<double, 12> c {};
        for (int j = 0; j < 36; ++j)
        {
            const double v = fine[(size_t) j];
            if (v <= 0.0) continue;
            const double p = j / 3.0 - tuningSemis;
            const double lo = std::floor (p);
            const double frac = p - lo;
            const int pc = (((int) lo % 12) + 12) % 12;
            c[(size_t) pc] += v * (1.0 - frac);
            c[(size_t) ((pc + 1) % 12)] += v * frac;
        }
        return c;
    }

    /** Accord estimé (demi-tons, −0,5 … 0,5) à partir des sommes circulaires ; 0 si l'estimation est trop dispersée. */
    static double tuningFrom (double x, double y, double w) noexcept
    {
        if (! (w > 0.0)) return 0.0;
        const double r = std::sqrt (x * x + y * y) / w;
        if (r < 0.25) return 0.0;
        return std::atan2 (y, x) / (2.0 * 3.14159265358979323846);
    }

private:
    void analyse() noexcept
    {
        out = {};
        out.dt = hop / fa;
        out.time = (samples - 0.5 * filled) / fa;
        const double* h = ring.data() + pos;           // les n derniers échantillons, du plus ancien au plus récent
        double energy = 0.0;
        for (int i = 0; i < n; ++i)
        {
            energy += h[i] * h[i];
            frame[(size_t) i] = h[i] * window[(size_t) i];
        }
        const double rms = std::sqrt (energy / std::max (1, filled));
        out.levelDb = rms > 1.0e-10 ? 20.0 * std::log10 (rms) : -200.0;
        if (out.levelDb < -70.0)
            return;

        fft.forward (frame.data(), re.data(), im.data());
        double peak = 0.0;
        prefix[0] = 0.0;
        for (int k = 0; k <= n / 2; ++k)
        {
            mag[(size_t) k] = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
            prefix[(size_t) k + 1] = prefix[(size_t) k] + mag[(size_t) k];
            if (k >= kLo && k <= kHi) peak = std::max (peak, mag[(size_t) k]);
        }
        if (! (peak > 0.0))
            return;
        const double floorAbs = peak * 1.0e-3;          // −60 dB
        constexpr int span = 12;
        double total = 0.0;
        std::array<double, 36> fine {};
        for (int k = kLo; k <= kHi; ++k)
        {
            const double m = mag[(size_t) k];
            if (! (m > mag[(size_t) k - 1] && m >= mag[(size_t) k + 1] && m > floorAbs))
                continue;
            const int a = std::max (0, k - span), b = std::min (n / 2, k + span);
            const double localMean = (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (b - a + 1);
            if (m < 1.6 * localMean)
                continue;
            const double la = std::log (std::max (1.0e-300, mag[(size_t) k - 1])), lb = std::log (m),
                         lc = std::log (std::max (1.0e-300, mag[(size_t) k + 1]));
            const double den = la - 2.0 * lb + lc;
            const double d = den < 0.0 ? std::clamp (0.5 * (la - lc) / den, -0.5, 0.5) : 0.0;
            const double freq = (k + d) * fa / n;
            const double amp = std::exp (lb - 0.25 * (la - lc) * d);
            if (freq < kMinHz || freq > kMaxHz)
                continue;
            const double w = std::sqrt (amp / peak);
            const double midi = 69.0 + 12.0 * std::log2 (freq / 440.0);
            const double dev = midi - std::round (midi);
            out.tuneX += w * std::cos (2.0 * 3.14159265358979323846 * dev);
            out.tuneY += w * std::sin (2.0 * 3.14159265358979323846 * dev);
            out.tuneW += w;
            const double p = 3.0 * (midi - 12.0 * std::floor (midi / 12.0));   // 0 … 36
            const double lo = std::floor (p);
            const double frac = p - lo;
            const int j = ((int) lo % 36 + 36) % 36;
            fine[(size_t) j] += w * (1.0 - frac);
            fine[(size_t) ((j + 1) % 36)] += w * frac;
            total += w;
        }
        if (! (total > 0.0))
            return;
        for (int j = 0; j < 36; ++j)
            out.fine[(size_t) j] = (float) (fine[(size_t) j] / total);
        out.weight = out.dt * std::clamp ((out.levelDb + 60.0) / 40.0, 0.0, 1.0);
    }

    aura::pitch::RealFFT fft;
    std::vector<double> window, ring, frame, re, im, mag, prefix;
    ChromaFrame out;
    double fa = 11025.0;
    long long samples = 0;
    int n = 8192, hop = 2048, pos = 0, sinceHop = 0, filled = 0, kLo = 2, kHi = 100;
};

} // namespace aura::key

// AuraPitch — NextDSP · AuraSuite
// Interpolator.h : lecture fractionnaire par sinc fenêtré (Kaiser), sans JUCE.
//
// Deux noyaux prototypes h(x), x en échantillons :
//   * Standard : 48 points, Kaiser β = 8,6  (≈ 85 dB)  ; plat à ±0,001 dB jusqu'à 19,5 kHz à 44,1 kHz ;
//   * Ultra    : 96 points, Kaiser β = 12,5 (≈ 125 dB) ; plat à ±0,0001 dB jusqu'à 20 kHz à 44,1 kHz.
// Table suréchantillonnée 512 fois, interpolation linéaire entre deux valeurs (erreur < −100 dB).
//
// Lecture accélérée d'un facteur r > 1 (hauteur vers le haut) — lot 1
//   Le noyau est ÉTIRÉ d'un facteur r : h_r(x) = h(x / r) / r, soit 48·r points (96·r en Ultra). La coupure descend à
//   fs / 2r à l'entrée — donc reste à fs / 2 à la sortie — et la bande de transition garde sa largeur vue de la sortie :
//   la réponse est la même qu'à r = 1 (plate jusqu'à 20 kHz) et le repliement reste sous le plancher du noyau quel
//   que soit r. Avant le lot 1, le noyau gardait 48 points avec une coupure abaissée (29 bancs) : la transition
//   s'élargissait d'un facteur r vue de la sortie, soit −0,04 dB à 19,4 kHz et un repliement à −42 dB à +12 dt
//   (48 kHz), −1,1 dB et −18 dB à +24 dt.
//   * à r ≤ 1, et à une position entière, le noyau standard rend exactement l'échantillon (sinc nul aux entiers).
// Causalité : lire la position p à la vitesse r demande les échantillons jusqu'à floor(p) + lookaheadFor (r).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aura::pitch
{

class SincInterpolator
{
public:
    enum Quality : int { Standard = 0, Ultra = 1 };

    static constexpr int kTaps      = 48;             // noyau standard
    static constexpr int kHalf      = kTaps / 2;
    static constexpr int kUltraTaps = 96;
    static constexpr int kUltraHalf = kUltraTaps / 2;
    static constexpr int kPhases    = 512;
    static constexpr int kBanks     = 29;             // vitesses mémorisées par les grains PSOLA : 1,06^b
    static constexpr double kBankStep = 1.06;
    static constexpr double kMaxSpeed = 5.2;
    static constexpr int kLookahead = kHalf;          // échantillons futurs nécessaires à r ≤ 1, noyau standard
    static constexpr int kMaxKernel = (int) (kUltraTaps * kMaxSpeed) + 4;

    void prepare()
    {
        build (tables[0], kTaps, 8.6);
        build (tables[1], kUltraTaps, 12.5);
        for (int b = 0; b < kBanks; ++b)
            bankSpeed[b] = std::pow (kBankStep, (double) b);
    }

    void setQuality (Quality q) noexcept { quality = q; }
    Quality getQuality() const noexcept  { return quality; }

    /** Échantillons futurs nécessaires pour lire à la vitesse speed. */
    static double lookaheadFor (double speed, Quality q = Standard) noexcept
    {
        return (q == Ultra ? kUltraHalf : kHalf) * std::max (1.0, std::min (speed, kMaxSpeed)) + 1.0;
    }
    /** Le plus grand des deux noyaux : marge de causalité indépendante de la qualité (latence identique). */
    static double maxLookaheadFor (double speed) noexcept { return lookaheadFor (speed, Ultra); }

    /** Vitesse mémorisée (PSOLA) : indice b tel que 1,06^b ≥ speed. */
    static int bankFor (double speed) noexcept
    {
        if (speed <= 1.0) return 0;
        return std::clamp ((int) std::ceil (std::log (speed) / std::log (kBankStep) - 1.0e-9), 0, kBanks - 1);
    }
    double speedOfBank (int bank) const noexcept { return bankSpeed[std::clamp (bank, 0, kBanks - 1)]; }

    /** Lecture d'un anneau de taille 2^k (masque) à la position absolue pos. */
    double read (const double* ring, std::int64_t mask, double pos, int bank = 0) const noexcept
    {
        double out = 0.0;
        readChannels (&ring, 1, mask, pos, bank, &out);
        return out;
    }

    /** Compatibilité (grains PSOLA) : vitesse = 1,06^bank. */
    void readChannels (const double* const* rings, int nc, std::int64_t mask, double pos, int bank,
                       double* out) const noexcept
    {
        readAtSpeed (rings, nc, mask, pos, bank <= 0 ? 1.0 : speedOfBank (bank), out);
    }

    /** Même lecture sur nc anneaux à la même position, à la vitesse speed : le noyau est calculé une fois pour tous
        les canaux (stéréo liée, mêmes positions de lecture). out[ch] = valeur lue. */
    void readAtSpeed (const double* const* rings, int nc, std::int64_t mask, double pos, double speed,
                      double* out) const noexcept
    {
        const double fl = std::floor (pos);
        const std::int64_t i = (std::int64_t) fl;
        const double f = pos - fl;
        const int half = quality == Ultra ? kUltraHalf : kHalf;
        const double* t = tables[(size_t) quality].data();

        if (speed <= 1.0 + 1.0e-9)
        {
            if (f <= 0.0)                          // position entière : l'échantillon lui-même
            {
                for (int ch = 0; ch < nc; ++ch) out[ch] = rings[ch][(size_t) (i & mask)];
                return;
            }
            // Point k (0 … N−1) <-> échantillon i − (half − 1) + k, décalage (k − (half − 1)) − f.
            const int taps = 2 * half;
            const double base = (1.0 - f) * kPhases;
            const int    ib   = (int) base;
            const double fb   = base - (double) ib;
            alignas (32) double h[kUltraTaps];
            double sum = 0.0;
            for (int k = 0; k < taps; ++k)
            {
                const int idx = ib + k * kPhases;
                h[k] = t[idx] + fb * (t[idx + 1] - t[idx]);
                sum += h[k];
            }
            const double norm = 1.0 / sum;         // gain continu exactement 1 à toute position fractionnaire
            accumulate (rings, nc, mask, i - (half - 1), h, taps, norm, out);
            return;
        }

        // Noyau étiré : h(x / s) / s sur ±half·s échantillons autour de pos.
        const double s = std::min (speed, kMaxSpeed);
        const double inv = 1.0 / s;
        const double reach = (double) half * s;
        const std::int64_t first = (std::int64_t) std::ceil (pos - reach);
        const std::int64_t last  = (std::int64_t) std::floor (pos + reach);
        const int taps = (int) std::min<std::int64_t> (last - first + 1, kMaxKernel);
        alignas (32) double h[kMaxKernel];
        double sum = 0.0;
        const double scale = inv * kPhases;
        const double u0 = (((double) first - pos) * inv + (double) half) * kPhases;     // index de table du premier point
        // first = ceil(pos − reach) et last = floor(pos + reach) : u reste dans [0, 2·half·kPhases] par construction.
        // Seuls les deux bouts peuvent en sortir d'un arrondi : ils sont bornés hors de la boucle, qui reste sans
        // branchement (le clamp à chaque point coûtait jusqu'à +40 % de charge aux grains PSOLA à 352,8 kHz).
        const double uMax = (double) (2 * half * kPhases);
        auto tap = [t] (double u) noexcept
        {
            const int idx = (int) u;
            const double fr = u - (double) idx;
            return t[idx] + fr * (t[idx + 1] - t[idx]);
        };
        h[0] = tap (std::clamp (u0, 0.0, uMax));
        h[taps - 1] = tap (std::clamp (u0 + (double) (taps - 1) * scale, 0.0, uMax));
        double s0 = h[0] + h[taps - 1], s1 = 0.0;
        int k = 1;
        for (; k + 1 < taps - 1; k += 2)                      // deux chaînes indépendantes (latence des chargements)
        {
            h[k]     = tap (u0 + (double) k * scale);
            h[k + 1] = tap (u0 + (double) (k + 1) * scale);
            s0 += h[k]; s1 += h[k + 1];
        }
        for (; k < taps - 1; ++k) { h[k] = tap (u0 + (double) k * scale); s0 += h[k]; }
        sum = s0 + s1;
        accumulate (rings, nc, mask, first, h, taps, sum > 0.0 ? 1.0 / sum : 0.0, out);
    }

private:
    static void accumulate (const double* const* rings, int nc, std::int64_t mask, std::int64_t firstIndex,
                            const double* h, int taps, double norm, double* out) noexcept
    {
        const std::int64_t s0 = firstIndex & mask;
        if (s0 + taps <= mask + 1)             // cas courant : les points sont contigus dans l'anneau
        {
            for (int ch = 0; ch < nc; ++ch)
            {
                const double* r = rings[ch] + s0;
                double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
                int k = 0;
                for (; k + 4 <= taps; k += 4)
                {
                    a0 += h[k] * r[k];         a1 += h[k + 1] * r[k + 1];
                    a2 += h[k + 2] * r[k + 2]; a3 += h[k + 3] * r[k + 3];
                }
                for (; k < taps; ++k) a0 += h[k] * r[k];
                out[ch] = ((a0 + a1) + (a2 + a3)) * norm;
            }
            return;
        }
        for (int ch = 0; ch < nc; ++ch)
        {
            double acc = 0.0;
            for (int k = 0; k < taps; ++k)
                acc += h[k] * rings[ch][(size_t) ((s0 + k) & mask)];
            out[ch] = acc * norm;
        }
    }

    /** Table du noyau h(x), x = i / kPhases − taps / 2, i = 0 … taps·kPhases (+ 1 garde). */
    static void build (std::vector<double>& t, int taps, double beta)
    {
        constexpr double pi = 3.14159265358979323846;
        const int half = taps / 2;
        const int len = taps * kPhases + 2;
        t.assign ((size_t) len, 0.0);
        const double i0b = besselI0 (beta);
        for (int i = 0; i < len; ++i)
        {
            const double x = (double) i / kPhases - half;
            const double u = x / half;
            if (std::abs (u) >= 1.0) continue;
            const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
            t[(size_t) i] = sinc * besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b;
        }
    }

    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 200; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
            if (term < sum * 1.0e-17) break;
        }
        return sum;
    }

    std::vector<double> tables[2];
    double bankSpeed[kBanks] {};
    Quality quality = Standard;
};

} // namespace aura::pitch

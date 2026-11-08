// AuraPitch — NextDSP · AuraSuite
// Interpolator.h : lecture fractionnaire par sinc fenêtré (Kaiser), 48 points, sans JUCE.
//
// Remplace l'interpolation Hermite cubique, qui coûte en moyenne −0,5 dB à 12 kHz, −1,4 dB à 16 kHz et −2,5 dB à
// 20 kHz à 48 kHz (position fractionnaire uniforme) : une part du « voile » entendu sur une vraie voix.
//   * 48 points, Kaiser β = 8,6 (≈ 85 dB) ; réponse plate à ±0,01 dB jusqu'à 20 kHz à 48 kHz ;
//   * table suréchantillonnée 512 fois, interpolation linéaire entre deux valeurs de table (erreur < −100 dB) ;
//   * bancs anti-repliement : pour une lecture accélérée d'un facteur r > 1 (hauteur vers le haut), le noyau est
//     rétréci à fc = 0,5 / r (pas de repliement du contenu qui sortirait au-delà de Nyquist). 29 bancs de r = 1 à
//     r = 5 (pas de 6 %) ;
//   * à une position entière, le banc r = 1 rend exactement l'échantillon (sinc nul aux entiers).
// Causalité : lire la position p demande les échantillons jusqu'à floor(p) + 24.
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
    static constexpr int kTaps      = 48;
    static constexpr int kHalf      = kTaps / 2;      // 24 points de chaque côté
    static constexpr int kPhases    = 512;
    static constexpr int kBanks     = 29;
    static constexpr double kBankStep = 1.06;
    static constexpr int kLookahead = kHalf;          // échantillons futurs nécessaires

    void prepare()
    {
        constexpr double pi = 3.14159265358979323846;
        const int len = kTaps * kPhases + 2;
        tables.assign ((size_t) kBanks, std::vector<double> ((size_t) len, 0.0));
        const double beta = 8.6, i0b = besselI0 (beta);
        for (int b = 0; b < kBanks; ++b)
        {
            const double fc = 1.0 / std::pow (kBankStep, (double) b);          // fréquence de coupure relative à fs/2
            auto& t = tables[(size_t) b];
            for (int i = 0; i < len; ++i)
            {
                const double x = (double) i / kPhases - kHalf;                  // −24 … +24
                const double u = x / kHalf;
                if (std::abs (u) >= 1.0) { t[(size_t) i] = 0.0; continue; }
                const double sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (pi * fc * x) / (pi * fc * x);
                t[(size_t) i] = fc * sinc * besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b;
            }
            // Gain continu exactement 1 pour toute position fractionnaire (somme des 48 coefficients).
            for (int ph = 0; ph < kPhases; ++ph)
            {
                double s = 0.0;
                for (int k = 0; k < kTaps; ++k) s += t[(size_t) (ph + k * kPhases)];
                if (s > 0.0) for (int k = 0; k < kTaps; ++k) t[(size_t) (ph + k * kPhases)] /= s;
            }
            t[(size_t) (kTaps * kPhases)] = t[0];
        }
    }

    /** Banc adapté à une lecture accélérée d'un facteur speed (≤ 1 : banc 0, pleine bande). */
    static int bankFor (double speed) noexcept
    {
        if (speed <= 1.0) return 0;
        return std::clamp ((int) std::ceil (std::log (speed) / std::log (kBankStep) - 1.0e-9), 0, kBanks - 1);
    }

    /** Lecture d'un anneau de taille 2^k (masque) à la position absolue pos. */
    double read (const double* ring, std::int64_t mask, double pos, int bank = 0) const noexcept
    {
        const double fl = std::floor (pos);
        const std::int64_t i = (std::int64_t) fl;
        const double f = pos - fl;
        if (f <= 0.0 && bank == 0)             // f ∈ [0, 1) : position entière
            return ring[(size_t) (i & mask)];

        // Point k (0 … 47) <-> échantillon i − 23 + k, décalage (k − 23) − f ; index de table ((k − 23 − f) + 24)·P.
        const double base = (1.0 - f) * kPhases;
        const int    ib   = (int) base;
        const double fb   = base - (double) ib;
        const double* t   = tables[(size_t) bank].data();
        double acc = 0.0;
        std::int64_t s = i - (kHalf - 1);
        for (int k = 0; k < kTaps; ++k, ++s)
        {
            const int idx = ib + k * kPhases;
            const double h = t[idx] + fb * (t[idx + 1] - t[idx]);
            acc += h * ring[(size_t) (s & mask)];
        }
        return acc;
    }

private:
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

    std::vector<std::vector<double>> tables;
};

} // namespace aura::pitch

// AuraPitch — NextDSP · AuraSuite
// FormantProcessor.h : enveloppe spectrale vraie (True Envelope, Röbel & Rodet 2005) et déformation de formants.
//
//   * cepstralSmooth : log|X| -> cepstre réel -> liftrage (ordre O, bord adouci) -> log-enveloppe lissée ;
//   * trueEnvelope   : A₀ = log|X| ; Vᵢ = lissage(Aᵢ) ; Aᵢ₊₁ = max(Aᵢ, Vᵢ), jusqu'à ce que l'enveloppe passe à moins
//                      de tolDb sous chaque pic. Contrairement au cepstre simple, qui passe entre les harmoniques
//                      (et sous-estime les formants des voix aiguës), l'enveloppe vraie s'appuie sur les pics ;
//   * warp           : E'(f) = E(f / γ), interpolation linéaire en log : formants déplacés d'un facteur γ.
//
// L'ordre cepstral se règle en échantillons de quéfrence : O = ½ · fs / F0 (Röbel). Il ne dépend que de la
// fréquence d'échantillonnage de la bande vocale (44,1 / 48 kHz en DXD, voir StudioFormantFilter.h) : l'enveloppe est la
// même à toutes les fréquences d'entrée. Aucune allocation après prepare().
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "FFT.h"

namespace aura::pitch
{

class SpectralEnvelope
{
public:
    static constexpr double kLogFloor = -50.0;   // ≈ −434 dB : protège log(0)

    void prepare (int fftOrder)
    {
        fft.prepare (fftOrder);
        n    = fft.getSize();
        bins = fft.getNumBins();
        cep.assign ((size_t) n, 0.0);
        re.assign ((size_t) bins, 0.0);
        im.assign ((size_t) bins, 0.0);
        work.assign ((size_t) bins, 0.0);
    }

    int getFftSize() const noexcept { return n; }
    int getNumBins() const noexcept { return bins; }

    /** Ordre cepstral recommandé pour une F0 (Hz) à la fréquence fs. */
    int orderFor (double f0Hz, double fs) const noexcept
    {
        const double o = f0Hz > 0.0 ? 0.5 * fs / f0Hz : 0.5 * fs / 200.0;
        return std::clamp ((int) std::lround (o), 8, n / 4);
    }

    /** logIn[0 … N/2] (log naturel de la magnitude) -> logOut[0 … N/2] (peuvent être le même tableau). */
    void cepstralSmooth (const double* logIn, double* logOut, int order) noexcept
    {
        order = std::clamp (order, 1, n / 2 - 1);
        for (int k = 0; k < bins; ++k) { re[(size_t) k] = logIn[k]; im[(size_t) k] = 0.0; }
        fft.inverse (re.data(), im.data(), cep.data());

        // Liftre : 1 jusqu'à 0,8 O, puis demi-cosinus jusqu'à O (réduit les oscillations de Gibbs de l'enveloppe).
        const int taper0 = std::max (1, (int) (0.8 * order));
        for (int q = 1; q < n / 2; ++q)
        {
            double g = 0.0;
            if (q <= taper0)      g = 1.0;
            else if (q <= order)  g = 0.5 * (1.0 + std::cos (3.14159265358979323846 * (q - taper0) / (double) (order - taper0 + 1)));
            cep[(size_t) q]     *= g;
            cep[(size_t) (n - q)] *= g;
        }
        cep[(size_t) (n / 2)] = 0.0;

        fft.forward (cep.data(), re.data(), im.data());
        for (int k = 0; k < bins; ++k)
            logOut[k] = re[(size_t) k];
    }

    /** Enveloppe vraie. logMag[0 … N/2] -> envLog[0 … N/2]. Renvoie le nombre d'itérations effectuées. */
    int trueEnvelope (const double* logMag, double* envLog, int order, int maxIterations = 6, double tolDb = 1.0) noexcept
    {
        const double tol = tolDb / 8.685889638065035;      // dB -> népers
        for (int k = 0; k < bins; ++k)
            work[(size_t) k] = std::max (logMag[k], kLogFloor);

        cepstralSmooth (work.data(), envLog, order);
        int it = 1;
        for (; it < maxIterations; ++it)
        {
            double worst = 0.0;
            for (int k = 0; k < bins; ++k)
            {
                const double a = std::max (logMag[k], kLogFloor);
                worst = std::max (worst, a - envLog[k]);
                work[(size_t) k] = std::max (work[(size_t) k], envLog[k]);
            }
            if (worst < tol)
                break;
            cepstralSmooth (work.data(), envLog, order);
        }
        return it;
    }

    /** out[k] = env(k / γ), interpolation linéaire, bornes prolongées. */
    static void warp (const double* envLog, double* out, int numBins, double gamma) noexcept
    {
        const double inv = 1.0 / std::max (1.0e-6, gamma);
        for (int k = 0; k < numBins; ++k)
        {
            const double src = (double) k * inv;
            if (src >= (double) (numBins - 1)) { out[k] = envLog[numBins - 1]; continue; }
            const int    i = (int) src;
            const double f = src - (double) i;
            out[k] = envLog[i] + f * (envLog[i + 1] - envLog[i]);
        }
    }

private:
    RealFFT fft;
    int n = 0, bins = 0;
    std::vector<double> cep, re, im, work;
};

} // namespace aura::pitch

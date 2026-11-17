// AuraKey — NextDSP · AuraSuite
// Decimator.h : ramène le flux (44,1 kHz … DXD 384 kHz) vers la fréquence d'analyse (≈ 11 kHz).
//
//   * Facteur entier D = round(fs / 11025) : 44,1 k -> 4 (11 025 Hz), 48 k -> 4 (12 000), 96 k -> 9 (10 667),
//     352,8 k -> 32 (11 025), 384 k -> 35 (10 971). L'analyse (chroma, tempo) ne dépend que de la fréquence obtenue.
//   * Filtre RIF à phase linéaire, sinus cardinal fenêtré (Blackman), coupure 0,42 × fa, 16·D + 1 coefficients :
//     au moins 70 dB d'atténuation au-delà de la nouvelle fréquence de Nyquist ; seul un échantillon sur D est calculé.
//   * Historique en double longueur (chaque échantillon écrit deux fois) : le produit scalaire lit une zone contiguë.
//
// Temps réel : prepare() alloue ; push() n'alloue jamais.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace aura::key
{

class Decimator
{
public:
    static constexpr double kTargetRate = 11025.0;

    void prepare (double inputRate)
    {
        fsIn = inputRate;
        factor = std::max (1, (int) std::lround (inputRate / kTargetRate));
        fsOut = inputRate / factor;
        const int taps = 16 * factor + 1;
        coeffs.assign ((size_t) taps, 0.0);
        const double fc = 0.42 / factor;                       // coupure relative à fsIn
        constexpr double pi = 3.14159265358979323846;
        const int mid = taps / 2;
        double sum = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            const double x = i - mid;
            const double sinc = i == mid ? 2.0 * fc : std::sin (2.0 * pi * fc * x) / (pi * x);
            const double w = 0.42 - 0.5 * std::cos (2.0 * pi * i / (taps - 1)) + 0.08 * std::cos (4.0 * pi * i / (taps - 1));
            coeffs[(size_t) i] = sinc * w;
            sum += coeffs[(size_t) i];
        }
        for (auto& c : coeffs) c /= sum;                       // gain unité au continu
        size = taps;
        history.assign ((size_t) (2 * size), 0.0);
        reset();
    }

    void reset() noexcept
    {
        std::fill (history.begin(), history.end(), 0.0);
        pos = 0;
        phase = 0;
    }

    int getFactor() const noexcept       { return factor; }
    double getOutputRate() const noexcept { return fsOut; }
    double getInputRate() const noexcept  { return fsIn; }
    /** Retard de groupe du filtre, en échantillons d'entrée. */
    int getDelay() const noexcept         { return size / 2; }

    /** `onOutput (double)` est appelé pour chaque échantillon décimé. */
    template <typename Fn>
    void push (const double* x, int n, Fn&& onOutput) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            history[(size_t) pos] = x[i];
            history[(size_t) (pos + size)] = x[i];
            if (++pos == size) pos = 0;
            if (++phase == factor)
            {
                phase = 0;
                // les `size` derniers échantillons, du plus ancien au plus récent : history[pos … pos + size − 1]
                const double* h = history.data() + pos;
                double acc = 0.0;
                for (int k = 0; k < size; ++k)
                    acc += coeffs[(size_t) k] * h[k];
                onOutput (acc);
            }
        }
    }

private:
    std::vector<double> coeffs, history;
    double fsIn = 48000.0, fsOut = 12000.0;
    int factor = 4, size = 1, pos = 0, phase = 0;
};

} // namespace aura::key

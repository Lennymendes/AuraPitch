// AuraPitch — NextDSP · AuraSuite
// SpliceShifter.h : correction de hauteur à tête de lecture unique (rééchantillonnage + raccords synchrones de la
// période), 1 ou 2 canaux, sans JUCE. Cœur de la correction en Live (2 ms) et en Studio (à 44,1 / 48 kHz).
//
// Pourquoi pas PSOLA pour la correction
//   En PSOLA, deux grains se recouvrent en permanence et lisent la voix avec des retards différents : les composantes
//   périodiques s'additionnent en phase, mais le souffle, l'air et le grain du micro (apériodiques) s'additionnent
//   hors phase. C'est un filtre en peigne qui varie à la cadence de la période : timbre métallique, voile au-dessus de
//   2 kHz, et impression de deux voix dès que les marques glissent par rapport aux impulsions glottiques.
//
// Principe
//   * Une seule tête lit l'anneau d'entrée à la vitesse r (rapport de hauteur), par sinc 48 points (Interpolator.h) :
//     sortie = x(n − o(n)), o(n + 1) = o(n) + (1 − r). Le spectre entier glisse d'un bloc, sans peigne.
//   * Quand le retard sort de sa plage [oMin, oMin + 1,25·T], la tête saute d'une période T* vers l'arrière ou vers
//     l'avant. T* est cherchée dans ±10 % de la période du détecteur en maximisant la corrélation normalisée entre
//     la période qui se termine sous la tête et celle sous la nouvelle position : les deux têtes sont en phase.
//   * Raccord : fondu en cosinus surélevé d'au plus une période, normalisé par la corrélation mesurée (gain constant
//     que les deux lectures soient identiques, ρ = 1, ou indépendantes, ρ = 0). À 40 cents de correction, un raccord
//     survient toutes les ~0,25 s ; entre deux raccords la voix n'est que rééchantillonnée.
//   * Non voisé (après le maintien du moteur) : la tête revient sur o = D (fondu de 10 ms) et s'y verrouille, vitesse 1 :
//     sortie = entrée retardée de D, au sample près (alignement Dry / Wet).
//   * Formants : un rééchantillonnage les déplace du rapport r (2,3 % à 40 cents, inaudible pour une correction).
//     En Studio, le filtre différentiel de StudioFormantFilter.h les remet exactement en place.
// Aucune allocation après prepare(), aucun verrou.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Interpolator.h"

namespace aura::pitch
{

class SpliceShifter
{
public:
    static constexpr int kMaxChannels = 2;

    /** latencySeconds : retard nominal D (verrouillage au rapport 1). */
    void prepare (double fs, int numChannels, double latencySeconds = 0.002, double maxPeriodSeconds = 1.0 / 45.0)
    {
        sampleRate = fs;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        nominal = std::max ((double) SincInterpolator::kLookahead + 3.0, std::round (latencySeconds * fs));
        maxPeriod = maxPeriodSeconds * fs;
        minPeriod = fs / 1400.0;
        int size = 1;
        while (size < (int) (nominal + 6.0 * maxPeriod + 4096)) size <<= 1;
        mask = size - 1;
        for (auto& r : rings) r.assign ((size_t) size, 0.0);
        mono.assign ((size_t) size, 0.0);
        interp.prepare();
        ratioCoeff = 1.0 - std::exp (-1.0 / (0.001 * fs));
        bypassFade = std::max (16.0, 0.010 * fs);
        reset();
    }

    void reset() noexcept
    {
        for (auto& r : rings) std::fill (r.begin(), r.end(), 0.0);
        std::fill (mono.begin(), mono.end(), 0.0);
        counter = 0;
        period = sampleRate * 0.005;
        ratioTarget = ratio = 1.0;
        voiced = false;
        head = { nominal, 1.0 };
        fading = false;
        locked = true;
        lastCorrelation = 0.0;
        splices = 0;
    }

    int getLatencySamples() const noexcept { return (int) nominal; }
    std::int64_t getSpliceCount() const noexcept { return splices; }
    double getCurrentOffset() const noexcept { return head.offset; }

    /** Commande (à chaque estimation, voisement déjà maintenu par le moteur). */
    void setPitch (double periodSamples, bool isVoiced) noexcept
    {
        if (isVoiced && periodSamples >= minPeriod && periodSamples <= maxPeriod)
            period = periodSamples;
        voiced = isVoiced;
    }
    void setRatio (double r) noexcept { ratioTarget = std::clamp (r, 0.2, 5.0); }

    template <typename SampleType>
    void process (const SampleType* const* in, SampleType* const* out, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const std::int64_t n = counter;
            const size_t slot = (size_t) (n & mask);
            double m = 0.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                double v = (double) in[ch][i];
                if (! std::isfinite (v)) v = 0.0;
                rings[(size_t) ch][slot] = v;
                m += v;
            }
            mono[slot] = m / channels;

            step (n);

            for (int ch = 0; ch < channels; ++ch)
                out[ch][i] = (SampleType) render ((size_t) ch, n);
            ++counter;
        }
    }

private:
    struct Head { double offset = 0.0, gain = 1.0; };

    double oMin() const noexcept
    {
        // Pendant un fondu de longueur C, la tête continue de se rapprocher de l'écriture de (r − 1)·C.
        return std::max (nominal, (double) SincInterpolator::kLookahead + 3.0 + std::max (0.0, ratio - 1.0) * period);
    }

    void step (std::int64_t n) noexcept
    {
        const bool bypass = ! voiced && std::abs (ratioTarget - 1.0) < 1.0e-12;
        ratio += ratioCoeff * (ratioTarget - ratio);
        if (std::abs (ratio - ratioTarget) < 1.0e-12) ratio = ratioTarget;

        if (locked)
        {
            if (! bypass) locked = false;
            else { head.offset = nominal; ratio = 1.0; return; }
        }

        // Avance des têtes : vitesse r
        const double drift = 1.0 - ratio;
        head.offset += drift;
        if (fading)
        {
            other.offset += drift;
            fadePos += 1.0;
            if (fadePos >= fadeLen) { head = other; head.gain = 1.0; fading = false; }
        }

        if (fading) return;

        if (bypass)
        {
            if (std::abs (head.offset - nominal) < 1.0e-9) { head.offset = nominal; locked = true; return; }
            startFade (nominal, bypassFade, 0.0);                       // retour sur D (consonne : ρ ≈ 0)
            return;
        }

        const double lo = oMin(), hi = lo + 1.25 * period;
        if (head.offset < lo)       spliceBy (+1.0, n);              // tête trop près de l'écriture : une période en arrière
        else if (head.offset > hi)  spliceBy (-1.0, n);              // tête trop en retard : une période en avant
    }

    /** Saut d'une période (direction +1 : plus ancien, −1 : plus récent), T* calée par corrélation. */
    void spliceBy (double direction, std::int64_t n) noexcept
    {
        const double readPos = (double) n - head.offset;
        const int    T  = std::max (8, (int) std::lround (period));
        const int    W  = T;                                          // une période sous la tête
        const int    stride = std::max (1, T / 96);
        const int    lo = (int) (0.9 * T), hi = (int) std::ceil (1.1 * T);
        const std::int64_t a = (std::int64_t) std::floor (readPos) - W;  // [a, a + W) : période qui finit sous la tête

        auto ncc = [&] (int lag, int s) noexcept
        {
            const std::int64_t b = a + (std::int64_t) (direction > 0 ? -lag : lag);
            double xy = 0, xx = 0, yy = 0;
            for (int k = 0; k < W; k += s)
            {
                const double x = mono[(size_t) ((a + k) & mask)];
                const double y = mono[(size_t) ((b + k) & mask)];
                xy += x * y; xx += x * x; yy += y * y;
            }
            return xy / std::sqrt (xx * yy + 1.0e-30);
        };

        // Recherche grossière (pas « stride »), puis fine (pas 1) autour du meilleur
        int best = T; double bestC = -2.0;
        for (int lag = lo; lag <= hi; lag += stride)
        {
            // causalité vers l'avant : la nouvelle tête doit rester ≥ oMin
            if (direction < 0 && head.offset - lag < oMin()) break;
            const double c = ncc (lag, stride);
            if (c > bestC) { bestC = c; best = lag; }
        }
        for (int lag = best - stride + 1; lag < best + stride; ++lag)
        {
            if (lag < lo || lag > hi || lag == best) continue;
            if (direction < 0 && head.offset - lag < oMin()) continue;
            const double c = ncc (lag, 1);
            if (c > bestC) { bestC = c; best = lag; }
        }

        // Sous-échantillon : sommet parabolique de la corrélation (à 48 kHz, une période de 226 Hz vaut 212,4
        // échantillons ; un saut arrondi à 212 laisse un défaut de phase qui, répété à la cadence des raccords
        // (= F_in − F_out), recrée une raie à la fréquence chantée).
        double frac = 0.0;
        if (best > lo && best < hi)
        {
            const double cm = ncc (best - 1, 1), c0 = ncc (best, 1), cp = ncc (best + 1, 1);
            const double den = cm - 2.0 * c0 + cp;
            if (den < 0.0) frac = std::clamp (0.5 * (cm - cp) / den, -0.5, 0.5);
        }
        if (direction < 0 && head.offset - ((double) best + frac) < oMin()) frac = std::min (frac, 0.0);
        const double target = head.offset + direction * ((double) best + frac);
        const double rate = std::abs (1.0 - ratio);
        const double len = std::max (8.0, std::min ((double) T, rate > 1.0e-9 ? 0.8 * (double) T / rate : (double) T));
        startFade (target, len, std::clamp (bestC, 0.0, 1.0));
        ++splices;
    }

    void startFade (double targetOffset, double length, double correlation) noexcept
    {
        other = { targetOffset, 0.0 };
        fadeLen = length;
        fadePos = 0.0;
        lastCorrelation = correlation;
        fading = true;
    }

    double render (size_t ch, std::int64_t n) const noexcept
    {
        const double* ring = rings[ch].data();
        if (locked)
            return ring[(size_t) ((n - (std::int64_t) nominal) & mask)];

        const int bank = SincInterpolator::bankFor (ratio);
        const double a = interp.read (ring, mask, (double) n - head.offset, bank);
        if (! fading)
            return a;
        const double b = interp.read (ring, mask, (double) n - other.offset, bank);
        const double t = fadePos / fadeLen;
        const double w = 0.5 - 0.5 * std::cos (3.14159265358979323846 * t);      // 0 -> 1
        const double g0 = 1.0 - w, g1 = w;
        const double norm = 1.0 / std::sqrt (std::max (1.0e-6, g0 * g0 + g1 * g1 + 2.0 * lastCorrelation * g0 * g1));
        return (g0 * a + g1 * b) * norm;
    }

    double sampleRate = 48000.0, nominal = 96.0, maxPeriod = 1000.0, minPeriod = 30.0;
    int channels = 1;
    std::int64_t mask = 0, counter = 0, splices = 0;
    std::array<std::vector<double>, kMaxChannels> rings;
    std::vector<double> mono;
    SincInterpolator interp;

    double period = 240.0, ratio = 1.0, ratioTarget = 1.0, ratioCoeff = 0.02, bypassFade = 240.0;
    bool   voiced = false, fading = false, locked = true;
    Head   head, other;
    double fadeLen = 1.0, fadePos = 0.0, lastCorrelation = 0.0;
};

} // namespace aura::pitch

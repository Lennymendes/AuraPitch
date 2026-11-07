// AuraPitch — NextDSP · AuraSuite
// HalfBand.h : briques multicadence partagées (sans JUCE).
//   * MirrorRing                 : tampon circulaire dont les n derniers échantillons sont toujours contigus ;
//   * HalfBandDecimatorT<M>      : FIR demi-bande ÷2, 4M − 1 coefficients (M distincts), phase linéaire ;
//   * HalfBandInterpolatorT<M>   : FIR demi-bande ×2, mêmes coefficients, deux sorties par entrée.
// Coefficients : sinc fenêtré Kaiser, coefficients pairs exactement nuls, centre exactement 1/2, gain continu
// exactement 1. Retard de groupe : 2M − 1 échantillons à la cadence haute.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace aura::pitch::detail
{

class MirrorRing
{
public:
    void allocate (int capacity)
    {
        cap = std::max (1, capacity);
        data.assign ((size_t) (2 * cap), 0.0);
        reset();
    }
    void reset() noexcept
    {
        std::fill (data.begin(), data.end(), 0.0);
        head = cap - 1;
        count = 0;
    }
    void push (double v) noexcept
    {
        head = (head + 1 == cap) ? 0 : head + 1;
        data[(size_t) head] = v;
        data[(size_t) (head + cap)] = v;
        if (count < cap) ++count;
    }
    /** Les n derniers échantillons (n ≤ capacité), du plus ancien [0] au plus récent [n − 1]. */
    const double* last (int n) const noexcept { return data.data() + head + 1 + cap - n; }
    int size() const noexcept     { return count; }
    int capacity() const noexcept { return cap; }

private:
    std::vector<double> data;
    int cap = 1, head = 0, count = 0;
};

inline double besselI0 (double x) noexcept
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

/** Coefficients des décalages impairs ±1, ±3, … ±(2M − 1) autour du centre (un côté). */
template <int M>
struct HalfBandCoefficients
{
    static constexpr int kTaps   = 4 * M - 1;
    static constexpr int kCentre = 2 * M - 1;
    std::array<double, (size_t) M> c {};

    void design (double beta) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const double i0b = besselI0 (beta);
        double sum = 0.0;
        for (int i = 0; i < M; ++i)
        {
            const int    o   = 2 * i + 1;
            const double t   = (double) o / (double) kCentre;
            const double win = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - t * t))) / i0b;
            const double x   = 0.5 * (double) o;
            c[(size_t) i] = 0.5 * std::sin (pi * x) / (pi * x) * win;
            sum += 2.0 * c[(size_t) i];
        }
        for (auto& v : c)
            v *= 0.5 / sum;
    }
};

template <int M>
class HalfBandDecimatorT
{
public:
    static constexpr int    kTaps       = HalfBandCoefficients<M>::kTaps;
    static constexpr int    kCentre     = HalfBandCoefficients<M>::kCentre;
    static constexpr double kGroupDelay = kCentre;

    void prepare (double beta = 9.0)
    {
        ring.allocate (kTaps);
        coeffs.design (beta);
        reset();
    }
    void reset() noexcept { ring.reset(); phase = 0; }

    /** true quand un échantillon de sortie est produit (un appel sur deux). */
    bool push (double x, double& y) noexcept
    {
        ring.push (x);
        phase ^= 1;
        if (phase != 0)
            return false;

        const double* p = ring.last (kTaps);
        double acc = 0.5 * p[kCentre];
        for (int i = 0; i < M; ++i)
        {
            const int o = 2 * i + 1;
            acc += coeffs.c[(size_t) i] * (p[kCentre - o] + p[kCentre + o]);
        }
        y = acc;
        return true;
    }

private:
    MirrorRing ring;
    HalfBandCoefficients<M> coeffs;
    int phase = 0;
};

template <int M>
class HalfBandInterpolatorT
{
public:
    void prepare (double beta = 9.0)
    {
        ring.allocate (2 * M);
        coeffs.design (beta);
        reset();
    }
    void reset() noexcept { ring.reset(); }

    /** Une entrée à la cadence basse -> deux sorties consécutives à la cadence haute (y0 puis y1). */
    void push (double u, double& y0, double& y1) noexcept
    {
        ring.push (u);
        const double* p = ring.last (2 * M);           // p[2M − 1] = u
        double acc = 0.0;
        for (int i = 0; i < M; ++i)
            acc += coeffs.c[(size_t) i] * (p[M - 1 - i] + p[M + i]);
        y0 = 2.0 * acc;
        y1 = p[M];                                       // phase du centre : 2 × ½ = recopie exacte
    }

private:
    MirrorRing ring;
    HalfBandCoefficients<M> coeffs;
};

/** Décimateur du détecteur de pitch (étape 1) : 47 coefficients, Kaiser β 9 (≈ 90 dB). */
class HalfBandDecimator : public HalfBandDecimatorT<12>
{
public:
    void prepare() { HalfBandDecimatorT<12>::prepare (9.0); }
};

} // namespace aura::pitch::detail

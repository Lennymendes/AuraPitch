// AuraPitch — NextDSP · AuraSuite
// FFT.h : FFT réelle en double (sans JUCE), taille 2^k, aucune allocation après prepare().
//
// La FFT réelle de N points passe par une FFT complexe de N/2 points (échantillons pairs en partie réelle, impairs
// en partie imaginaire) puis une passe de séparation : deux fois moins de calcul qu'une FFT complexe de N points.
// Convention : X[k] = Σ x[n] e^{−j2πkn/N}, k = 0 … N/2 ; inverse(X) = x (facteur 1/N inclus).
#pragma once

#include <cmath>
#include <vector>

namespace aura::pitch
{

class RealFFT
{
public:
    void prepare (int order)
    {
        n    = 1 << order;
        half = n / 2;

        // Bit-reverse et facteurs de rotation de la FFT complexe de taille half.
        bitrev.assign ((size_t) half, 0);
        int bits = 0;
        while ((1 << bits) < half) ++bits;
        for (int i = 0; i < half; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            bitrev[(size_t) i] = r;
        }
        constexpr double twoPi = 6.28318530717958647692;
        cosT.resize ((size_t) std::max (1, half / 2));
        sinT.resize ((size_t) std::max (1, half / 2));
        for (int k = 0; k < half / 2; ++k)
        {
            cosT[(size_t) k] = std::cos (twoPi * k / half);
            sinT[(size_t) k] = std::sin (twoPi * k / half);
        }
        // Facteurs de séparation e^{−j2πk/N}, k = 0 … N/2.
        splitCos.resize ((size_t) half + 1);
        splitSin.resize ((size_t) half + 1);
        for (int k = 0; k <= half; ++k)
        {
            splitCos[(size_t) k] = std::cos (twoPi * k / n);
            splitSin[(size_t) k] = std::sin (twoPi * k / n);
        }
        zr.assign ((size_t) half, 0.0);
        zi.assign ((size_t) half, 0.0);
    }

    int getSize() const noexcept { return n; }
    int getNumBins() const noexcept { return half + 1; }

    /** x[0 … N−1] -> re/im[0 … N/2]. */
    void forward (const double* x, double* re, double* im) noexcept
    {
        for (int i = 0; i < half; ++i)
        {
            zr[(size_t) i] = x[2 * i];
            zi[(size_t) i] = x[2 * i + 1];
        }
        complexFFT (zr.data(), zi.data(), false);

        for (int k = 0; k <= half; ++k)
        {
            const int a = k % half, b = (half - k) % half;
            const double zkR = zr[(size_t) a], zkI = zi[(size_t) a];
            const double zmR = zr[(size_t) b], zmI = -zi[(size_t) b];           // conj Z[M − k]
            const double eR = 0.5 * (zkR + zmR), eI = 0.5 * (zkI + zmI);        // E[k]
            const double dR = 0.5 * (zkR - zmR), dI = 0.5 * (zkI - zmI);        // (Z − conj Z)/2 = j·O
            // O = d / j = (dI, −dR) ; X = E + W^k O, W^k = (c, −s)
            const double oR = dI, oI = -dR;
            const double c = splitCos[(size_t) k], s = -splitSin[(size_t) k];
            re[k] = eR + c * oR - s * oI;
            im[k] = eI + c * oI + s * oR;
        }
    }

    /** re/im[0 … N/2] (spectre d'un signal réel) -> x[0 … N−1]. */
    void inverse (const double* re, const double* im, double* x) noexcept
    {
        for (int k = 0; k < half; ++k)
        {
            const int b = half - k;
            const double xkR = re[k], xkI = im[k];
            const double xmR = re[b], xmI = -im[b];                             // conj X[M − k]
            const double eR = 0.5 * (xkR + xmR), eI = 0.5 * (xkI + xmI);
            const double dR = 0.5 * (xkR - xmR), dI = 0.5 * (xkI - xmI);
            // O = d · W^{−k},  W^{−k} = (c, s)
            const double c = splitCos[(size_t) k], s = splitSin[(size_t) k];
            const double oR = dR * c - dI * s, oI = dR * s + dI * c;
            // Z = E + j O
            zr[(size_t) k] = eR - oI;
            zi[(size_t) k] = eI + oR;
        }
        complexFFT (zr.data(), zi.data(), true);
        const double scale = 1.0 / (double) half;
        for (int i = 0; i < half; ++i)
        {
            x[2 * i]     = zr[(size_t) i] * scale;
            x[2 * i + 1] = zi[(size_t) i] * scale;
        }
    }

private:
    void complexFFT (double* re, double* im, bool inverseDirection) noexcept
    {
        for (int i = 0; i < half; ++i)
        {
            const int j = bitrev[(size_t) i];
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        const double sign = inverseDirection ? 1.0 : -1.0;
        for (int len = 2; len <= half; len <<= 1)
        {
            const int h = len / 2, step = half / len;
            for (int i = 0; i < half; i += len)
            {
                for (int k = 0; k < h; ++k)
                {
                    const double wr = cosT[(size_t) (k * step)], wi = sign * sinT[(size_t) (k * step)];
                    const int a = i + k, b = a + h;
                    const double vr = re[b] * wr - im[b] * wi;
                    const double vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr; im[b] = im[a] - vi;
                    re[a] += vr;        im[a] += vi;
                }
            }
        }
    }

    int n = 0, half = 0;
    std::vector<int>    bitrev;
    std::vector<double> cosT, sinT, splitCos, splitSin, zr, zi;
};

} // namespace aura::pitch

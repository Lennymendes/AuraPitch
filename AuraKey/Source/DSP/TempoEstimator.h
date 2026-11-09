// AuraKey — NextDSP · AuraSuite
// TempoEstimator.h : tempo global d'un fichier (analyse hors ligne, fil de travail).
//
//   * Enveloppe d'attaques : flux spectral positif du log-module (FFT 512 points, pas de 64 échantillons décimés,
//     ≈ 172 trames/s), moyenne locale (±0,3 s) retirée, partie positive, lissage gaussien (σ = 1 trame).
//   * Autocorrélation de l'enveloppe jusqu'à 4 s.
//   * Peigne : pour chaque tempo candidat (60 … 200 BPM, pas de 0,05), somme de l'autocorrélation aux 4 premiers
//     multiples de la période (interpolée), pondérée par un a priori log-normal centré sur 120 BPM (σ = 1 octave), qui
//     départage les erreurs d'octave (64 / 128 / 256) ; affinage au millième autour du meilleur.
//   * Confiance : contraste du meilleur score avec le score médian (0 … 1).
//
// Hors ligne : push() fait grandir l'enveloppe (≈ 700 Ko pour 10 minutes) ; à ne pas utiliser sur le thread audio.
#pragma once

#include "FFT.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aura::key
{

class TempoEstimator
{
public:
    struct Result
    {
        double bpm = 0.0;          // 0 : pas assez de signal
        double confidence = 0.0;
    };

    static constexpr double kMinBpm = 60.0, kMaxBpm = 200.0;

    void prepare (double analysisRate)
    {
        fa = analysisRate;
        fft.prepare (kOrder);
        window.resize (kSize);
        for (int i = 0; i < kSize; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * (i + 0.5) / kSize);
        ring.assign (2 * kSize, 0.0);
        frame.assign (kSize, 0.0);
        re.assign (kSize / 2 + 1, 0.0);
        im.assign (kSize / 2 + 1, 0.0);
        prevLog.assign (kSize / 2 + 1, 0.0);
        flux.clear();
        pos = 0; sinceHop = 0; havePrev = false;
    }

    double getFrameRate() const noexcept { return fa / kHop; }
    const std::vector<float>& getEnvelope() const noexcept { return flux; }

    void push (const double* x, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            ring[(size_t) pos] = x[i];
            ring[(size_t) (pos + kSize)] = x[i];
            if (++pos == kSize) pos = 0;
            if (++sinceHop == kHop)
            {
                sinceHop = 0;
                analyse();
            }
        }
    }

    Result estimate() const
    {
        Result r;
        const double fr = getFrameRate();
        const int count = (int) flux.size();
        if (count < (int) (8.0 * fr))
            return r;

        // retrait de la moyenne locale, partie positive
        std::vector<double> prefix ((size_t) count + 1, 0.0);
        for (int i = 0; i < count; ++i) prefix[(size_t) i + 1] = prefix[(size_t) i] + flux[(size_t) i];
        const int half = std::max (1, (int) std::lround (0.3 * fr));
        std::vector<double> e ((size_t) count, 0.0);
        for (int i = 0; i < count; ++i)
        {
            const int a = std::max (0, i - half), b = std::min (count - 1, i + half);
            const double mean = (prefix[(size_t) b + 1] - prefix[(size_t) a]) / (b - a + 1);
            e[(size_t) i] = std::max (0.0, flux[(size_t) i] - mean);
        }
        // lissage gaussien σ = 1 trame
        std::vector<double> s ((size_t) count, 0.0);
        constexpr double g[] = { 0.054, 0.242, 0.399, 0.242, 0.054 };
        for (int i = 0; i < count; ++i)
        {
            double acc = 0.0;
            for (int k = -2; k <= 2; ++k)
                acc += g[k + 2] * e[(size_t) std::clamp (i + k, 0, count - 1)];
            s[(size_t) i] = acc;
        }
        double mean = 0.0;
        for (double v : s) mean += v;
        mean /= count;
        for (auto& v : s) v -= mean;

        const int maxLag = std::min (count - 1, (int) std::ceil (4.0 * 60.0 * fr / kMinBpm) + 2);
        std::vector<double> acf ((size_t) maxLag + 1, 0.0);
        for (int lag = 0; lag <= maxLag; ++lag)
        {
            double acc = 0.0;
            for (int i = lag; i < count; ++i)
                acc += s[(size_t) i] * s[(size_t) (i - lag)];
            acf[(size_t) lag] = acc / (count - lag);
        }
        if (! (acf[0] > 0.0))
            return r;
        auto at = [&] (double lag)
        {
            const int i = (int) lag;
            if (i < 0 || i + 1 > maxLag) return 0.0;
            const double f = lag - i;
            return (acf[(size_t) i] * (1.0 - f) + acf[(size_t) i + 1] * f) / acf[0];
        };
        auto score = [&] (double bpm)
        {
            const double period = 60.0 * fr / bpm;
            double sum = 0.0;
            for (int k = 1; k <= 4; ++k) sum += at (k * period);
            const double o = std::log2 (bpm / 120.0);
            return sum * std::exp (-0.5 * o * o);
        };

        std::vector<double> scores;
        double best = -1.0e300, bestBpm = 120.0;
        for (double bpm = kMinBpm; bpm <= kMaxBpm + 1e-9; bpm += 0.05)
        {
            const double v = score (bpm);
            scores.push_back (v);
            if (v > best) { best = v; bestBpm = bpm; }
        }
        for (double bpm = bestBpm - 0.06; bpm <= bestBpm + 0.06; bpm += 0.001)
        {
            const double v = score (bpm);
            if (v > best) { best = v; r.bpm = bpm; }
        }
        if (r.bpm <= 0.0) r.bpm = bestBpm;
        std::nth_element (scores.begin(), scores.begin() + (long) scores.size() / 2, scores.end());
        const double median = scores[scores.size() / 2];
        r.confidence = best > 0.0 ? std::clamp ((best - std::max (0.0, median)) / best, 0.0, 1.0) : 0.0;
        if (! (best > 0.0)) r.bpm = 0.0;
        return r;
    }

private:
    static constexpr int kOrder = 9, kSize = 1 << kOrder, kHop = 64;

    void analyse()
    {
        const double* h = ring.data() + pos;
        for (int i = 0; i < kSize; ++i)
            frame[(size_t) i] = h[i] * window[(size_t) i];
        fft.forward (frame.data(), re.data(), im.data());
        double sum = 0.0;
        for (int k = 1; k <= kSize / 2; ++k)
        {
            const double m = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
            const double l = std::log1p (100.0 * m);
            if (havePrev) sum += std::max (0.0, l - prevLog[(size_t) k]);
            prevLog[(size_t) k] = l;
        }
        havePrev = true;
        flux.push_back ((float) sum);
    }

    aura::pitch::RealFFT fft;
    std::vector<double> window, ring, frame, re, im, prevLog;
    std::vector<float> flux;
    double fa = 11025.0;
    int pos = 0, sinceHop = 0;
    bool havePrev = false;
};

} // namespace aura::key

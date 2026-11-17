// AuraPitch — NextDSP · AuraSuite
// FastPitchTrack.h : hauteur chantée à un instant donné, d'après le suivi rapide du PitchDetector (lot 1).
//
// Le détecteur date chaque estimation rapide au centre de sa fenêtre (1,25 période avant l'échantillon courant). Le
// moteur, lui, a besoin de la hauteur à l'instant du signal qu'il lit vraiment :
//   * Studio : un instant passé (l'audio est retardé) -> interpolation linéaire entre deux estimations ;
//   * Live   : la tête lit entre 2 ms et 2 ms + 1,25 période derrière l'entrée -> interpolation quand l'instant est
//              déjà couvert, sinon extrapolation de quelques ms.
// Extrapolation : parabole des moindres carrés sur les 24 dernières ms (un vibrato est localement une parabole, pas une
// droite), prolongée d'au plus 12 ms ; avec moins de 5 points ou 10 ms d'historique, droite puis valeur tenue.
// Historique de 96 estimations (144 ms au pas de 1,5 ms). Aucune allocation, thread audio seulement.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace aura::pitch
{

class FastPitchTrack
{
public:
    void prepare (double fs) noexcept
    {
        sampleRate   = fs;
        fitSamples   = 0.024 * fs;
        maxHorizon   = 0.012 * fs;
        staleSamples = (std::int64_t) (0.030 * fs);
        clear();
    }
    void clear() noexcept { count = 0; head = 0; }
    bool empty() const noexcept { return count == 0; }

    void push (std::int64_t time, double midi) noexcept
    {
        if (count > 0 && time <= entry (0).time)
            return;                                              // horloge non croissante : ignoré
        if (count > 0 && time - entry (0).time > staleSamples)
            clear();                                             // trou : l'historique ne décrit plus la même note
        head = (head + 1) % kSize;
        points[(size_t) head] = { time, midi };
        count = std::min (count + 1, kSize);
    }

    /** Hauteur (demi-tons MIDI) à l'instant t. false : pas d'historique utilisable. */
    bool at (std::int64_t t, double& midi) const noexcept
    {
        if (count == 0)
            return false;
        const auto& newest = entry (0);
        if (t <= newest.time)
        {
            for (int j = 1; j < count; ++j)
            {
                const auto& older = entry (j);
                if (older.time <= t)
                {
                    const auto& newer = entry (j - 1);
                    const double w = (double) (t - older.time) / (double) (newer.time - older.time);
                    midi = older.midi + w * (newer.midi - older.midi);
                    return true;
                }
            }
            midi = entry (count - 1).midi;                       // plus ancien que l'historique : la première valeur
            return t >= entry (count - 1).time - staleSamples;
        }
        if (t - newest.time > staleSamples)
            return false;

        // Moindres carrés sur les points récents ; abscisse u = (temps − dernier) / durée d'ajustement, −1 … 0.
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, y0 = 0, y1 = 0, y2 = 0;
        int n = 0;
        double span = 0.0;
        for (int j = 0; j < count; ++j)
        {
            const auto& p = entry (j);
            const double age = (double) (newest.time - p.time);
            if (age > fitSamples) break;
            const double u = -age / fitSamples, u2 = u * u;
            s0 += 1.0; s1 += u; s2 += u2; s3 += u2 * u; s4 += u2 * u2;
            y0 += p.midi; y1 += p.midi * u; y2 += p.midi * u2;
            span = age / sampleRate;
            ++n;
        }
        const double h = std::min ((double) (t - newest.time), maxHorizon) / fitSamples;
        const double T = fitSamples / sampleRate;
        if (n < 3 || span < 0.004)
        {
            midi = y0 / s0;
            return true;
        }
        if (n >= 5 && span >= 0.010)
        {
            // Système normal 3 × 3 (a + b u + c u²)
            const double det = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s3 * s2) + s2 * (s1 * s3 - s2 * s2);
            if (std::abs (det) > 1.0e-30)
            {
                const double a = (y0 * (s2 * s4 - s3 * s3) - s1 * (y1 * s4 - s3 * y2) + s2 * (y1 * s3 - s2 * y2)) / det;
                const double b = (s0 * (y1 * s4 - s3 * y2) - y0 * (s1 * s4 - s3 * s2) + s2 * (s1 * y2 - y1 * s2)) / det;
                const double c = (s0 * (s2 * y2 - y1 * s3) - s1 * (s1 * y2 - y1 * s2) + y0 * (s1 * s3 - s2 * s2)) / det;
                // Garde-fous : pente ≤ 60 dt/s, courbure ≤ 2000 dt/s² (vibrato de ±1,5 dt à 6 Hz)
                const double bc = std::clamp (b, -60.0 * T, 60.0 * T), cc = std::clamp (c, -2000.0 * T * T, 2000.0 * T * T);
                midi = a + bc * h + cc * h * h;
                return true;
            }
        }
        const double den = s0 * s2 - s1 * s1;
        if (std::abs (den) < 1.0e-30) { midi = y0 / s0; return true; }
        const double b = std::clamp ((s0 * y1 - s1 * y0) / den, -60.0 * T, 60.0 * T);
        const double a = (y0 - b * s1) / s0;
        midi = a + b * h;
        return true;
    }

private:
    struct Point { std::int64_t time = 0; double midi = 0.0; };
    static constexpr int kSize = 96;
    const Point& entry (int back) const noexcept { return points[(size_t) ((head - back + kSize) % kSize)]; }

    std::array<Point, kSize> points {};
    int head = 0, count = 0;
    double sampleRate = 48000.0, fitSamples = 1152.0, maxHorizon = 576.0;
    std::int64_t staleSamples = 1440;
};

} // namespace aura::pitch

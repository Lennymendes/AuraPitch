// AuraKey — NextDSP · AuraSuite
// SyntheticMusic.h : morceaux de synthèse pour les bancs d'essai (tonalité, tempo et sections connus).
//
// Une mesure = un accord (4 temps). Nappe : les 3 notes de l'accord (octave 4, 6 harmoniques en 1/h, enveloppe
// lente) ; basse : fondamentale de l'accord à chaque temps (octave 2, 4 harmoniques, décroissance) ; mélodie : notes de
// l'accord et de la gamme sur les croches (octave 5) ; batterie : grosse caisse sur les temps, caisse claire sur 2 et 4,
// charleston sur les croches. Désaccord global optionnel (en cents). Graine fixe : rendu identique d'un essai à l'autre.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aurakey_test
{

struct Chord { int root; bool minor; };           // root : classe de hauteur 0 … 11

struct Part
{
    int tonic = 0;
    bool minor = false;
    std::vector<Chord> progression;               // une mesure par accord, en boucle
    double seconds = 30.0;
};

struct Song
{
    double bpm = 120.0;
    double detuneCents = 0.0;
    bool drums = true;
    std::vector<Part> parts;
};

/** Progressions nettes : I–IV–V–I en majeur, i–iv–V–i (V majeur, sensible) en mineur. */
inline Part cadence (int tonic, bool minor, double seconds)
{
    Part p;
    p.tonic = tonic; p.minor = minor; p.seconds = seconds;
    if (minor) p.progression = { { tonic, true }, { tonic + 5, true }, { tonic + 7, false }, { tonic, true } };
    else       p.progression = { { tonic, false }, { tonic + 5, false }, { tonic + 7, false }, { tonic, false } };
    return p;
}

/** Progression pop : I–vi–IV–V en majeur, i–VI–III–VII en mineur (plus proche de la relative : plus difficile). */
inline Part pop (int tonic, bool minor, double seconds)
{
    Part p;
    p.tonic = tonic; p.minor = minor; p.seconds = seconds;
    if (minor) p.progression = { { tonic, true }, { tonic + 8, false }, { tonic + 3, false }, { tonic + 10, false } };
    else       p.progression = { { tonic, false }, { tonic + 9, true }, { tonic + 5, false }, { tonic + 7, false } };
    return p;
}

class Rng
{
public:
    explicit Rng (std::uint32_t s) : state (s) {}
    double uniform() { state = state * 1664525u + 1013904223u; return (state >> 8) / 16777216.0; }
    double bipolar() { return 2.0 * uniform() - 1.0; }
private:
    std::uint32_t state;
};

inline double songSeconds (const Song& s)
{
    double t = 0.0;
    for (const auto& p : s.parts) t += p.seconds;
    return t;
}

/** Rendu stéréo (canaux séparés), niveau crête ≈ −6 dBFS. */
inline std::vector<std::vector<float>> render (const Song& song, double fs, std::uint32_t seed = 12345)
{
    const double total = songSeconds (song);
    const auto n = (size_t) std::ceil (total * fs);
    std::vector<double> l (n, 0.0), r (n, 0.0);
    constexpr double twoPi = 6.28318530717958647692;
    const double beat = 60.0 / song.bpm, bar = 4.0 * beat;
    const double tune = std::exp2 (song.detuneCents / 1200.0);
    auto hz = [&] (double midi) { return 440.0 * tune * std::exp2 ((midi - 69.0) / 12.0); };
    Rng rng (seed);

    // ajoute une note (harmoniques en 1/h^tilt) entre t0 et t1, enveloppe attaque / maintien / relâchement
    auto note = [&] (double midi, double t0, double dur, double amp, int harmonics, double attack, double decay, double pan)
    {
        const double f0 = hz (midi);
        const size_t i0 = (size_t) std::max (0.0, t0 * fs), i1 = std::min (n, (size_t) ((t0 + dur + 0.08) * fs));
        double phase[8] {};
        for (int h = 0; h < harmonics; ++h) phase[h] = twoPi * rng.uniform();
        const double gl = std::cos (0.25 * twoPi * (pan + 1.0) * 0.5), gr = std::sin (0.25 * twoPi * (pan + 1.0) * 0.5);
        for (size_t i = i0; i < i1; ++i)
        {
            const double t = (double) i / fs - t0;
            double env = std::min (1.0, t / attack);
            if (decay > 0.0) env *= std::exp (-t / decay);
            if (t > dur) env *= std::max (0.0, 1.0 - (t - dur) / 0.08);
            double v = 0.0;
            for (int h = 1; h <= harmonics; ++h)
                if (f0 * h < 0.45 * fs)
                    v += std::sin (twoPi * f0 * h * t + phase[h - 1]) / h;
            v *= amp * env;
            l[i] += gl * v; r[i] += gr * v;
        }
    };

    double t0 = 0.0;
    for (const auto& part : song.parts)
    {
        const int bars = (int) std::floor (part.seconds / bar + 1e-9);
        for (int b = 0; b < bars; ++b)
        {
            const auto& c = part.progression[(size_t) b % part.progression.size()];
            const double tb = t0 + b * bar;
            const int root = ((c.root % 12) + 12) % 12;
            const int third = root + (c.minor ? 3 : 4), fifth = root + 7;
            // nappe (octave 4 : 60 … 71 + voix au-dessus)
            for (int m : { 60 + root, 60 + third, 60 + fifth })
                note (m > 74 ? m - 12 : m, tb, bar - 0.05, 0.10, 6, 0.08, 0.0, rng.bipolar() * 0.5);
            // basse sur les temps
            for (int k = 0; k < 4; ++k)
                note (36 + root, tb + k * beat, beat * 0.9, 0.22, 4, 0.005, 0.35, 0.0);
            // mélodie sur les croches : notes de l'accord, et de la gamme sur les temps faibles
            static constexpr int majorSteps[] = { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[] = { 0, 2, 3, 5, 7, 8, 11 };
            for (int k = 0; k < 8; ++k)
            {
                int pc;
                if (k % 2 == 0)
                {
                    const int pick = (int) (rng.uniform() * 3.0);
                    pc = pick == 0 ? root : pick == 1 ? third : fifth;
                }
                else
                {
                    const int deg = (int) (rng.uniform() * 7.0);
                    pc = part.tonic + (part.minor ? minorSteps[deg] : majorSteps[deg]);
                }
                pc = ((pc % 12) + 12) % 12;
                note (72 + pc, tb + k * beat * 0.5, beat * 0.45, 0.07, 5, 0.01, 0.4, 0.3);
            }
        }
        t0 += part.seconds;
    }

    if (song.drums)
    {
        const int beats = (int) std::floor (total / beat);
        for (int k = 0; k < beats; ++k)
        {
            const double tb = k * beat;
            // grosse caisse : sinus 120 -> 50 Hz
            {
                const size_t i0 = (size_t) (tb * fs), i1 = std::min (n, (size_t) ((tb + 0.25) * fs));
                double ph = 0.0;
                for (size_t i = i0; i < i1; ++i)
                {
                    const double t = (double) i / fs - tb;
                    const double f = 50.0 + 70.0 * std::exp (-t / 0.03);
                    ph += twoPi * f / fs;
                    const double v = 0.45 * std::sin (ph) * std::exp (-t / 0.09);
                    l[i] += v; r[i] += v;
                }
            }
            // caisse claire sur 2 et 4, charleston sur les croches
            auto noise = [&] (double t, double amp, double decay, double hp)
            {
                const size_t i0 = (size_t) (t * fs), i1 = std::min (n, (size_t) ((t + 6.0 * decay) * fs));
                double prev = 0.0, y = 0.0;
                const double a = std::exp (-twoPi * hp / fs);
                for (size_t i = i0; i < i1; ++i)
                {
                    const double x = rng.bipolar();
                    y = a * (y + x - prev);                  // passe-haut du premier ordre
                    prev = x;
                    const double v = amp * y * std::exp (-((double) i / fs - t) / decay);
                    l[i] += v; r[i] += v;
                }
            };
            if (k % 4 == 1 || k % 4 == 3) noise (tb, 0.25, 0.06, 1500.0);
            noise (tb, 0.06, 0.015, 7000.0);
            noise (tb + 0.5 * beat, 0.06, 0.015, 7000.0);
        }
    }

    double peak = 1e-9;
    for (size_t i = 0; i < n; ++i) peak = std::max ({ peak, std::abs (l[i]), std::abs (r[i]) });
    const double g = 0.5 / peak;
    std::vector<std::vector<float>> out (2, std::vector<float> (n));
    for (size_t i = 0; i < n; ++i) { out[0][i] = (float) (l[i] * g); out[1][i] = (float) (r[i] * g); }
    return out;
}

} // namespace aurakey_test

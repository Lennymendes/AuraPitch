// AuraPitch — NextDSP · AuraSuite
// AutoKeyDetector.h : détection automatique de la tonalité (Key + Major / Minor) sur la voix entrante.
//
//   * Histogramme chromatique : chaque trame voisée du PitchDetector ajoute à sa classe de hauteur un poids
//     durée × clarté × niveau (une note tenue compte plus qu'une note de passage). La hauteur est répartie entre les
//     deux demi-tons voisins selon l'écart en cents (une voix fausse de 40 c n'est pas comptée comme juste).
//   * Profils de Temperley (fréquence des degrés dans un corpus, version améliorée des profils de
//     Krumhansl-Schmuckler) pour les 12 toniques en majeur et en mineur.
//   * Décision bayésienne : log P(clé | voix) = N · Σ h(i) log p_clé(i), où h est l'histogramme normalisé et
//     N = 4 notes par seconde voisée (rythme syllabique). La confiance est la probabilité a posteriori de la clé
//     retenue parmi les 24 : elle monte avec la durée d'écoute et avec l'écart entre la meilleure clé et sa
//     concurrente (souvent la relative, Si mineur / Ré majeur).
//
// Temps réel : aucune allocation ; addFrame() et analyse() sont O(1) / O(24 × 12).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace aura::pitch
{

class AutoKeyDetector
{
public:
    struct Result
    {
        int    key        = -1;      // 0 = C … 11 = B, −1 = pas encore de décision
        bool   minor      = false;
        double confidence = 0.0;     // 0 … 1
        double seconds    = 0.0;     // durée voisée écoutée
    };

    static constexpr double kNotesPerSecond = 4.0;

    void reset() noexcept { hist.fill (0.0); seconds = 0.0; }

    /** Une trame voisée : hauteur (MIDI fractionnaire), clarté 0 … 1, niveau dBFS, durée de la trame. */
    void addFrame (double midi, double clarity, double levelDb, double dtSeconds) noexcept
    {
        if (! std::isfinite (midi) || dtSeconds <= 0.0)
            return;
        const double level = std::clamp ((levelDb + 60.0) / 40.0, 0.0, 1.0);   // −60 dBFS -> 0, −20 dBFS et plus -> 1
        const double w = dtSeconds * std::clamp (clarity, 0.0, 1.0) * level;
        if (w <= 0.0)
            return;
        const double lo = std::floor (midi);
        const double frac = midi - lo;
        const int pcLo = (((int) lo % 12) + 12) % 12;
        hist[(size_t) pcLo] += w * (1.0 - frac);
        hist[(size_t) ((pcLo + 1) % 12)] += w * frac;
        seconds += dtSeconds;
    }

    Result analyse() const noexcept
    {
        Result r;
        r.seconds = seconds;
        double total = 0.0;
        for (double v : hist) total += v;
        if (total <= 0.0)
            return r;

        const double n = kNotesPerSecond * seconds;
        std::array<double, 24> logPost {};
        double best = -1.0e300;
        int bestIdx = 0;
        for (int mode = 0; mode < 2; ++mode)
            for (int tonic = 0; tonic < 12; ++tonic)
            {
                const auto& prof = mode == 1 ? logMinor : logMajor;
                double ll = 0.0;
                for (int pc = 0; pc < 12; ++pc)
                    ll += hist[(size_t) pc] / total * prof[(size_t) (((pc - tonic) % 12 + 12) % 12)];
                const int idx = mode * 12 + tonic;
                logPost[(size_t) idx] = n * ll;
                if (logPost[(size_t) idx] > best) { best = logPost[(size_t) idx]; bestIdx = idx; }
            }
        double sum = 0.0;
        for (double v : logPost) sum += std::exp (v - best);
        r.key        = bestIdx % 12;
        r.minor      = bestIdx >= 12;
        r.confidence = 1.0 / sum;
        return r;
    }

    const std::array<double, 12>& histogram() const noexcept { return hist; }

private:
    static std::array<double, 12> logNormalised (std::array<double, 12> p) noexcept
    {
        double s = 0.0;
        for (double v : p) s += v;
        for (auto& v : p) v = std::log (v / s);
        return p;
    }

    // Profils de Temperley (degré 0 = tonique), en log de distribution ; calculés à la construction (pas de statique
    // locale : son initialisation prendrait un verrou sur le thread audio).
    std::array<double, 12> logMajor = logNormalised ({ 0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400 });
    std::array<double, 12> logMinor = logNormalised ({ 0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330 });
    std::array<double, 12> hist {};
    double seconds = 0.0;
};

} // namespace aura::pitch

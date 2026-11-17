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
//
// Étape 6 (AuraKey) : le même modèle reçoit aussi des chromagrammes polyphoniques (addPitchClassWeights), avec un débit
// d'évidence réglable (setEvidenceRate), une mémoire glissante (decay) et les 24 probabilités a posteriori
// (posteriors) pour le cercle des quintes. Les valeurs par défaut gardent le comportement de l'étape 5A.
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

    /** Nombre de « notes » équivalentes par seconde écoutée (4 pour une voix ; plus bas pour un chromagramme). */
    void setEvidenceRate (double notesPerSecond) noexcept { rate = std::max (1.0e-6, notesPerSecond); }

    /** Une trame de chromagramme : poids des 12 classes de hauteur (normalisés ici), poids de la trame (durée × niveau),
        durée écoutée. */
    void addPitchClassWeights (const std::array<double, 12>& w, double weight, double dtSeconds) noexcept
    {
        double sum = 0.0;
        for (double v : w) sum += std::max (0.0, v);
        if (! (sum > 0.0) || ! (weight > 0.0) || ! std::isfinite (sum * weight))
            return;
        for (size_t i = 0; i < 12; ++i)
            hist[i] += weight * std::max (0.0, w[i]) / sum;
        seconds += dtSeconds;
    }

    /** Mémoire glissante : l'historique est multiplié par `factor` (0 … 1). */
    void decay (double factor) noexcept
    {
        for (auto& v : hist) v *= factor;
        seconds *= factor;
    }

    /** Probabilités a posteriori des 24 clés : index = mode × 12 + tonique (0 … 11 majeur, 12 … 23 mineur). */
    void posteriors (std::array<double, 24>& out) const noexcept
    {
        double total = 0.0;
        for (double v : hist) total += v;
        if (total <= 0.0) { out.fill (1.0 / 24.0); return; }
        double best = -1.0e300;
        logLikelihoods (out, rate * seconds, total);
        for (double v : out) best = std::max (best, v);
        double sum = 0.0;
        for (auto& v : out) { v = std::exp (v - best); sum += v; }
        for (auto& v : out) v /= sum;
    }

    double getSeconds() const noexcept { return seconds; }

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

        std::array<double, 24> logPost {};
        logLikelihoods (logPost, rate * seconds, total);
        double best = -1.0e300;
        int bestIdx = 0;
        for (int idx = 0; idx < 24; ++idx)
            if (logPost[(size_t) idx] > best) { best = logPost[(size_t) idx]; bestIdx = idx; }
        double sum = 0.0;
        for (double v : logPost) sum += std::exp (v - best);
        r.key        = bestIdx % 12;
        r.minor      = bestIdx >= 12;
        r.confidence = 1.0 / sum;
        return r;
    }

    const std::array<double, 12>& histogram() const noexcept { return hist; }

private:
    void logLikelihoods (std::array<double, 24>& out, double n, double total) const noexcept
    {
        for (int mode = 0; mode < 2; ++mode)
            for (int tonic = 0; tonic < 12; ++tonic)
            {
                const auto& prof = mode == 1 ? logMinor : logMajor;
                double ll = 0.0;
                for (int pc = 0; pc < 12; ++pc)
                    ll += hist[(size_t) pc] / total * prof[(size_t) (((pc - tonic) % 12 + 12) % 12)];
                out[(size_t) (mode * 12 + tonic)] = n * ll;
            }
    }

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
    double rate = kNotesPerSecond;
};

} // namespace aura::pitch

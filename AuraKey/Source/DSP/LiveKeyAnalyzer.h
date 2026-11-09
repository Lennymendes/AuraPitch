// AuraKey — NextDSP · AuraSuite
// LiveKeyAnalyzer.h : tonalité du flux en temps réel, sur le thread audio.
//
// Bloc audio -> mixage mono (double) -> décimation (≈ 11 kHz) -> chromagramme (une trame / 0,19 s) -> Auto-Key de
// l'étape 5A (profils de Temperley, a posteriori bayésien sur les 24 clés).
//   * Mémoire τ : 8 s, 30 s ou infinie. À chaque trame avec du contenu, l'historique est multiplié par e^{−dt/τ} : avec
//     8 s la clé suit une modulation en ≈ 15 s, avec ∞ elle reste celle de tout le morceau. Les silences ne font pas
//     oublier (rien n'est atténué tant que rien n'est entendu).
//   * Accord du flux (moyenne circulaire glissante, 60 s) appliqué au repli du chromagramme.
//   * L'état publié (clé, confiance, 24 probabilités, chroma lissé, accord, niveau) passe à l'interface par un
//     seqlock à champs atomiques : l'écrivain (audio) ne bloque jamais, le lecteur recommence s'il a lu pendant une
//     écriture.
//
// Temps réel : prepare() alloue ; process() n'alloue pas, ne prend aucun verrou. Les blocs plus longs que la taille
// préparée sont traités par morceaux.
#pragma once

#include "AutoKeyDetector.h"
#include "ChromaAnalyzer.h"
#include "Decimator.h"
#include "KeyTypes.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace aura::key
{

/** Seqlock à un écrivain : la donnée est copiée mot à mot dans des atomiques (aucune course de données). Sans barrière
    autonome (que ThreadSanitizer ne modélise pas) : l'incrément impair est un RMW « acquire » (les écritures de données
    ne remontent pas avant), les données sont écrites en « release » et lues en « acquire » (la relecture du compteur
    ne remonte pas avant elles). */
template <typename T>
class SeqLock
{
    static_assert (std::is_trivially_copyable_v<T>);
    static constexpr size_t kWords = (sizeof (T) + 7) / 8;

public:
    void write (const T& value) noexcept
    {
        std::array<std::uint64_t, kWords> w {};
        std::memcpy (w.data(), &value, sizeof (T));
        const auto s = seq.fetch_add (1, std::memory_order_acquire);          // impair : écriture en cours
        for (size_t i = 0; i < kWords; ++i) words[i].store (w[i], std::memory_order_release);
        seq.store (s + 2, std::memory_order_release);
    }

    /** false si l'écrivain a écrit pendant toutes les tentatives (la valeur n'est alors pas modifiée). */
    bool read (T& value) const noexcept
    {
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto s0 = seq.load (std::memory_order_acquire);
            if ((s0 & 1u) != 0) continue;
            std::array<std::uint64_t, kWords> w {};
            for (size_t i = 0; i < kWords; ++i) w[i] = words[i].load (std::memory_order_acquire);
            if (seq.load (std::memory_order_relaxed) == s0)
            {
                std::memcpy (static_cast<void*> (&value), w.data(), sizeof (T));
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<std::uint32_t> seq { 0 };
    std::array<std::atomic<std::uint64_t>, kWords> words {};
};

struct LiveKeyState
{
    int tonic = -1;                        // −1 : pas encore de décision
    bool minor = false;
    double confidence = 0.0;
    double seconds = 0.0;                  // durée tonale retenue (mémoire comprise)
    std::array<double, 24> posteriors {};
    std::array<double, 12> chroma {};      // chroma récent lissé (≈ 1 s), max = 1
    double tuningCents = 0.0;
    double levelDb = -200.0;
    std::uint32_t frames = 0;              // trames analysées depuis la remise à zéro
};

class LiveKeyAnalyzer
{
public:
    /** Mémoire en secondes ; 0 = infinie. */
    void setMemorySeconds (double s) noexcept { memorySeconds.store (s, std::memory_order_relaxed); }
    /** Vide l'historique (pris en compte au bloc suivant). */
    void requestReset() noexcept { resetRequested.store (true, std::memory_order_release); }
    bool getState (LiveKeyState& s) const noexcept { return published.read (s); }

    void prepare (double sampleRate, int maxBlock)
    {
        decimator.prepare (sampleRate);
        chroma.prepare (decimator.getOutputRate());
        mono.assign ((size_t) std::max (1, maxBlock), 0.0);
        decimated.assign ((size_t) std::max (1, maxBlock / decimator.getFactor() + 2), 0.0);
        clearHistory();
    }

    template <typename Sample>
    void process (const Sample* const* channels, int numChannels, int n) noexcept
    {
        if (resetRequested.exchange (false, std::memory_order_acquire))
            clearHistory();
        if (numChannels <= 0) return;
        const int chunk = (int) mono.size();
        for (int start = 0; start < n; start += chunk)
        {
            const int len = std::min (chunk, n - start);
            const double g = 1.0 / numChannels;
            for (int i = 0; i < len; ++i)
            {
                double acc = 0.0;
                for (int c = 0; c < numChannels; ++c) acc += (double) channels[c][start + i];
                mono[(size_t) i] = acc * g;
            }
            int count = 0;
            decimator.push (mono.data(), len, [this, &count] (double v) { decimated[(size_t) count++] = v; });
            chroma.push (decimated.data(), count, [this] (const ChromaFrame& f) { addFrame (f); });
        }
    }

private:
    void clearHistory() noexcept
    {
        decimator.reset();
        chroma.reset();
        detector.reset();
        detector.setEvidenceRate (kChromaEvidenceRate);
        tuneX = tuneY = tuneW = 0.0;
        state = LiveKeyState {};
        state.posteriors.fill (1.0 / 24.0);
        published.write (state);
    }

    void addFrame (const ChromaFrame& f) noexcept
    {
        state.levelDb = f.levelDb;
        ++state.frames;
        if (f.weight > 0.0)
        {
            const double tauTune = std::exp (-f.dt / 60.0);
            tuneX = tuneX * tauTune + f.tuneX;
            tuneY = tuneY * tauTune + f.tuneY;
            tuneW = tuneW * tauTune + f.tuneW;
            const double tuning = ChromaAnalyzer::tuningFrom (tuneX, tuneY, tuneW);
            const auto pcs = ChromaAnalyzer::fold (f.fine, tuning);
            const double mem = memorySeconds.load (std::memory_order_relaxed);
            if (mem > 0.0)
                detector.decay (std::exp (-f.dt / mem));
            detector.addPitchClassWeights (pcs, f.weight, f.dt);

            double mx = 0.0;
            for (int k = 0; k < 12; ++k)
            {
                state.chroma[(size_t) k] = 0.8 * state.chroma[(size_t) k] + 0.2 * pcs[(size_t) k];
                mx = std::max (mx, state.chroma[(size_t) k]);
            }
            displayMax = mx;
            const auto r = detector.analyse();
            state.tonic = r.key;
            state.minor = r.minor;
            state.confidence = r.confidence;
            state.seconds = detector.getSeconds();
            state.tuningCents = 100.0 * tuning;
            detector.posteriors (state.posteriors);
        }
        LiveKeyState shown = state;
        if (displayMax > 0.0)
            for (auto& v : shown.chroma) v /= displayMax;
        published.write (shown);
    }

    Decimator decimator;
    ChromaAnalyzer chroma;
    aura::pitch::AutoKeyDetector detector;
    std::vector<double> mono, decimated;
    LiveKeyState state;
    double tuneX = 0.0, tuneY = 0.0, tuneW = 0.0, displayMax = 0.0;
    std::atomic<double> memorySeconds { 30.0 };
    std::atomic<bool> resetRequested { false };
    SeqLock<LiveKeyState> published;
};

} // namespace aura::key

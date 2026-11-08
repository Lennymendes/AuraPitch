// AuraPitch — NextDSP · AuraSuite
// PSOLAShifter.h : TD-PSOLA temps réel (sans JUCE), 1 ou 2 canaux, 44,1 … 384 kHz. Utilisé pour les effets
// (FormantShift, grandes transpositions avec Preserve) ; la correction courante passe par SpliceShifter.
//
// Principe
//   * Marques d'analyse : chaque marque est propagée de la précédente par corrélation croisée locale (NCC sur le
//     signal passe-bas, ±Ta/4, raffinement parabolique) : elles restent sur le même point du cycle glottique,
//     ancrées sur le centre d'énergie de la période. La grille est extrapolée vers l'avant.
//   * Grains : fenêtre de Hann de ±Ta autour de la marque d'analyse m (dans le domaine d'entrée), posée sur la marque
//     de synthèse ts ; les marques de synthèse sont espacées de Ts = Ta / ratio.
//   * Lecture en continu : un grain ne copie rien, il lit l'anneau d'entrée (sinc 48 points, banc
//     anti-repliement choisi selon α) avec un décalage o = ts − m. La sortie
//     de l'échantillon n n'utilise que des entrées ≤ n − 2 : le moteur est causal avec une latence D fixe (2 ms).
//   * Formants (domaine temporel) : le contenu du grain est rééchantillonné d'un facteur α autour de la marque
//     (α > 1 : formants vers le haut). FormantPreserve : α = 2^(FormantShift/12) ; sinon α = ratio × 2^(…/12)
//     (les formants suivent la hauteur, comme un rééchantillonnage).
//   * OLA à gain constant : la somme des fenêtres est normalisée par le recouvrement attendu (demi-fenêtre /
//     espacement) : au rapport 1 la sortie est exactement l'entrée retardée de D, même si la période varie d'une
//     marque à l'autre ; en montée chaque impulsion glottique garde son amplitude ; en descente, gain 1/√max(r, ½)
//     lissé (+3 dB au plus à −12 dt) : niveau RMS constant à ±0,5 dB.
//   * Verrouillage Dry : au rapport 1 (et à chaque reprise après un passage non voisé), les marques de synthèse
//     sont recalées sur « marque d'analyse + D » : o = D, la sortie est en phase avec le Dry retardé de D.
//   * Non voisé : fondu d'une période (Ta) vers l'entrée retardée de D ; pendant le bypass, les grains continuent
//     d'être posés en o = D, si bien que la reprise part d'une sortie identique à l'entrée (aucun clic).
//
// Contrainte physique (validée) : un grain causal décrit une hauteur passée ; le contenu corrigé sort avec un
// retard de D à D + Ta (au plus une période) quand ratio ≠ 1. Aucune allocation après prepare().
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Interpolator.h"

namespace aura::pitch
{

class PSOLAShifter
{
public:
    static constexpr int    kMaxChannels = 2;
    static constexpr int    kMaxGrains   = 32;
    static constexpr double kMinRatio    = 0.2,  kMaxRatio = 5.0;
    static constexpr double kMinAlpha    = 0.5,  kMaxAlpha = 4.0;

    //--- préparation ----------------------------------------------------------------------------------------------
    void prepare (double newSampleRate, int numChannels, double latencySeconds = 0.002)
    {
        sampleRate = newSampleRate;
        channels   = std::clamp (numChannels, 1, kMaxChannels);
        latency    = std::max (4, (int) std::lround (latencySeconds * sampleRate));

        int size = 1;
        while (size < (int) std::ceil (0.35 * sampleRate) + latency) size <<= 1;
        ringMask = size - 1;
        for (auto& r : rings) r.assign ((size_t) size, 0.0);
        lpRing.assign ((size_t) size, 0.0);
        energyRing.assign ((size_t) size, 0.0);
        energyCoeff = 1.0 - std::exp (-1.0 / (0.00025 * newSampleRate));     // énergie lissée sur 0,25 ms
        interp.prepare();

        for (int i = 0; i <= kWindowSize; ++i)
            window[(size_t) i] = 0.5 * (1.0 + std::cos (3.14159265358979323846 * (double) i / kWindowSize));

        lpCoeff = 1.0 - std::exp (-2.0 * 3.14159265358979323846 * 1200.0 / sampleRate);
        compCoeff = 1.0 - std::exp (-1.0 / (0.020 * sampleRate));
        minPeriod = sampleRate / 1400.0;
        maxPeriod = sampleRate / 45.0;
        reset();
    }

    void reset() noexcept
    {
        for (auto& r : rings) std::fill (r.begin(), r.end(), 0.0);
        std::fill (lpRing.begin(), lpRing.end(), 0.0);
        std::fill (energyRing.begin(), energyRing.end(), 0.0);
        energy = 0.0;
        for (auto& g : grains) g.active = false;
        counter = 0;
        lp1 = lp2 = 0.0;
        period = sampleRate * 0.005;
        lastMark = 0.0;
        pendingMark = period;
        numMarks = 0;
        ratio = 1.0; alpha = 1.0;
        compGain = 1.0;
        voicedTarget = false;
        gain = 0.0;
        nextStart = 0.0;
        nextTs = 0.0;
        needsRelock = true;
        locked = false;
        lastGrainMark = 0.0;
    }

    int    getLatencySamples() const noexcept { return latency; }

    /** Inactif : l'historique, les marques et les grains restent à jour (reprise instantanée), mais aucun grain
        n'est calculé et la sortie est l'entrée retardée de D. */
    void setActive (bool a) noexcept { active = a; }
    double getPeriod() const noexcept         { return period; }

    //--- commande (thread audio, à chaque estimation du détecteur) --------------------------------------------------
    void setPitch (double periodSamples, bool voiced) noexcept
    {
        if (voiced && periodSamples >= minPeriod && periodSamples <= maxPeriod)
            period = periodSamples;
        voicedTarget = voiced;
    }

    void setShift (double pitchRatio, double formantFactor, bool preserveFormants) noexcept
    {
        ratio = std::clamp (pitchRatio, kMinRatio, kMaxRatio);
        const double f = std::clamp (formantFactor, 0.5, 2.0);
        alpha = std::clamp (preserveFormants ? f : ratio * f, kMinAlpha, kMaxAlpha);
    }

    //--- traitement -----------------------------------------------------------------------------------------------
    /** in et out peuvent être les mêmes tampons. */
    template <typename SampleType>
    void process (const SampleType* const* in, SampleType* const* out, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const std::int64_t n = counter;
            const size_t slot = (size_t) (n & ringMask);

            double mono = 0.0;
            for (int ch = 0; ch < channels; ++ch)
            {
                double v = (double) in[ch][i];
                if (! std::isfinite (v)) v = 0.0;
                rings[(size_t) ch][slot] = v;
                mono += v;
            }
            mono /= (double) channels;
            lp1 += lpCoeff * (mono - lp1);
            lp2 += lpCoeff * (lp1 - lp2);
            lpRing[slot] = lp2 + 1.0e-30;
            energy += energyCoeff * (mono * mono - energy);
            energyRing[slot] = energy;

            while ((double) n >= pendingMark + 0.6 * period + 1.0)
                confirmMark (n);

            while ((double) n >= nextStart)
                createGrain (n);

            // Rendu des grains
            std::array<double, kMaxChannels> acc {};
            double sumW = 0.0, sumWD = 0.0, unlockedW = 0.0;
            const bool render = active && (gain > 0.0 || voicedTarget);
            for (auto& g : grains)
            {
                if (! g.active) continue;
                const double k = (double) n - g.ts;
                if (k >= g.halfLength) { g.active = false; continue; }
                if (k <= -g.halfLength || ! render) continue;

                const double w   = windowAt (k * g.invHalf);
                const double pos = (double) n - g.offset + k * (g.alpha - 1.0);
                for (int ch = 0; ch < channels; ++ch)
                    acc[(size_t) ch] += w * interp.read (rings[(size_t) ch].data(), ringMask, pos, g.bank);
                sumW  += w;
                sumWD += w * g.density;
                if (! g.locked) unlockedW += w;
            }
            // Gain constant : la somme des fenêtres est ramenée à 1 compte tenu du recouvrement attendu
            // (densité ρ = demi-fenêtre / espacement, ≥ 1 en montée) ; en descente, PSOLA classique (pas de comblement).
            // Grains tous verrouillés (même décalage) : division exacte par Σw -> sortie = entrée retardée.
            const double norm = sumW <= 1.0e-9 ? 1.0
                              : unlockedW <= 0.0 ? sumW
                              : std::max (sumW * sumW / sumWD, 0.9);

            // Fondu voisé / non voisé d'une période
            const double step = 1.0 / std::max (period, 16.0);
            gain = voicedTarget ? std::min (1.0, gain + step) : std::max (0.0, gain - step);
            if (gain <= 0.0 && ! voicedTarget)
                needsRelock = true;

            // Compensation de niveau en descente : les impulsions gardent leur amplitude mais s'espacent, la puissance
            // suit le rapport r ; gain 1/√max(r, ½) (jusqu'à +3 dB à −12 dt), lissé sur 20 ms.
            const double compTarget = ratio < 1.0 ? 1.0 / std::sqrt (std::max (ratio, 0.5)) : 1.0;
            compGain += compCoeff * (compTarget - compGain);
            if (std::abs (compTarget - compGain) < 1.0e-12) compGain = compTarget;

            const size_t drySlot = (size_t) ((n - latency) & ringMask);
            for (int ch = 0; ch < channels; ++ch)
            {
                const double dry = rings[(size_t) ch][drySlot];
                const double wet = sumW > 1.0e-9 && render ? compGain * acc[(size_t) ch] / norm : dry;
                out[ch][i] = (SampleType) (dry + gain * (wet - dry));
            }
            ++counter;
        }
    }

private:
    static constexpr int kWindowSize = 4096;
    static constexpr int kHistory    = 128;

    struct Grain
    {
        double ts = 0.0, offset = 0.0, alpha = 1.0, halfLength = 1.0, invHalf = 1.0, density = 1.0;
        bool   active = false, locked = false;
        int    bank = 0;
    };

    double windowAt (double u) const noexcept
    {
        const double x = std::min (std::abs (u), 1.0) * kWindowSize;
        const int    i = std::min ((int) x, kWindowSize - 1);
        const double f = x - (double) i;
        return window[(size_t) i] + f * (window[(size_t) i + 1] - window[(size_t) i]);
    }

    /** Marques d'analyse. Première marque d'une phrase : maximum du passe-bas (pic glottique) dans ±Ta/4. Ensuite,
        la marque suivante est placée à la période T* (±10 % de Ta) qui maximise la corrélation normalisée entre la
        période centrée sur la marque précédente et la suivante : deux grains voisins décrivent la même phase du cycle
        glottique et s'additionnent en phase, même quand la période réelle tremble (jitter) ou glisse plus vite que le
        détecteur. Si la corrélation tombe sous 0,5 (attaque, changement de registre), on revient au pic. */
    void confirmMark (std::int64_t now) noexcept
    {
        const double pred = pendingMark;
        const double half = 0.25 * period;
        double mark = pred;
        bool fromCorrelation = false;

        if (numMarks > 0)
        {
            const int T = std::max (8, (int) std::lround (period));
            const int W = T, stride = std::max (1, T / 96);
            const std::int64_t a = (std::int64_t) std::lround (lastMark) - W / 2;
            auto ncc = [&] (int lag, int st) noexcept
            {
                double xy = 0, xx = 0, yy = 0;
                for (int k = 0; k < W; k += st)
                {
                    const double x = lpRing[(size_t) ((a + k) & ringMask)];
                    const double y = lpRing[(size_t) ((a + lag + k) & ringMask)];
                    xy += x * y; xx += x * x; yy += y * y;
                }
                return xy / std::sqrt (xx * yy + 1.0e-30);
            };
            const int lo = (int) (0.9 * T), hi = (int) std::ceil (1.1 * T);
            int best = T; double bestC = -2.0;
            for (int lag = lo; lag <= hi; lag += stride)
            {
                if (a + lag + W > now) break;
                const double c = ncc (lag, stride);
                if (c > bestC) { bestC = c; best = lag; }
            }
            for (int lag = best - stride + 1; lag < best + stride; ++lag)
            {
                if (lag < lo || lag > hi || lag == best || a + lag + W > now) continue;
                const double c = ncc (lag, 1);
                if (c > bestC) { bestC = c; best = lag; }
            }
            if (bestC > 0.5)
            {
                double frac = 0.0;
                if (best > lo && best < hi && a + best + 1 + W <= now)
                {
                    const double cm = ncc (best - 1, 1), c0 = ncc (best, 1), cp = ncc (best + 1, 1);
                    const double den = cm - 2.0 * c0 + cp;
                    if (den < 0.0) frac = std::clamp (0.5 * (cm - cp) / den, -0.5, 0.5);
                }
                mark = lastMark + (double) best + frac;
                fromCorrelation = true;
            }
        }

        if (! fromCorrelation)
        {
            // Ancrage sur le centre d'énergie du cycle (impulsion glottique) dans ±T/2 : les fenêtres de Hann
            // coupent ensuite là où le cycle est le plus calme.
            std::int64_t lo = (std::int64_t) std::ceil (pred - 2.0 * half);
            std::int64_t hi = std::min ((std::int64_t) std::floor (pred + 2.0 * half), now);
            lo = std::max (lo, now - (std::int64_t) ringMask + 4);
            double peakPos = pred, best = -1.0e300;
            for (std::int64_t t = lo; t <= hi; ++t)
            {
                const double v = energyRing[(size_t) (t & ringMask)];
                if (v > best) { best = v; peakPos = (double) t; }
            }
            mark = peakPos;
        }
        else
        {
            // Propagation par corrélation, rappelée doucement vers le centre d'énergie (dérive nulle à long terme).
            std::int64_t lo = (std::int64_t) std::ceil (mark - half);
            std::int64_t hi = std::min ((std::int64_t) std::floor (mark + half), now);
            lo = std::max (lo, now - (std::int64_t) ringMask + 4);
            double peakPos = mark, best = -1.0e300;
            for (std::int64_t t = lo; t <= hi; ++t)
            {
                const double v = energyRing[(size_t) (t & ringMask)];
                if (v > best) { best = v; peakPos = (double) t; }
            }
            mark += 0.1 * (peakPos - mark);
        }

        history[(size_t) (numMarks % kHistory)] = mark;
        ++numMarks;
        lastMark = mark;
        pendingMark = mark + period;
    }

    /** Dernière marque d'analyse ≤ t (historique confirmé, ou grille extrapolée au-delà de la dernière). */
    double markAtOrBefore (double t) const noexcept
    {
        if (t >= lastMark)
            return lastMark + std::floor ((t - lastMark) / period) * period;
        const int count = (int) std::min<std::int64_t> (numMarks, kHistory);
        for (int j = 1; j <= count; ++j)
        {
            const double m = history[(size_t) ((numMarks - j) % kHistory)];
            if (m <= t) return m;
        }
        return t;
    }

    /** Première marque d'analyse ≥ t. */
    double markAtOrAfter (double t) const noexcept
    {
        if (t >= lastMark)
            return lastMark + std::ceil ((t - lastMark) / period) * period;
        const int count = (int) std::min<std::int64_t> (numMarks, kHistory);
        double candidate = lastMark;
        for (int j = 1; j <= count; ++j)
        {
            const double m = history[(size_t) ((numMarks - j) % kHistory)];
            if (m < t) break;
            candidate = m;
        }
        return candidate;
    }

    void createGrain (std::int64_t now) noexcept
    {
        const double ta      = period;
        const double half    = ta / alpha;                                   // demi-fenêtre en sortie
        // Causalité : la lecture du bord droit (k = half) doit rester ≤ now − 2.
        const double extra   = std::max (0.0, ta - half) + 3.0 + SincInterpolator::kLookahead;
        const double oLock   = std::max ((double) latency, extra);
        const bool   unity   = std::abs (ratio - 1.0) < 1.0e-6 && std::abs (alpha - 1.0) < 1.0e-9;

        double ts, m;
        if (needsRelock)
        {
            // Reprise (démarrage, sortie de bypass) : grain posé exactement sur « marque + D ».
            m  = markAtOrAfter ((double) now + half - oLock);
            ts = m + oLock;
            needsRelock = false;
            locked = true;
        }
        else if (locked && unity)
        {
            // Rapport 1 verrouillé : les marques de synthèse suivent les marques d'analyse (+ D) ;
            // tous les grains lisent avec le même décalage : sortie = entrée retardée de D, au sample près.
            m  = markAtOrAfter (lastGrainMark + 0.5 * ta);
            ts = m + oLock;
        }
        else
        {
            locked = false;
            ts = std::max (nextTs, (double) now + 1.0 - half);
            m  = markAtOrBefore (ts - oLock);
        }

        double offset = ts - m;
        double nextSpacing = ta / ratio;
        if (! locked && std::abs (ratio - 1.0) < 1.0e-3 && std::abs (alpha - 1.0) < 1.0e-9)
        {
            // Proche du rapport 1 : recalage lent de la grille de synthèse sur « analyse + D »
            // (≤ 0,2 % de période par grain : < 3,5 cents de déviation transitoire), puis verrouillage.
            double e = offset - oLock;
            if (e > 0.5 * ta) e -= ta;
            if (unity && std::abs (e) < 0.01)
            {
                ts = m + oLock; offset = oLock; locked = true;
            }
            else
            {
                nextSpacing -= std::clamp (0.5 * e, -0.002 * ta, 0.002 * ta);
            }
        }
        if (locked)
            offset = oLock;

        for (auto& g : grains)
        {
            if (g.active) continue;
            g.ts = ts; g.offset = offset; g.alpha = alpha;
            g.halfLength = half; g.invHalf = 1.0 / half; g.active = true;
            g.bank = SincInterpolator::bankFor (alpha);
            g.density = locked ? 1.0 : std::max (1.0, half / nextSpacing);
            g.locked = locked;
            break;
        }
        lastGrainMark = m;

        nextTs    = locked ? m + ta + oLock : ts + nextSpacing;
        nextStart = std::max (nextTs - half, (double) now + 1.0);
    }

    double sampleRate = 44100.0;
    int    channels = 1, latency = 88;
    std::int64_t ringMask = 0, counter = 0;
    std::array<std::vector<double>, kMaxChannels> rings;
    std::vector<double> lpRing;
    std::array<double, kWindowSize + 1> window {};
    std::array<Grain, kMaxGrains> grains {};
    std::array<double, kHistory> history {};
    SincInterpolator interp;
    std::int64_t numMarks = 0;

    double lpCoeff = 0.1, lp1 = 0.0, lp2 = 0.0, energyCoeff = 0.1, energy = 0.0;
    std::vector<double> energyRing;
    double minPeriod = 30.0, maxPeriod = 1000.0;
    double period = 220.0, lastMark = 0.0, pendingMark = 220.0;
    double ratio = 1.0, alpha = 1.0;
    bool   voicedTarget = false, needsRelock = true, locked = false, active = true;
    double lastGrainMark = 0.0;
    double gain = 0.0, nextStart = 0.0, nextTs = 0.0;
    double compGain = 1.0, compCoeff = 0.001;
};

} // namespace aura::pitch

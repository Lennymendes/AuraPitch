// AuraPitch — NextDSP · AuraSuite
// StudioPhaseVocoder.h : moteur haute précision du mode Studio/DXD (sans JUCE), 1 ou 2 canaux, 44,1 … 384 kHz.
//
// Architecture « delta » multicadence
//
//     x ─► ÷2 … ÷2 ─► d (fsV = 44,1 / 48 kHz) ─► vocodeur ─► v ─┐
//                       └───────► retard N − 1 ────────────────┴─(−)─► δ ─► ×2 … ×2 ─► Δ
//     sortie Wet = x retardé de L + Δ
//
//   * Le vocodeur ne travaille que dans la bande vocale, à 44,1 ou 48 kHz, quelle que soit l'entrée : même
//     résolution en Hz dans les graves (jusqu'à 11,7 Hz par bin), même coût, de 44,1 kHz au DXD 384 kHz.
//   * Il ne fournit qu'une correction δ = v − d : quand il ne modifie rien (non voisé, rapport 1), δ = 0 et la sortie
//     est EXACTEMENT l'entrée retardée, Dry inclus : aucun filtrage en peigne pour Mix < 100 %. Le contenu au-delà de
//     la bande vocale (l'air du DXD, 20 … 190 kHz) n'est jamais filtré ni décalé : il traverse en retard pur.
//   * Les demi-bandes (127 coefficients, ≈ 110 dB) n'ont pas besoin d'être parfaites : leurs défauts s'annulent dans
//     la différence v − d tant que le vocodeur est neutre.
//
// Vocodeur (Laroche & Dolson 1999, « New phase-vocoder techniques for pitch-shifting… »)
//   * STFT Hann à fsV, saut N/8 : N = 2048 (43 ms, Soprano / Alto-Tenor : vibrato et attaques) ou 4096 (85 ms,
//     Low Male / Instrument : harmoniques séparées jusqu'à 50 Hz), même latence ; fréquence instantanée par
//     différence de phase.
//   * Décalage par pics : chaque région d'influence (pic ± minima voisins) est déplacée d'un nombre entier de bins
//     et tournée d'un phaseur Z commun : verrouillage de phase VERTICAL (toute la région garde ses phases relatives).
//     Z(u) = Z(u−1) · e^{jR(β−1)ω} : la phase progresse de βωR par saut (verrouillage HORIZONTAL).
//   * Transitoires : flux spectral > 0,5 avec hausse d'énergie > 3 dB -> Z = 1 (phases de l'entrée) : attaque intacte.
//   * Formants : enveloppe vraie (FormantProcessor.h) ; chaque bin reçoit le gain E'(k')/E(k), E' = E déformée de γ
//     (FormantPreserve : γ = 2^(FormantShift/12) ; sinon γ = β · 2^(…/12)).
//   * Trame non voisée ou neutre : recopie de la trame d'entrée (OLA temporel, sans FFT) et Z remis à 1. L'OLA de
//     Hann sur 85 ms assure un fondu doux et sans clic entre trames corrigées et neutres.
//   * Stéréo : pics, phases, enveloppe et Z calculés sur la moyenne des canaux, appliqués à chaque canal :
//     l'image stéréo et les phases inter-canaux sont conservées.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "FFT.h"
#include "FormantProcessor.h"
#include "HalfBand.h"

namespace aura::pitch
{

//==============================================================================================================
/** Commande d'une trame du vocodeur. */
struct FrameControl
{
    double ratio          = 1.0;    // rapport de hauteur β
    double formantFactor  = 1.0;    // 2^(FormantShift/12)
    bool   preserve       = true;   // FormantPreserve
    bool   voiced         = false;
    double f0             = 0.0;    // Hz (ordre de l'enveloppe)
};

/** Piste de commande horodatée (horloge d'entrée) : les estimations du détecteur y sont rangées à leur instant
    frameCentre ; le vocodeur lit la valeur à l'instant du centre de chaque trame. */
class PitchControlTrack
{
public:
    struct Point
    {
        std::int64_t time = 0;
        double shiftSemitones = 0.0, formantSemitones = 0.0, f0 = 0.0;
        bool   voiced = false, preserve = true;
    };

    void reset() noexcept { count = 0; }

    void push (const Point& p) noexcept
    {
        points[(size_t) (count % kSize)] = p;
        ++count;
    }

    /** Commande à l'instant t. Une trame est traitée comme voisée si une estimation voisée tombe dans ±span
        autour de son centre (attaque ou fin de note dans la partie utile de la fenêtre) ; le rapport est alors
        celui de l'instant t, ou de l'estimation voisée la plus proche si t lui-même est non voisé. */
    FrameControl at (std::int64_t t, std::int64_t span = 0) const noexcept
    {
        FrameControl c;
        const int available = (int) std::min<std::int64_t> (count, kSize);
        if (available == 0) return c;

        const Point* before = nullptr;      // dernier point ≤ t
        const Point* after  = nullptr;      // premier point > t
        const Point* nearestVoiced = nullptr;
        std::int64_t nearestDist = span + 1;
        for (int j = 1; j <= available; ++j)
        {
            const Point& p = points[(size_t) ((count - j) % kSize)];
            if (p.time > t) after = &p;
            else if (before == nullptr) before = &p;
            const std::int64_t dist = p.time > t ? p.time - t : t - p.time;
            if (p.voiced && dist < nearestDist) { nearestDist = dist; nearestVoiced = &p; }
            if (p.time < t - span) break;
        }
        if (before == nullptr) return c;     // plus ancien que l'historique : neutre

        double shift = before->shiftSemitones;
        bool voiced = before->voiced;
        if (after != nullptr && after->time > before->time)
        {
            const double f = (double) (t - before->time) / (double) (after->time - before->time);
            shift += f * (after->shiftSemitones - before->shiftSemitones);
            if (f > 0.5) voiced = after->voiced;
        }
        const Point* ref = before;
        if (! voiced && nearestVoiced != nullptr)
        {
            voiced = true;
            shift = nearestVoiced->shiftSemitones;
            ref = nearestVoiced;
        }
        c.ratio = std::exp2 (shift / 12.0);
        c.formantFactor = std::exp2 (ref->formantSemitones / 12.0);
        c.preserve = ref->preserve;
        c.voiced = voiced;
        c.f0 = ref->f0;
        return c;
    }

private:
    static constexpr int kSize = 2048;
    std::array<Point, kSize> points {};
    std::int64_t count = 0;
};

//==============================================================================================================
class PhaseVocoderCore
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double fs, int numChannels, int fftOrder)
    {
        sampleRate = fs;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        n = 1 << fftOrder;
        hop = n / 8;
        bins = n / 2 + 1;
        fft.prepare (fftOrder);
        envelope.prepare (fftOrder);

        constexpr double twoPi = 6.28318530717958647692;
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            window[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * i / n);   // Hann périodique : Σw² = 3 au saut N/8

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            inRing[(size_t) ch].allocate (n);
            acc[(size_t) ch].assign ((size_t) n, 0.0);
            outQueue[(size_t) ch].assign ((size_t) hop, 0.0);
            xr[(size_t) ch].assign ((size_t) bins, 0.0);
            xi[(size_t) ch].assign ((size_t) bins, 0.0);
            yr[(size_t) ch].assign ((size_t) bins, 0.0);
            yi[(size_t) ch].assign ((size_t) bins, 0.0);
        }
        frame.assign ((size_t) n, 0.0);
        mag.assign ((size_t) bins, 0.0); phase.assign ((size_t) bins, 0.0);
        prevMag.assign ((size_t) bins, 0.0); prevPhase.assign ((size_t) bins, 0.0);
        logMag.assign ((size_t) bins, 0.0); env.assign ((size_t) bins, 0.0); envTarget.assign ((size_t) bins, 0.0);
        prevRotR.assign ((size_t) bins, 1.0); prevRotI.assign ((size_t) bins, 0.0);
        curRotR.assign ((size_t) bins, 1.0);  curRotI.assign ((size_t) bins, 0.0);
        peaks.assign ((size_t) bins, 0);
        envPeaks.assign ((size_t) bins, 0);
        reset();
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            inRing[(size_t) ch].reset();
            std::fill (acc[(size_t) ch].begin(), acc[(size_t) ch].end(), 0.0);
            std::fill (outQueue[(size_t) ch].begin(), outQueue[(size_t) ch].end(), 0.0);
        }
        std::fill (prevMag.begin(), prevMag.end(), 0.0);
        std::fill (prevPhase.begin(), prevPhase.end(), 0.0);
        std::fill (prevRotR.begin(), prevRotR.end(), 1.0);
        std::fill (prevRotI.begin(), prevRotI.end(), 0.0);
        hopCounter = 0;
        queuePos = hop;
        prevEnergy = 0.0;
        transientCount = 0;
        framesProcessed = 0;
    }

    int getFftSize() const noexcept        { return n; }
    int getHop() const noexcept            { return hop; }
    int getLatencySamples() const noexcept { return n - 1; }      // à fsV
    std::int64_t getTransientCount() const noexcept { return transientCount; }
    void setTransientResetEnabled (bool e) noexcept  { transientReset = e; }

    /** Un échantillon par canal à fsV. getControl() est appelé une fois par trame (tous les N/8 échantillons). */
    /** Alimente l'historique sans calculer de trame (cœur inactif, prêt à prendre le relais). */
    void feed (const double* in) noexcept
    {
        for (int ch = 0; ch < channels; ++ch)
            inRing[(size_t) ch].push (in[ch]);
        if (++hopCounter >= hop) hopCounter = 0;
    }

    template <typename GetControl>
    void push (const double* in, double* out, GetControl&& getControl) noexcept
    {
        for (int ch = 0; ch < channels; ++ch)
            inRing[(size_t) ch].push (in[ch]);

        if (++hopCounter >= hop)
        {
            hopCounter = 0;
            processFrame (getControl());
            for (int ch = 0; ch < channels; ++ch)
            {
                auto& a = acc[(size_t) ch];
                std::copy (a.begin(), a.begin() + hop, outQueue[(size_t) ch].begin());
                std::copy (a.begin() + hop, a.end(), a.begin());
                std::fill (a.end() - hop, a.end(), 0.0);
            }
            queuePos = 0;
        }

        for (int ch = 0; ch < channels; ++ch)
            out[ch] = queuePos < hop ? outQueue[(size_t) ch][(size_t) queuePos] : 0.0;
        if (queuePos < hop) ++queuePos;
    }

private:
    static double princarg (double p) noexcept
    {
        constexpr double pi = 3.14159265358979323846, twoPi = 2.0 * pi;
        return p - twoPi * std::floor ((p + pi) / twoPi);
    }

    void processFrame (const FrameControl& c) noexcept
    {
        constexpr double twoPi = 6.28318530717958647692;
        const double olaScale = 1.0 / 3.0;
        ++framesProcessed;

        // Analyse de la moyenne des canaux (pics, phases, enveloppe, transitoires)
        for (int ch = 0; ch < channels; ++ch)
        {
            const double* src = inRing[(size_t) ch].last (n);
            for (int i = 0; i < n; ++i) frame[(size_t) i] = src[i] * window[(size_t) i];
            fft.forward (frame.data(), xr[(size_t) ch].data(), xi[(size_t) ch].data());
        }
        double energy = 0.0, flux = 0.0, magSum = 0.0, maxMag = 0.0;
        for (int k = 0; k < bins; ++k)
        {
            double r = 0.0, im = 0.0;
            for (int ch = 0; ch < channels; ++ch) { r += xr[(size_t) ch][(size_t) k]; im += xi[(size_t) ch][(size_t) k]; }
            r /= channels; im /= channels;
            const double m = std::sqrt (r * r + im * im);
            mag[(size_t) k] = m;
            phase[(size_t) k] = std::atan2 (im, r);
            energy += m * m;
            magSum += m;
            maxMag = std::max (maxMag, m);
            flux += std::max (0.0, m - prevMag[(size_t) k]);
        }
        const bool transient = transientReset && magSum > 1.0e-12 && flux / magSum > 0.5 && energy > 2.0 * prevEnergy;
        if (transient) ++transientCount;
        prevEnergy = energy;

        const double gammaTarget = c.preserve ? c.formantFactor : c.ratio * c.formantFactor;
        const bool neutral = ! c.voiced || (std::abs (c.ratio - 1.0) < 1.0e-9 && std::abs (gammaTarget - 1.0) < 1.0e-9);

        if (neutral || maxMag <= 1.0e-15)
        {
            // Recopie exacte de la trame (OLA temporel) ; rotations remises à zéro.
            for (int ch = 0; ch < channels; ++ch)
            {
                const double* src = inRing[(size_t) ch].last (n);
                auto& a = acc[(size_t) ch];
                for (int i = 0; i < n; ++i)
                    a[(size_t) i] += src[i] * window[(size_t) i] * window[(size_t) i] * olaScale;
            }
            std::fill (prevRotR.begin(), prevRotR.end(), 1.0);
            std::fill (prevRotI.begin(), prevRotI.end(), 0.0);
            std::copy (mag.begin(), mag.end(), prevMag.begin());
            std::copy (phase.begin(), phase.end(), prevPhase.begin());
            return;
        }

        const double floorMag = maxMag * 1.0e-5;                     // −100 dB sous le pic maximal
        // Pics
        int numPeaks = 0;
        for (int k = 2; k < bins - 2; ++k)
        {
            const double m = mag[(size_t) k];
            if (m > floorMag && m > mag[(size_t) k - 1] && m >= mag[(size_t) k + 1]
                && m > mag[(size_t) k - 2] && m >= mag[(size_t) k + 2])
                peaks[(size_t) numPeaks++] = k;
        }

        // Enveloppe : log-magnitude interpolée entre les maxima harmoniques (maxima sur ± 0,4 F0), puis enveloppe
        // vraie. Partir des pics évite les vallées profondes entre harmoniques, que le cepstre ne sait pas suivre.
        {
            const double f0Bins = (c.f0 > 0.0 ? c.f0 : 200.0) * n / sampleRate;
            const int    w      = std::max (2, (int) (0.4 * f0Bins));
            int numEnvPeaks = 0;
            for (int k = 1; k < bins - 1; ++k)
            {
                const double m = mag[(size_t) k];
                if (m <= floorMag) continue;
                bool isMax = true;
                for (int j = std::max (0, k - w); j <= std::min (bins - 1, k + w) && isMax; ++j)
                    if (mag[(size_t) j] > m || (j < k && mag[(size_t) j] >= m)) isMax = false;
                if (isMax) envPeaks[(size_t) numEnvPeaks++] = k;
            }
            if (numEnvPeaks == 0)
            {
                for (int k = 0; k < bins; ++k) logMag[(size_t) k] = std::log (mag[(size_t) k] + 1.0e-30);
            }
            else
            {
                auto lm = [&] (int k) { return std::log (mag[(size_t) k] + 1.0e-30); };
                const int first = envPeaks[0], last = envPeaks[(size_t) numEnvPeaks - 1];
                for (int k = 0; k <= first; ++k)          logMag[(size_t) k] = lm (first);
                for (int k = last; k < bins; ++k)         logMag[(size_t) k] = lm (last);
                for (int p = 0; p + 1 < numEnvPeaks; ++p)
                {
                    const int a = envPeaks[(size_t) p], b = envPeaks[(size_t) p + 1];
                    const double la = lm (a), lb = lm (b);
                    for (int k = a; k <= b; ++k)
                        logMag[(size_t) k] = la + (lb - la) * (double) (k - a) / (double) (b - a);
                }
            }
            envelope.trueEnvelope (logMag.data(), env.data(), envelope.orderFor (c.f0, sampleRate), 4, 0.5);
            SpectralEnvelope::warp (env.data(), envTarget.data(), bins, gammaTarget);
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            std::fill (yr[(size_t) ch].begin(), yr[(size_t) ch].end(), 0.0);
            std::fill (yi[(size_t) ch].begin(), yi[(size_t) ch].end(), 0.0);
        }
        std::fill (curRotR.begin(), curRotR.end(), 1.0);
        std::fill (curRotI.begin(), curRotI.end(), 0.0);

        const double beta = c.ratio;
        const double binToOmega = twoPi / n;
        int regionStart = 0;
        for (int p = 0; p < numPeaks; ++p)
        {
            const int k = peaks[(size_t) p];
            int regionEnd = bins - 1;
            if (p + 1 < numPeaks)
            {
                const int k2 = peaks[(size_t) p + 1];
                regionEnd = k;
                for (int j = k + 1; j < k2; ++j)
                    if (mag[(size_t) j] < mag[(size_t) regionEnd]) regionEnd = j;
                if (regionEnd == k) regionEnd = (k + k2) / 2;
            }

            // Fréquence instantanée du pic (rad/échantillon)
            const double expected = binToOmega * k * hop;
            const double omega = binToOmega * k + princarg (phase[(size_t) k] - prevPhase[(size_t) k] - expected) / hop;
            const int shift = (int) std::lround (beta * omega / binToOmega) - k;

            // Z = Z_précédent(région qui contenait k) · e^{jR(β−1)ω}
            double zr = 1.0, zi = 0.0;
            if (! transient)
            {
                const double a = (double) hop * (beta - 1.0) * omega;
                const double cr = std::cos (a), ci = std::sin (a);
                const double pr = prevRotR[(size_t) k], pi = prevRotI[(size_t) k];
                zr = pr * cr - pi * ci;
                zi = pr * ci + pi * cr;
                const double norm = 1.0 / std::max (1.0e-12, std::sqrt (zr * zr + zi * zi));
                zr *= norm; zi *= norm;
            }

            for (int j = regionStart; j <= regionEnd; ++j)
            {
                curRotR[(size_t) j] = zr;
                curRotI[(size_t) j] = zi;
                const int d = j + shift;
                if (d < 0 || d >= bins) continue;
                const double g = std::exp (std::clamp (envTarget[(size_t) d] - env[(size_t) j], -9.2, 4.6));
                const double gr = zr * g, gi = zi * g;
                for (int ch = 0; ch < channels; ++ch)
                {
                    const double ar = xr[(size_t) ch][(size_t) j], ai = xi[(size_t) ch][(size_t) j];
                    yr[(size_t) ch][(size_t) d] += ar * gr - ai * gi;
                    yi[(size_t) ch][(size_t) d] += ar * gi + ai * gr;
                }
            }
            regionStart = regionEnd + 1;
        }

        // Synthèse
        for (int ch = 0; ch < channels; ++ch)
        {
            yi[(size_t) ch][0] = 0.0;
            yi[(size_t) ch][(size_t) bins - 1] = 0.0;
            fft.inverse (yr[(size_t) ch].data(), yi[(size_t) ch].data(), frame.data());
            auto& a = acc[(size_t) ch];
            for (int i = 0; i < n; ++i)
                a[(size_t) i] += frame[(size_t) i] * window[(size_t) i] * olaScale;
        }

        std::swap (prevRotR, curRotR);
        std::swap (prevRotI, curRotI);
        std::copy (mag.begin(), mag.end(), prevMag.begin());
        std::copy (phase.begin(), phase.end(), prevPhase.begin());
    }

    double sampleRate = 48000.0;
    int channels = 1, n = 4096, hop = 512, bins = 2049;
    RealFFT fft;
    SpectralEnvelope envelope;
    std::vector<double> window, frame;
    std::array<detail::MirrorRing, kMaxChannels> inRing;
    std::array<std::vector<double>, kMaxChannels> acc, outQueue, xr, xi, yr, yi;
    std::vector<double> mag, phase, prevMag, prevPhase, logMag, env, envTarget;
    std::vector<double> prevRotR, prevRotI, curRotR, curRotI;
    std::vector<int> peaks, envPeaks;
    int hopCounter = 0, queuePos = 0;
    double prevEnergy = 0.0;
    bool transientReset = true;
    std::int64_t transientCount = 0, framesProcessed = 0;
};

//==============================================================================================================
/** Vocodeur dans la bande vocale + découpe multicadence « delta ». process() rend Δ : la sortie Wet est
    x(n − getLatencySamples()) + Δ(n).

    Deux résolutions, même latence : FFT 2048 (43 ms, suit mieux vibrato et attaques : Soprano, Alto/Tenor) et
    FFT 4096 (85 ms, sépare les harmoniques jusqu'à 50 Hz : Low Male, Instrument). La sortie du cœur 2048 est
    retardée de 2048 échantillons pour que la latence annoncée ne dépende pas de l'InputType. Les deux cœurs
    reçoivent l'entrée en permanence ; au changement, Δ s'éteint en 10 ms, le nouveau cœur remplit sa fenêtre
    (la sortie vaut alors exactement le Dry retardé), puis Δ revient en 10 ms. */
class StudioDxdShifter
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxStages   = 4;     // ÷16 : 705,6 / 768 kHz au plus
    static constexpr int kSplitM      = 32;    // demi-bandes de 127 coefficients
    static constexpr double kSplitBeta = 11.0;

    enum class Resolution : int { Fast = 0, Fine = 1 };

    void prepare (double fs, int numChannels)
    {
        sampleRate = fs;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        stages = 0;
        while (stages < kMaxStages && fs / (double) (2 << stages) >= 44000.0)
            ++stages;
        factor = 1 << stages;
        vocoderRate = fs / factor;

        // FFT fine ≈ 85 ms à fsV : 4096 à 44,1 / 48 kHz ; FFT rapide : moitié.
        int order = 8;
        while ((double) (1 << (order + 1)) <= 0.0853 * vocoderRate * 1.41421356 && order < 15) ++order;
        cores[1].prepare (vocoderRate, channels, order);
        cores[0].prepare (vocoderRate, channels, order - 1);
        const int padding = cores[1].getLatencySamples() - cores[0].getLatencySamples();

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            for (int s = 0; s < kMaxStages; ++s)
            {
                decim[(size_t) ch][(size_t) s].prepare (kSplitBeta);
                interp[(size_t) ch][(size_t) s].prepare (kSplitBeta);
            }
            lowDelay[(size_t) ch].allocate (cores[1].getLatencySamples() + 1);
            fastPad[(size_t) ch].allocate (padding + 1);
            emit[(size_t) ch].assign ((size_t) factor, 0.0);
            tmp[(size_t) ch].assign ((size_t) factor, 0.0);
        }
        rampStep = 1.0 / std::max (1.0, 0.010 * vocoderRate);
        latency = calibrateLatency();
        reset();
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            for (int s = 0; s < kMaxStages; ++s)
            {
                decim[(size_t) ch][(size_t) s].reset();
                interp[(size_t) ch][(size_t) s].reset();
            }
            lowDelay[(size_t) ch].reset();
            fastPad[(size_t) ch].reset();
            std::fill (emit[(size_t) ch].begin(), emit[(size_t) ch].end(), 0.0);
        }
        for (auto& c : cores) c.reset();
        active = pending;
        deltaGain = 1.0;
        holdCounter = 0;
        emitPos = factor;
        counter = 0;
    }

    /** Choix de la résolution (thread audio). Effectif après un fondu de Δ, sans changer la latence. */
    void setResolution (Resolution r) noexcept { pending = (int) r; }
    Resolution getActiveResolution() const noexcept { return (Resolution) active; }

    int    getLatencySamples() const noexcept   { return latency; }
    int    getDecimationFactor() const noexcept { return factor; }
    double getVocoderRate() const noexcept      { return vocoderRate; }
    int    getFftSize() const noexcept          { return cores[(size_t) active].getFftSize(); }
    PhaseVocoderCore& getCore() noexcept        { return cores[(size_t) active]; }
    PhaseVocoderCore& getCore (Resolution r) noexcept { return cores[(size_t) r]; }

    /** Pour le banc : remplace la sortie du vocodeur par 0 (Δ = −bande vocale) afin de vérifier l'alignement. */
    void setMuteVocoderForTest (bool m) noexcept { muteVocoder = m; }

    /** x : échantillons d'entrée (déjà retardés par le moteur si besoin), delta : sortie Δ.
        getControl (std::int64_t centre, std::int64_t span) -> FrameControl : centre de la trame et demi-largeur
        utile (¼ de fenêtre) sur l'horloge des échantillons passés à process(). */
    template <typename GetControl>
    void process (const double* const* x, double* const* delta, int numSamples, GetControl&& getControl) noexcept
    {
        const std::int64_t frameCentreOffset = (std::int64_t) (latency / 2);
        for (int i = 0; i < numSamples; ++i)
        {
            std::array<double, kMaxChannels> low {};
            bool reached = true;
            for (int ch = 0; ch < channels; ++ch)
            {
                double v = x[ch][i];
                for (int s = 0; s < stages; ++s)
                {
                    double y;
                    if (! decim[(size_t) ch][(size_t) s].push (v, y)) { reached = false; break; }
                    v = y;
                }
                low[(size_t) ch] = v;
            }

            if (reached)
            {
                updateResolution();

                std::array<double, kMaxChannels> voc {};
                auto& core = cores[(size_t) active];
                auto& idle = cores[(size_t) (1 - active)];
                const std::int64_t span = (std::int64_t) (core.getFftSize() / 4) * factor;
                // Le centre d'une trame du cœur rapide est en avance de la moitié de l'écart de latence (rattrapé
                // par fastPad) : on corrige l'instant demandé pour que la commande reste calée sur le contenu.
                const std::int64_t lead = active == 0 ? (std::int64_t) (cores[1].getLatencySamples() - cores[0].getLatencySamples()) * factor / 2 : 0;
                const std::int64_t centre = counter - frameCentreOffset + lead;
                core.push (low.data(), voc.data(), [&] { return getControl (centre, span); });
                idle.feed (low.data());

                for (int ch = 0; ch < channels; ++ch)
                {
                    double v = voc[(size_t) ch];
                    if (active == 0)
                    {
                        auto& pad = fastPad[(size_t) ch];
                        pad.push (v);
                        v = pad.last (pad.capacity())[0];
                    }
                    auto& dl = lowDelay[(size_t) ch];
                    dl.push (low[(size_t) ch]);
                    const double delayed = dl.last (dl.capacity())[0];          // d retardé comme le vocodeur
                    const double d = muteVocoder ? -delayed : deltaGain * (v - delayed);
                    upsample ((size_t) ch, d);
                }
                emitPos = 0;
            }

            for (int ch = 0; ch < channels; ++ch)
                delta[ch][i] = emitPos < factor ? emit[(size_t) ch][(size_t) emitPos] : 0.0;
            if (emitPos < factor) ++emitPos;
            ++counter;
        }
    }

private:
    void updateResolution() noexcept
    {
        if (pending != active)
        {
            deltaGain = std::max (0.0, deltaGain - rampStep);
            if (deltaGain <= 0.0)
            {
                active = pending;
                cores[(size_t) active].reset();                  // repart d'une fenêtre vide…
                holdCounter = cores[1].getLatencySamples() + 1;  // …que l'on laisse se remplir, Δ = 0
                for (auto& pad : fastPad) pad.reset();
            }
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
        }
        else
        {
            deltaGain = std::min (1.0, deltaGain + rampStep);
        }
    }

    void upsample (size_t ch, double d) noexcept
    {
        auto& e = emit[ch];
        auto& t = tmp[ch];
        e[0] = d;
        int count = 1;
        for (int s = stages - 1; s >= 0; --s)
        {
            for (int j = 0; j < count; ++j)
                interp[ch][(size_t) s].push (e[(size_t) j], t[(size_t) (2 * j)], t[(size_t) (2 * j + 1)]);
            count *= 2;
            std::copy (t.begin(), t.begin() + count, e.begin());
        }
    }

    /** Retard exact de la chaîne ÷ -> retard N − 1 -> × (entier, à fs), mesuré sur un signal à bande limitée. */
    int calibrateLatency()
    {
        const int nominal = cores[1].getLatencySamples() * factor;
        if (stages == 0)
            return nominal;

        std::array<detail::HalfBandDecimatorT<kSplitM>, kMaxStages> dc;
        std::array<detail::HalfBandInterpolatorT<kSplitM>, kMaxStages> ic;
        for (int s = 0; s < stages; ++s) { dc[(size_t) s].prepare (kSplitBeta); ic[(size_t) s].prepare (kSplitBeta); }
        detail::MirrorRing dl;
        dl.allocate (cores[1].getLatencySamples() + 1);

        const int length = nominal + 64 * factor + 8192;
        std::vector<double> in ((size_t) length), out ((size_t) length, 0.0), e ((size_t) factor), t ((size_t) factor);
        for (int i = 0; i < length; ++i)
        {
            const double tt = (double) i / sampleRate;
            in[(size_t) i] = std::sin (2 * 3.14159265358979 * 997.0 * tt) + 0.5 * std::sin (2 * 3.14159265358979 * 3163.0 * tt + 1.0)
                           + 0.25 * std::sin (2 * 3.14159265358979 * 8111.0 * tt + 2.0);
        }
        int pos = factor;
        for (int i = 0; i < length; ++i)
        {
            double v = in[(size_t) i];
            bool reached = true;
            for (int s = 0; s < stages; ++s)
            {
                double y;
                if (! dc[(size_t) s].push (v, y)) { reached = false; break; }
                v = y;
            }
            if (reached)
            {
                dl.push (v);
                e[0] = dl.last (dl.capacity())[0];
                int count = 1;
                for (int s = stages - 1; s >= 0; --s)
                {
                    for (int j = 0; j < count; ++j)
                        ic[(size_t) s].push (e[(size_t) j], t[(size_t) (2 * j)], t[(size_t) (2 * j + 1)]);
                    count *= 2;
                    std::copy (t.begin(), t.begin() + count, e.begin());
                }
                pos = 0;
            }
            out[(size_t) i] = pos < factor ? e[(size_t) pos] : 0.0;
            if (pos < factor) ++pos;
        }

        int best = nominal;
        double bestErr = 1.0e300;
        const int expected = nominal + 2 * (2 * kSplitM - 1) * (factor - 1);   // retards de groupe ÷ et ×
        for (int lag = expected - 4 * factor; lag <= expected + 4 * factor; ++lag)
        {
            double err = 0.0;
            for (int i = length - 4096; i < length; ++i)
            {
                const double d = out[(size_t) i] - in[(size_t) (i - lag)];
                err += d * d;
            }
            if (err < bestErr) { bestErr = err; best = lag; }
        }
        return best;
    }

    double sampleRate = 48000.0, vocoderRate = 48000.0;
    int channels = 1, stages = 0, factor = 1, latency = 0, emitPos = 1;
    int active = 0, pending = 0, holdCounter = 0;
    double deltaGain = 1.0, rampStep = 0.001;
    std::int64_t counter = 0;
    bool muteVocoder = false;
    std::array<PhaseVocoderCore, 2> cores;
    std::array<std::array<detail::HalfBandDecimatorT<kSplitM>, kMaxStages>, kMaxChannels> decim;
    std::array<std::array<detail::HalfBandInterpolatorT<kSplitM>, kMaxStages>, kMaxChannels> interp;
    std::array<detail::MirrorRing, kMaxChannels> lowDelay, fastPad;
    std::array<std::vector<double>, kMaxChannels> emit, tmp;
};

} // namespace aura::pitch

// AuraPitch — NextDSP · AuraSuite
// StudioFormantFilter.h : moteur du mode Studio/DXD (sans JUCE), 1 ou 2 canaux, 44,1 … 384 kHz.
//
// Chaîne, dans l'architecture « delta » multicadence de l'étape 2 (inchangée) :
//
//     x ─► ÷2 … ÷2 ─► d (fsV = 44,1 / 48 kHz) ─► SpliceShifter ─► filtre de formants ─► v ─┐
//                       └──────────► retard (D + latence du filtre) ────────────────────────┴─(−)─► δ ─► ×2 … ×2 ─► Δ
//     sortie Wet = x retardé de L + Δ
//
//   * Hauteur : SpliceShifter (une tête de lecture, sinc 48 points, raccords synchrones de la période calés par
//     corrélation). Plus de vocodeur de phase : plus de phasing, plus d'étalement des consonnes, plus de « deux voix »
//     dans les trames qui chevauchaient deux notes (fenêtres de 43 / 85 ms de l'étape 2).
//   * Formants : filtre DIFFÉRENTIEL d'amplitude seule. Le rééchantillonnage a déplacé l'enveloppe : E_s(f) =
//     E(f / r). Le gain G(f) = E_s(f · r / γ) / E_s(f) (FormantPreserve, γ = 2^(FormantShift/12)) ou
//     E_s(f / γ) / E_s(f) (sans Preserve) est lisse, réel et proche de 1 : les phases, le grain du micro, le souffle et
//     l'air de la voix ne sont pas resynthétisés, seulement égalisés. Fenêtre de Hann de 1024 points (21 ms à 48 kHz),
//     FFT de 2048 avec zéros (aucun repliement temporel du filtre), saut de 256.
//   * Δ = v − d retardé : quand rien n'est corrigé (non voisé, ou voix juste), la tête est verrouillée, le filtre
//     recopie la trame, Δ = 0 et la sortie est exactement le Dry retardé. La bande vocale corrigée remplace 100 % de la
//     bande vocale d'origine (annulation à l'échantillon près, résidu < −130 dB) ; l'air du DXD au-delà de ~20 kHz
//     traverse en retard pur.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "FFT.h"
#include "FormantProcessor.h"
#include "HalfBand.h"
#include "PSOLAShifter.h"
#include "SpliceShifter.h"

namespace aura::pitch
{

//==============================================================================================================
/** Commande d'une trame (ou d'un instant) du moteur Studio. */
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
/** Filtre de formants différentiel (amplitude seule), multicanal, à fsV. */
class FormantFilterCore
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double fs, int numChannels, int frameOrder)
    {
        sampleRate = fs;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        n = 1 << frameOrder;            // fenêtre
        m = 2 * n;                      // FFT avec zéros
        hop = n / 4;
        bins = m / 2 + 1;
        fft.prepare (frameOrder + 1);
        envelope.prepare (frameOrder + 1);
        window.resize ((size_t) n);
        for (int i = 0; i < n; ++i) window[(size_t) i] = 0.5 - 0.5 * std::cos (6.28318530717958647692 * i / n);
        int size = 1;
        while (size < 2 * m + n) size <<= 1;
        accMask = size - 1;
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            inRing[(size_t) ch].allocate (n);
            acc[(size_t) ch].assign ((size_t) size, 0.0);
            xr[(size_t) ch].assign ((size_t) bins, 0.0);
            xi[(size_t) ch].assign ((size_t) bins, 0.0);
        }
        frame.assign ((size_t) m, 0.0);
        mag.assign ((size_t) bins, 0.0);
        logMag.assign ((size_t) bins, 0.0);
        env.assign ((size_t) bins, 0.0);
        target.assign ((size_t) bins, 0.0);
        reset();
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            inRing[(size_t) ch].reset();
            std::fill (acc[(size_t) ch].begin(), acc[(size_t) ch].end(), 0.0);
        }
        counter = 0;
        hopCounter = 0;
        filteredFrames = 0;
    }

    /** Latence : fenêtre + quart de FFT (les queues du filtre à phase nulle débordent de ±m/4). */
    int getLatencySamples() const noexcept { return n - 1 + m / 4; }
    int getWindowSize() const noexcept { return n; }
    std::int64_t getFilteredFrames() const noexcept { return filteredFrames; }

    /** gammaFor() -> facteur de lecture de l'enveloppe (1 = neutre), appelé à chaque trame ; f0 pour l'ordre. */
    template <typename GetGamma>
    void push (const double* in, double* out, GetGamma&& gammaFor) noexcept
    {
        for (int ch = 0; ch < channels; ++ch)
            inRing[(size_t) ch].push (in[ch]);

        if (++hopCounter >= hop)
        {
            hopCounter = 0;
            double f0 = 0.0;
            const double gamma = gammaFor (f0);
            processFrame (gamma, f0);
        }

        const std::int64_t p = counter - getLatencySamples();
        for (int ch = 0; ch < channels; ++ch)
        {
            auto& a = acc[(size_t) ch];
            const size_t s = (size_t) (p & accMask);
            out[ch] = a[s];
            a[s] = 0.0;
        }
        ++counter;
    }

private:
    void processFrame (double gamma, double f0) noexcept
    {
        // La trame couvre counter − n + 1 … counter ; elle est posée au quart de la FFT, sa sortie commence m/4 avant.
        const std::int64_t start = counter - n + 1 - m / 4;
        const bool neutral = std::abs (gamma - 1.0) < 1.0e-9;

        if (neutral)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const double* src = inRing[(size_t) ch].last (n);
                auto& a = acc[(size_t) ch];
                for (int i = 0; i < n; ++i)
                    a[(size_t) ((start + m / 4 + i) & accMask)] += 0.5 * window[(size_t) i] * src[i];
            }
            return;
        }

        ++filteredFrames;
        for (int ch = 0; ch < channels; ++ch)
        {
            const double* src = inRing[(size_t) ch].last (n);
            std::fill (frame.begin(), frame.end(), 0.0);
            for (int i = 0; i < n; ++i) frame[(size_t) (m / 4 + i)] = src[i] * window[(size_t) i];
            fft.forward (frame.data(), xr[(size_t) ch].data(), xi[(size_t) ch].data());
        }

        // Enveloppe de la moyenne des canaux (la même égalisation pour tous : image stéréo conservée)
        for (int k = 0; k < bins; ++k)
        {
            double r = 0.0, im = 0.0;
            for (int ch = 0; ch < channels; ++ch) { r += xr[(size_t) ch][(size_t) k]; im += xi[(size_t) ch][(size_t) k]; }
            mag[(size_t) k] = std::sqrt (r * r + im * im) / channels;
            logMag[(size_t) k] = std::log (mag[(size_t) k] + 1.0e-30);
        }
        const int order = std::min (envelope.orderFor (f0, sampleRate), m / 8);
        envelope.trueEnvelope (logMag.data(), env.data(), order, 4, 1.0);
        SpectralEnvelope::warp (env.data(), target.data(), bins, gamma);

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int k = 0; k < bins; ++k)
            {
                const double g = std::exp (std::clamp (target[(size_t) k] - env[(size_t) k], -2.3, 2.3));   // ±20 dB
                xr[(size_t) ch][(size_t) k] *= g;
                xi[(size_t) ch][(size_t) k] *= g;
            }
            fft.inverse (xr[(size_t) ch].data(), xi[(size_t) ch].data(), frame.data());
            auto& a = acc[(size_t) ch];
            for (int i = 0; i < m; ++i)
                a[(size_t) ((start + i) & accMask)] += 0.5 * frame[(size_t) i];
        }
    }

    double sampleRate = 48000.0;
    int channels = 1, n = 1024, m = 2048, hop = 256, bins = 1025, hopCounter = 0;
    std::int64_t counter = 0, accMask = 0, filteredFrames = 0;
    RealFFT fft;
    SpectralEnvelope envelope;
    std::vector<double> window, frame, mag, logMag, env, target;
    std::array<detail::MirrorRing, kMaxChannels> inRing;
    std::array<std::vector<double>, kMaxChannels> acc, xr, xi;
};

//==============================================================================================================
/** Moteur Studio complet. process() rend Δ : Wet = x(n − getLatencySamples()) + Δ(n). */
class StudioDxdShifter
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxStages   = 4;
    static constexpr int kSplitM      = 32;
    static constexpr double kSplitBeta = 11.0;
    static constexpr int kControlStride = 32;           // commande de la tête relue tous les 32 échantillons à fsV

    void prepare (double fs, int numChannels)
    {
        sampleRate = fs;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        stages = 0;
        while (stages < kMaxStages && fs / (double) (2 << stages) >= 44000.0)
            ++stages;
        factor = 1 << stages;
        vocoderRate = fs / factor;

        splicer.prepare (vocoderRate, channels, 0.002);
        psola.prepare (vocoderRate, channels, 0.002);           // même retard nominal : fondu sans décalage
        int order = 6;                                   // fenêtre ≈ 21 ms : 1024 à 44,1 / 48 kHz
        while ((double) (1 << (order + 1)) <= 0.0213 * vocoderRate * 1.41421356 && order < 14) ++order;
        formant.prepare (vocoderRate, channels, order);
        lowLatency = splicer.getLatencySamples() + formant.getLatencySamples();
        blendStep = 1.0 / (0.010 * vocoderRate);

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            for (int s = 0; s < kMaxStages; ++s)
            {
                decim[(size_t) ch][(size_t) s].prepare (kSplitBeta);
                interp[(size_t) ch][(size_t) s].prepare (kSplitBeta);
            }
            lowDelay[(size_t) ch].allocate (lowLatency + 1);
            emit[(size_t) ch].assign ((size_t) factor, 0.0);
            tmp[(size_t) ch].assign ((size_t) factor, 0.0);
            spliced[(size_t) ch] = 0.0;
        }
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
            std::fill (emit[(size_t) ch].begin(), emit[(size_t) ch].end(), 0.0);
        }
        splicer.reset();
        psola.reset();
        psolaBlend = usePsolaTarget ? 1.0 : 0.0;
        formant.reset();
        emitPos = factor;
        counter = 0;
        lowCounter = 0;
        current = FrameControl {};
    }

    int    getLatencySamples() const noexcept   { return latency; }
    int    getDecimationFactor() const noexcept { return factor; }
    double getVocoderRate() const noexcept      { return vocoderRate; }
    int    getFormantWindow() const noexcept    { return formant.getWindowSize(); }
    const SpliceShifter& getSplicer() const noexcept         { return splicer; }
    const FormantFilterCore& getFormantFilter() const noexcept { return formant; }

    /** Effets de formant et grandes transpositions avec Preserve : PSOLA (les grains gardent l'enveloppe) au lieu de la
        tête unique + filtre différentiel. Fondu de 10 ms. */
    void setUsePsola (bool b) noexcept { usePsolaTarget = b; }
    /** Mode Ultra : noyau sinc 96 points et raccords au pas d'un échantillon (latence inchangée). */
    void setUltra (bool u) noexcept { splicer.setUltra (u); psola.setUltra (u); }

    /** Pour le banc : remplace la sortie corrigée par 0 (Δ = −bande vocale) afin de vérifier l'alignement. */
    void setMuteForTest (bool m) noexcept { muteVoice = m; }

    /** x : échantillons d'entrée (déjà retardés par le moteur), delta : sortie Δ.
        getControl (std::int64_t instant, std::int64_t span) -> FrameControl, sur l'horloge des échantillons passés. */
    template <typename GetControl>
    void process (const double* const* x, double* const* delta, int numSamples, GetControl&& getControl) noexcept
    {
        const std::int64_t splitDelay = (std::int64_t) ((latency - lowLatency * factor) / 2);
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
                const std::int64_t lowTime = counter - splitDelay;              // instant (fs) du dernier échantillon d
                // Tête de lecture : commande à l'instant qu'elle lit
                if (lowCounter % kControlStride == 0)
                {
                    // Lot 1 : la commande est lue en avance du lissage du rapport (1 ms) et d'un demi-pas de commande,
                    // et pendant un raccord la seconde tête reçoit le rapport de l'instant qu'elle lit.
                    const std::int64_t lead = (std::int64_t) ((0.001 * vocoderRate + 0.5 * kControlStride) * factor);
                    const std::int64_t readTime = lowTime - (std::int64_t) (splicer.getCurrentOffset() * factor) + lead;
                    current = getControl (readTime, (std::int64_t) 0);
                    if (splicer.isFading() && current.voiced)
                    {
                        const auto c2 = getControl (lowTime - (std::int64_t) (splicer.getFadeOffset() * factor) + lead, (std::int64_t) 0);
                        if (c2.voiced && current.ratio > 0.0) splicer.setFadeRatioScale (c2.ratio / current.ratio);
                    }
                    const double periodLow = current.f0 > 0.0 ? vocoderRate / current.f0 : 0.0;
                    splicer.setPitch (periodLow, current.voiced);
                    splicer.setRatio (current.voiced ? current.ratio : 1.0);
                    psola.setPitch (periodLow, current.voiced);
                    psola.setShift (current.voiced ? current.ratio : 1.0, current.formantFactor, current.preserve);
                }
                ++lowCounter;

                std::array<double, kMaxChannels> s {}, sp {}, v {};
                {
                    const double* in[kMaxChannels] = { &low[0], &low[1] };
                    double* out[kMaxChannels] = { &s[0], &s[1] };
                    double* outP[kMaxChannels] = { &sp[0], &sp[1] };
                    splicer.process (in, out, 1);
                    psola.setActive (usePsolaTarget || psolaBlend > 0.0);
                    psola.process (in, outP, 1);
                }
                psolaBlend += std::clamp ((usePsolaTarget ? 1.0 : 0.0) - psolaBlend, -blendStep, blendStep);
                if (psolaBlend > 0.0)
                    for (int ch = 0; ch < channels; ++ch) s[(size_t) ch] += psolaBlend * (sp[(size_t) ch] - s[(size_t) ch]);
                const std::int64_t frameTime = lowTime - (std::int64_t) ((splicer.getCurrentOffset() + formant.getWindowSize() / 2) * factor);
                const std::int64_t span = (std::int64_t) (formant.getWindowSize() / 4) * factor;
                formant.push (s.data(), v.data(), [&] (double& f0) noexcept
                {
                    const auto c = getControl (frameTime, span);
                    f0 = c.f0;
                    if (! c.voiced || usePsolaTarget) return 1.0;            // PSOLA : formants déjà traités
                    return c.preserve ? c.formantFactor / c.ratio : c.formantFactor;
                });

                for (int ch = 0; ch < channels; ++ch)
                {
                    auto& dl = lowDelay[(size_t) ch];
                    dl.push (low[(size_t) ch]);
                    const double delayed = dl.last (dl.capacity())[0];
                    upsample ((size_t) ch, (muteVoice ? 0.0 : v[(size_t) ch]) - delayed);
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

    /** Retard exact de la chaîne ÷ -> retard bas débit -> × (entier, à fs), mesuré sur un signal à bande limitée. */
    int calibrateLatency()
    {
        const int nominal = lowLatency * factor;
        if (stages == 0)
            return nominal;

        std::array<detail::HalfBandDecimatorT<kSplitM>, kMaxStages> dc;
        std::array<detail::HalfBandInterpolatorT<kSplitM>, kMaxStages> ic;
        for (int s = 0; s < stages; ++s) { dc[(size_t) s].prepare (kSplitBeta); ic[(size_t) s].prepare (kSplitBeta); }
        detail::MirrorRing dl;
        dl.allocate (lowLatency + 1);

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
        const int expected = nominal + 2 * (2 * kSplitM - 1) * (factor - 1);
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
    int channels = 1, stages = 0, factor = 1, latency = 0, lowLatency = 0, emitPos = 1;
    std::int64_t counter = 0, lowCounter = 0;
    bool muteVoice = false;
    FrameControl current;
    SpliceShifter splicer;
    PSOLAShifter psola;
    bool usePsolaTarget = false;
    double psolaBlend = 0.0, blendStep = 0.002;
    FormantFilterCore formant;
    std::array<std::array<detail::HalfBandDecimatorT<kSplitM>, kMaxStages>, kMaxChannels> decim;
    std::array<std::array<detail::HalfBandInterpolatorT<kSplitM>, kMaxStages>, kMaxChannels> interp;
    std::array<detail::MirrorRing, kMaxChannels> lowDelay;
    std::array<std::vector<double>, kMaxChannels> emit, tmp;
    std::array<double, kMaxChannels> spliced {};
};

} // namespace aura::pitch

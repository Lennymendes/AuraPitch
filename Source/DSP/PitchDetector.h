// AuraPitch — NextDSP · AuraSuite
// PitchDetector.h : détecteur de F0 temps réel, 50 … 1200 Hz, de 8 kHz au DXD 384 kHz.
//
// Chaîne
//   entrée (fs, float ou double) ─► cascade de demi-bandes ÷2 (FIR 47 coefficients, Kaiser β 9, ≈ 90 dB)
//        ├─ niveau « raffinement » fsR = 4 × fsA (44,1 / 48 kHz quand fs ≥ 44,1 kHz)
//        └─ niveau « analyse »     fsA ∈ [8 ; 16[ kHz (11 025 ou 12 000 Hz pour toutes les fréquences usuelles)
//
//   1. MPM (McLeod & Wyvill 2005) sur fsA : NSDF n(τ) = 2 r(τ) / m(τ), maxima clés, seuil k × max, interpolation
//      parabolique. Coût indépendant de fs : la fenêtre (2 périodes de la F0 la plus grave) compte les mêmes
//      échantillons à 44,1 kHz et à 384 kHz.
//   2. Raffinement sur fsR : NSDF recalculée sur ±4 retards autour de la période trouvée (fenêtre de même durée),
//      interpolation parabolique à 4× la résolution temporelle. À 1 kHz, la période passe de 11 à 44 échantillons :
//      le biais de l'interpolation parabolique tombe de quelques cents à moins de 0,1 cent.
//   3. Voisement : niveau RMS (gate), clarté NSDF (avec hystérésis), et « fréquence équivalente » des aigus
//      fEq = fsR / 2π · acos(ρ1) (ρ1 = autocorrélation normalisée au retard 1, soit la fréquence RMS du spectre).
//      Une sifflante (s, ch, f), une plosive sourde (t, k, p) ou un souffle ont fEq de 3 à 10 kHz, une voyelle
//      quelques centaines de Hz : la trame est déclarée non voisée et ne sera pas pitchée.
//
// Temps réel : prepare() alloue (fenêtre maximale, 50 Hz) ; process(), setRange(), setInputType(), setHopSeconds()
// et setConfig() n'allouent rien et ne prennent aucun verrou. Calculs internes en double.
// Aucun en-tête JUCE (banc : Tests/PitchDspSelfTest.cpp).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "HalfBand.h"

namespace aura::pitch
{

enum class InputType : int { Soprano = 0, AltoTenor, LowMale, Instrument };

struct F0Range { double minHz, maxHz; };

constexpr double kDetectorMinHz = 50.0;
constexpr double kDetectorMaxHz = 1200.0;

/** Bornes de recherche de F0 de chaque InputType (avec la marge d'une tierce sous la note la plus grave courante). */
constexpr F0Range f0RangeFor (InputType type) noexcept
{
    switch (type)
    {
        case InputType::Soprano:    return { 150.0, 1200.0 };
        case InputType::AltoTenor:  return {  75.0,  900.0 };
        case InputType::LowMale:    return {  50.0,  450.0 };
        case InputType::Instrument: break;
    }
    return { kDetectorMinHz, kDetectorMaxHz };
}

struct PitchEstimate
{
    double       frequencyHz    = 0.0;    // F0 estimée (même non voisée, 0 si aucun maximum) — n'utiliser que si voiced
    double       periodSamples  = 0.0;    // période en échantillons de l'entrée (fs) : marques PSOLA
    float        clarity        = 0.0f;   // pic NSDF 0 … 1 (périodicité)
    float        levelDb        = -200.0f;// RMS de la trame, dBFS
    float        brightnessHz   = 0.0f;   // fréquence équivalente des aigus (détection des sifflantes)
    bool         voiced         = false;
    std::int64_t frameCentre    = 0;      // instant (échantillon d'entrée, horloge absolue) que décrit l'estimation
};

namespace detail
{
    /** Passe-bas Butterworth du 4e ordre (2 cellules biquad, transposée directe II, double). Limite le signal de
        raffinement à la bande de l'analyse : le bruit de souffle au-dessus de 4 kHz, où la voix n'a presque plus
        d'harmoniques, ne vient pas perturber la position du pic. Phase sans importance : l'autocorrélation est
        insensible au retard de groupe. */
    class Butterworth4
    {
    public:
        void design (double cutoffHz, double fs) noexcept
        {
            constexpr double pi = 3.14159265358979323846;
            const double w0 = 2.0 * pi * std::min (cutoffHz, 0.45 * fs) / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double qs[2] = { 0.54119610014619698, 1.30656296487637653 };   // Q des 2 paires de pôles
            for (int s = 0; s < 2; ++s)
            {
                const double alpha = sw / (2.0 * qs[s]);
                const double a0 = 1.0 + alpha;
                auto& c = cells[(size_t) s];
                c.b0 = (1.0 - cw) * 0.5 / a0;
                c.b1 = (1.0 - cw) / a0;
                c.b2 = c.b0;
                c.a1 = -2.0 * cw / a0;
                c.a2 = (1.0 - alpha) / a0;
            }
            reset();
        }
        void reset() noexcept { for (auto& c : cells) c.z1 = c.z2 = 0.0; }

        double process (double x) noexcept
        {
            x += 1.0e-20;                              // pas de nombres dénormalisés sur le silence (retiré avec la moyenne)
            for (auto& c : cells)
            {
                const double y = c.b0 * x + c.z1;
                c.z1 = c.b1 * x - c.a1 * y + c.z2;
                c.z2 = c.b2 * x - c.a2 * y;
                x = y;
            }
            return x;
        }

    private:
        struct Cell { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
        std::array<Cell, 2> cells {};
    };
} // namespace detail

//==============================================================================================================
class PitchDetector
{
public:
    struct Config
    {
        double mpmThreshold     = 0.93;   // k du MPM : premier maximum clé ≥ k × le plus haut (anti-octave basse)
        double clarityOn        = 0.80;   // clarté pour entrer en voisé
        double clarityOff       = 0.70;   // clarté pour en sortir (hystérésis)
        double strongClarity    = 0.95;   // au-delà, la trame est voisée même si elle est brillante (synthé, cuivre)
        double gateDb           = -60.0;  // sous ce niveau RMS : silence, non voisé
        double sibilanceHz      = 3000.0; // fréquence équivalente au-delà de laquelle une trame est une sifflante
    };

    static constexpr int    kMaxStages        = 7;
    static constexpr double kMinAnalysisRate  = 8000.0;
    static constexpr int    kRefineFactorLog2 = 2;        // fsR = 4 × fsA

    //--- préparation (message thread / prepareToPlay) -------------------------------------------------------------
    void prepare (double newSampleRate)
    {
        sampleRate = std::max (1000.0, newSampleRate);

        numStages = 0;
        while (numStages < kMaxStages && sampleRate / (double) (2 << numStages) >= kMinAnalysisRate)
            ++numStages;

        refineLog2   = std::min (kRefineFactorLog2, numStages);
        refineLevel  = numStages - refineLog2;
        analysisRate = sampleRate / (double) (1 << numStages);
        refineRate   = analysisRate * (double) (1 << refineLog2);

        for (auto& s : stages)
            s.prepare();

        // Dimensionnement pour la F0 la plus grave possible : setRange() ne réalloue jamais.
        const int maxPeriodA = (int) std::ceil (analysisRate / kDetectorMinHz);
        capWindowA = 2 * maxPeriodA + 4;
        capLagA    = maxPeriodA + 2;
        capWindowR = capWindowA << refineLog2;

        analysisRing.allocate (capWindowA);
        refineRing.allocate (capWindowR);
        refineLpRing.allocate (capWindowR);
        refineLowpass.design (0.35 * analysisRate, refineRate);
        frameA.assign ((size_t) capWindowA, 0.0);
        frameR.assign ((size_t) capWindowR, 0.0);
        prefixR.assign ((size_t) capWindowR + 1, 0.0);
        nsdf.assign ((size_t) capLagA + 2, 0.0);
        keyMaxima.assign ((size_t) capLagA + 2, 0);

        setRange (range);
        setHopSeconds (hopSeconds);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages)
            s.reset();
        analysisRing.reset();
        refineRing.reset();
        refineLpRing.reset();
        refineLowpass.reset();
        hopCounter   = 0;
        inputCounter = 0;
        latest       = PitchEstimate {};
    }

    //--- réglages temps réel (aucune allocation) -----------------------------------------------------------------
    void setInputType (InputType type) noexcept { setRange (f0RangeFor (type)); }

    void setRange (F0Range newRange) noexcept
    {
        range.minHz = std::clamp (newRange.minHz, kDetectorMinHz, kDetectorMaxHz * 0.5);
        range.maxHz = std::clamp (newRange.maxHz, range.minHz * 2.0, kDetectorMaxHz);
        if (capWindowA == 0)
            return;                                              // appliqué au prochain prepare()

        const int periodA = (int) std::ceil (analysisRate / range.minHz);
        lagMaxA  = std::min (capLagA, periodA + 2);
        windowA  = std::min (capWindowA, 2 * periodA + 4);
        lagMinA  = std::max (2, (int) std::floor (analysisRate / range.maxHz) - 1);
        windowR  = windowA << refineLog2;
    }

    /** Intervalle entre deux estimations, 0,5 … 10 ms (défaut 1,5 ms). */
    void setHopSeconds (double seconds) noexcept
    {
        hopSeconds = std::clamp (seconds, 0.0005, 0.010);
        if (analysisRate > 0.0)
            hopA = std::max (1, (int) std::lround (hopSeconds * analysisRate));
    }

    void setConfig (const Config& c) noexcept { config = c; }

    //--- traitement (thread audio) -------------------------------------------------------------------------------
    /** Traite un bloc mono. onEstimate (const PitchEstimate&, int sampleIndexInBlock) est appelé à chaque nouvelle
        estimation, avec l'indice de l'échantillon d'entrée qui l'a déclenchée. */
    template <typename SampleType, typename Callback>
    void process (const SampleType* input, int numSamples, Callback&& onEstimate) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            double v = (double) input[i];
            if (! std::isfinite (v))
                v = 0.0;
            ++inputCounter;

            if (refineLevel == 0)
                pushRefine (v);

            bool reachedAnalysis = true;
            for (int k = 0; k < numStages; ++k)
            {
                double y;
                if (! stages[(size_t) k].push (v, y)) { reachedAnalysis = false; break; }
                v = y;
                if (k + 1 == refineLevel)
                    pushRefine (v);
            }

            if (! reachedAnalysis)
                continue;

            analysisRing.push (v);
            if (++hopCounter >= hopA)
            {
                hopCounter = 0;
                if (analysisRing.size() >= windowA && refineRing.size() >= windowR)
                {
                    analyse();
                    onEstimate (static_cast<const PitchEstimate&> (latest), i);
                }
            }
        }
    }

    template <typename SampleType>
    void process (const SampleType* input, int numSamples) noexcept
    {
        process (input, numSamples, [] (const PitchEstimate&, int) noexcept {});
    }

    //--- informations ---------------------------------------------------------------------------------------------
    const PitchEstimate& getLatest() const noexcept { return latest; }
    double  getSampleRate() const noexcept          { return sampleRate; }
    double  getAnalysisRate() const noexcept        { return analysisRate; }
    double  getRefineRate() const noexcept          { return refineRate; }
    int     getDecimationFactor() const noexcept    { return 1 << numStages; }
    int     getHopInputSamples() const noexcept     { return hopA << numStages; }
    F0Range getRange() const noexcept               { return range; }

    /** Retard entre le dernier échantillon reçu et l'instant décrit par une estimation (frameCentre), en
        échantillons d'entrée : retard de groupe des demi-bandes + demi-fenêtre. Le mode Studio retarde l'audio
        d'autant pour que la correction tombe exactement sur la note ; le mode Live l'accepte (comme un accordeur). */
    double getAnalysisLatencySamples() const noexcept
    {
        const double scale = (double) (1 << refineLevel);
        return (double) HalfBandDecimator::kGroupDelay * (scale - 1.0) + 0.5 * (double) (windowR - 1) * scale;
    }

private:
    using HalfBandDecimator = detail::HalfBandDecimator;

    void pushRefine (double v) noexcept
    {
        refineRing.push (v);
        refineLpRing.push (refineLog2 > 0 ? refineLowpass.process (v) : v);
    }

    static double meanRemovedCopy (const double* src, double* dst, int n) noexcept
    {
        double mean = 0.0;
        for (int j = 0; j < n; ++j) mean += src[j];
        mean /= (double) n;
        double energy = 0.0;
        for (int j = 0; j < n; ++j) { dst[j] = src[j] - mean; energy += dst[j] * dst[j]; }
        return energy;
    }

    /** Σ x[j]·x[j+lag]. Quatre sommes partielles indépendantes : le compilateur vectorise (SSE2/AVX/NEON) sans
        -ffast-math, et le résultat reste déterministe d'un hôte à l'autre. */
    static double crossCorrelation (const double* x, int n, int lag) noexcept
    {
        const double* y = x + lag;
        const int len = n - lag;
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        int j = 0;
        for (; j + 4 <= len; j += 4)
        {
            s0 += x[j]     * y[j];
            s1 += x[j + 1] * y[j + 1];
            s2 += x[j + 2] * y[j + 2];
            s3 += x[j + 3] * y[j + 3];
        }
        for (; j < len; ++j)
            s0 += x[j] * y[j];
        return (s0 + s1) + (s2 + s3);
    }

    /** Interpolation parabolique autour de l'indice t ; renvoie le décalage (−0,5 … 0,5) et la valeur du sommet. */
    static double parabolicOffset (double a, double b, double c, double& peak) noexcept
    {
        const double den = a - 2.0 * b + c;
        if (den >= 0.0) { peak = b; return 0.0; }
        const double d = std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        peak = b - 0.25 * (a - c) * d;
        return d;
    }

    void analyse() noexcept
    {
        PitchEstimate est;
        const int W = windowA;
        double* x = frameA.data();

        //--- trame d'analyse, niveau ------------------------------------------------------------------------------
        const double energy = meanRemovedCopy (analysisRing.last (W), x, W);
        const double rms    = std::sqrt (energy / (double) W);
        est.levelDb = (float) (20.0 * std::log10 (rms + 1.0e-30));

        const int WR = windowR;
        est.frameCentre = inputCounter - 1 - (std::int64_t) std::llround (getAnalysisLatencySamples());

        if (energy <= 1.0e-24)
        {
            finish (est, 0.0, false);
            return;
        }

        //--- NSDF : r(τ) direct, m(τ) incrémental ---------------------------------------------------------------
        double m = 2.0 * energy;
        for (int tau = 0; tau <= lagMaxA; ++tau)
        {
            const double r = crossCorrelation (x, W, tau);
            nsdf[(size_t) tau] = m > 1.0e-30 ? 2.0 * r / m : 0.0;
            m -= x[W - 1 - tau] * x[W - 1 - tau] + x[tau] * x[tau];
        }

        //--- maxima clés (MPM) ----------------------------------------------------------------------------------
        int tau = 1;
        while (tau < lagMaxA && nsdf[(size_t) tau] > 0.0)   // lobe du retard 0
            ++tau;

        int numKeys = 0, regionBest = -1;
        auto closeRegion = [&]
        {
            if (regionBest >= lagMinA)
                keyMaxima[(size_t) numKeys++] = regionBest;
            regionBest = -1;
        };
        for (; tau < lagMaxA; ++tau)
        {
            const double v = nsdf[(size_t) tau];
            if (v > 0.0)
            {
                if (regionBest < 0 || v > nsdf[(size_t) regionBest])
                    regionBest = tau;
            }
            else if (regionBest >= 0)
            {
                closeRegion();
            }
        }
        if (regionBest >= 0)
            closeRegion();

        if (numKeys == 0)
        {
            finish (est, 0.0, false);
            return;
        }

        double highest = 0.0;
        for (int k = 0; k < numKeys; ++k)
            highest = std::max (highest, nsdf[(size_t) keyMaxima[(size_t) k]]);

        int chosen = keyMaxima[0];
        for (int k = 0; k < numKeys; ++k)
            if (nsdf[(size_t) keyMaxima[(size_t) k]] >= config.mpmThreshold * highest) { chosen = keyMaxima[(size_t) k]; break; }

        double clarity = 0.0;
        const double offset = parabolicOffset (nsdf[(size_t) chosen - 1], nsdf[(size_t) chosen], nsdf[(size_t) chosen + 1], clarity);
        double periodA = (double) chosen + offset;

        //--- brillance (bande complète du niveau de raffinement) -----------------------------------------------
        constexpr double twoPi = 6.28318530717958647692;
        double* y = frameR.data();
        double brightness = 0.0;
        {
            const double e = meanRemovedCopy (refineRing.last (WR), y, WR);
            const double m1 = 2.0 * e - y[0] * y[0] - y[WR - 1] * y[WR - 1];
            if (m1 > 1.0e-30)
                brightness = refineRate / twoPi * std::acos (std::clamp (2.0 * crossCorrelation (y, WR, 1) / m1, -1.0, 1.0));
        }

        //--- raffinement à 4 × fsA (signal limité à la bande d'analyse) ---------------------------------------
        if (refineLog2 > 0 && meanRemovedCopy (refineLpRing.last (WR), y, WR) > 1.0e-24)
        {
            double* P = prefixR.data();                  // P[k] = Σ_{j<k} y[j]²  ->  m(τ) en O(1)
            P[0] = 0.0;
            for (int j = 0; j < WR; ++j)
                P[j + 1] = P[j] + y[j] * y[j];

            auto nsdfR = [&] (int lag) noexcept
            {
                const double mm = (P[WR - lag] - P[0]) + (P[WR] - P[lag]);
                return mm > 1.0e-30 ? 2.0 * crossCorrelation (y, WR, lag) / mm : 0.0;
            };

            const int    f      = 1 << refineLog2;
            const double centre = periodA * (double) f;
            const int    lo     = std::max (2, (int) std::floor (centre) - f);
            const int    hi     = std::min (WR / 2, (int) std::ceil (centre) + f);

            std::array<double, 2 * (1 << kRefineFactorLog2) + 8> v {};
            int best = -1;
            for (int lag = lo - 1; lag <= hi + 1; ++lag)
            {
                v[(size_t) (lag - lo + 1)] = nsdfR (lag);
                if (lag >= lo && lag <= hi && (best < 0 || v[(size_t) (lag - lo + 1)] > v[(size_t) (best - lo + 1)]))
                    best = lag;
            }

            // Sommet strictement intérieur : on garde le raffinement ; sinon la NSDF est plate ou monotone,
            // l'estimation grossière reste la meilleure.
            if (best > lo && best < hi)
            {
                const size_t b = (size_t) (best - lo + 1);
                double peakR = 0.0;
                const double d = parabolicOffset (v[b - 1], v[b], v[b + 1], peakR);
                periodA = ((double) best + d) / (double) f;
                clarity = std::max (clarity, std::min (peakR, 1.0));
            }
        }

        est.clarity      = (float) std::clamp (clarity, 0.0, 1.0);
        est.brightnessHz = (float) brightness;
        est.frequencyHz  = analysisRate / periodA;
        est.periodSamples = periodA * (double) (1 << numStages);

        //--- décision voisé / non voisé -------------------------------------------------------------------------
        const double clarityThreshold = latest.voiced ? config.clarityOff : config.clarityOn;
        const bool inRange   = est.frequencyHz >= range.minHz * 0.97 && est.frequencyHz <= range.maxHz * 1.03;
        const bool loud      = est.levelDb > config.gateDb;
        const bool periodic  = clarity >= clarityThreshold;
        const bool sibilant  = brightness > std::max (config.sibilanceHz, 2.5 * est.frequencyHz)
                               && clarity < config.strongClarity;

        finish (est, est.frequencyHz, loud && inRange && periodic && ! sibilant);
    }

    void finish (PitchEstimate& est, double frequency, bool voiced) noexcept
    {
        est.frequencyHz = frequency;
        est.voiced      = voiced;
        latest          = est;
    }

    double sampleRate   = 0.0;
    double analysisRate = 0.0;
    double refineRate   = 0.0;
    int    numStages    = 0;
    int    refineLog2   = 0;
    int    refineLevel  = 0;

    std::array<HalfBandDecimator, kMaxStages> stages;
    detail::MirrorRing analysisRing, refineRing, refineLpRing;
    detail::Butterworth4 refineLowpass;
    std::vector<double> frameA, frameR, prefixR, nsdf;
    std::vector<int>    keyMaxima;

    int capWindowA = 0, capLagA = 0, capWindowR = 0;
    int windowA = 0, windowR = 0, lagMinA = 2, lagMaxA = 0;
    int hopA = 1, hopCounter = 0;
    double hopSeconds = 0.0015;
    std::int64_t inputCounter = 0;

    F0Range       range { kDetectorMinHz, kDetectorMaxHz };
    Config        config;
    PitchEstimate latest;
};

} // namespace aura::pitch

// AuraPitch — NextDSP · AuraSuite
// FormantProcessor.h : enveloppe spectrale vraie (True Envelope, Röbel & Rodet 2005) et déformation de formants.
//
//   * cepstralSmooth : log|X| -> cepstre réel -> liftrage (ordre O, bord adouci) -> log-enveloppe lissée ;
//   * trueEnvelope   : A₀ = log|X| ; Vᵢ = lissage(Aᵢ) ; Aᵢ₊₁ = max(Aᵢ, Vᵢ), jusqu'à ce que l'enveloppe passe à moins
//                      de tolDb sous chaque pic. Contrairement au cepstre simple, qui passe entre les harmoniques
//                      (et sous-estime les formants des voix aiguës), l'enveloppe vraie s'appuie sur les pics ;
//   * warp           : E'(f) = E(f / γ), interpolation linéaire en log : formants déplacés d'un facteur γ.
//
//   * ThroatModel    : longueur de gorge (80 … 120 %). Les formants F1 … F4 sont estimés par LPC (pôles du modèle
//                      tous-pôles) sur la voix sortante, décimée vers 11–12,6 kHz, et déplacés par f' = f · τ^(−g(f)),
//                      g = 1 jusqu'à 3,5 kHz puis demi-cosinus vers 0 à 5 kHz (déformation non linéaire).
//                      Chaque formant devient une cellule du 2e ordre A(z)/A'(z) : zéros sur le formant d'origine (f, B),
//                      pôles sur le formant déplacé (f', B·f'/f : même Q, la forme est étirée comme l'enveloppe), gain
//                      normalisé à 0 Hz. C'est exactement E(f·τ)/E(f) sous 3,5 kHz (±0,2 dB) ; mais la pente 1/f² des
//                      formants déplacés changerait le haut du spectre de ±13 à 16 dB. Les cellules ne traitent donc que
//                      la voie basse d'un croisement doublement complémentaire (Butterworth d'ordre 9 à 4,6 kHz, somme
//                      et différence de deux passe-tout : LP + HP = passe-tout) : au-dessus de 6 kHz la sortie est la
//                      voix elle-même à ±0,5 dB (±0,02 dB dès 8 kHz). Sans latence, à la fréquence native.
//                      τ = 100 % : rien n'est calculé (identité exacte). Sinon la sortie entière garde la phase du
//                      passe-tout (module plat) ; le Dry passe par le même passe-tout pour que le Mix reste cohérent.
//
// L'ordre cepstral se règle en échantillons de quéfrence : O = ½ · fs / F0 (Röbel). Il ne dépend que de la
// fréquence d'échantillonnage de la bande vocale (44,1 / 48 kHz en DXD, voir StudioFormantFilter.h) : l'enveloppe est la
// même à toutes les fréquences d'entrée. Aucune allocation après prepare().
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

#include "FFT.h"
#include "HalfBand.h"

namespace aura::pitch
{

class SpectralEnvelope
{
public:
    static constexpr double kLogFloor = -50.0;   // ≈ −434 dB : protège log(0)

    void prepare (int fftOrder)
    {
        fft.prepare (fftOrder);
        n    = fft.getSize();
        bins = fft.getNumBins();
        cep.assign ((size_t) n, 0.0);
        re.assign ((size_t) bins, 0.0);
        im.assign ((size_t) bins, 0.0);
        work.assign ((size_t) bins, 0.0);
    }

    int getFftSize() const noexcept { return n; }
    int getNumBins() const noexcept { return bins; }

    /** Ordre cepstral recommandé pour une F0 (Hz) à la fréquence fs. */
    int orderFor (double f0Hz, double fs) const noexcept
    {
        const double o = f0Hz > 0.0 ? 0.5 * fs / f0Hz : 0.5 * fs / 200.0;
        return std::clamp ((int) std::lround (o), 8, n / 4);
    }

    /** logIn[0 … N/2] (log naturel de la magnitude) -> logOut[0 … N/2] (peuvent être le même tableau). */
    void cepstralSmooth (const double* logIn, double* logOut, int order) noexcept
    {
        order = std::clamp (order, 1, n / 2 - 1);
        for (int k = 0; k < bins; ++k) { re[(size_t) k] = logIn[k]; im[(size_t) k] = 0.0; }
        fft.inverse (re.data(), im.data(), cep.data());

        // Liftre : 1 jusqu'à 0,8 O, puis demi-cosinus jusqu'à O (réduit les oscillations de Gibbs de l'enveloppe).
        const int taper0 = std::max (1, (int) (0.8 * order));
        for (int q = 1; q < n / 2; ++q)
        {
            double g = 0.0;
            if (q <= taper0)      g = 1.0;
            else if (q <= order)  g = 0.5 * (1.0 + std::cos (3.14159265358979323846 * (q - taper0) / (double) (order - taper0 + 1)));
            cep[(size_t) q]     *= g;
            cep[(size_t) (n - q)] *= g;
        }
        cep[(size_t) (n / 2)] = 0.0;

        fft.forward (cep.data(), re.data(), im.data());
        for (int k = 0; k < bins; ++k)
            logOut[k] = re[(size_t) k];
    }

    /** Enveloppe vraie. logMag[0 … N/2] -> envLog[0 … N/2]. Renvoie le nombre d'itérations effectuées. */
    int trueEnvelope (const double* logMag, double* envLog, int order, int maxIterations = 6, double tolDb = 1.0) noexcept
    {
        const double tol = tolDb / 8.685889638065035;      // dB -> népers
        for (int k = 0; k < bins; ++k)
            work[(size_t) k] = std::max (logMag[k], kLogFloor);

        cepstralSmooth (work.data(), envLog, order);
        int it = 1;
        for (; it < maxIterations; ++it)
        {
            double worst = 0.0;
            for (int k = 0; k < bins; ++k)
            {
                const double a = std::max (logMag[k], kLogFloor);
                worst = std::max (worst, a - envLog[k]);
                work[(size_t) k] = std::max (work[(size_t) k], envLog[k]);
            }
            if (worst < tol)
                break;
            cepstralSmooth (work.data(), envLog, order);
        }
        return it;
    }

    /** out[k] = env(k / γ), interpolation linéaire, bornes prolongées. */
    static void warp (const double* envLog, double* out, int numBins, double gamma) noexcept
    {
        const double inv = 1.0 / std::max (1.0e-6, gamma);
        for (int k = 0; k < numBins; ++k)
        {
            const double src = (double) k * inv;
            if (src >= (double) (numBins - 1)) { out[k] = envLog[numBins - 1]; continue; }
            const int    i = (int) src;
            const double f = src - (double) i;
            out[k] = envLog[i] + f * (envLog[i + 1] - envLog[i]);
        }
    }

private:
    RealFFT fft;
    int n = 0, bins = 0;
    std::vector<double> cep, re, im, work;
};


//==============================================================================================================
/** Longueur de gorge : déplacement non linéaire des formants F1 … F3 (voir l'en-tête). Thread audio, sans allocation
    après prepare(). Mono ou stéréo : analyse sur la somme, même filtre sur chaque canal (phase identique). */
class ThroatModel
{
public:
    static constexpr int    kMaxChannels = 2;
    static constexpr int    kMaxBlock    = 1024;
    static constexpr int    kOrder       = 14;    // LPC, ≈ fsA / 1 kHz + 2
    static constexpr int    kSlots       = 4;     // formants suivis
    static constexpr double kMinRatio    = 0.8, kMaxRatio = 1.2;
    static constexpr double kCrossoverHz = 4600.0;

    void prepare (double fs, int numChannels)
    {
        sampleRate = fs;
        channels   = std::clamp (numChannels, 1, kMaxChannels);
        stages = 0;
        while (fs / (double) (1 << stages) > 12600.0 && stages < kMaxStages) ++stages;
        fsA = fs / (double) (1 << stages);
        for (auto& d : decim) d.prepare (9.0);

        window = (int) std::lround (0.025 * fsA);
        hop    = std::max (1, (int) std::lround (0.005 * fsA));
        frame.assign ((size_t) window, 0.0);
        hann.resize ((size_t) window);
        for (int i = 0; i < window; ++i)
            hann[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / window);
        work.assign ((size_t) window, 0.0);
        engageBuf.assign (kMaxBlock, 0.0);

        // Fenêtre de décalage gaussienne de 20 Hz : écarte les pôles qui s'accrocheraient sur une harmonique, sans trop
        // élargir les formants (bande passante estimée à ±15 % sur une voyelle à 110 … 300 Hz).
        for (int k = 0; k <= kOrder; ++k)
        {
            const double a = 2.0 * kPi * 20.0 * k / fsA;
            lagWindow[(size_t) k] = std::exp (-0.5 * a * a);
        }
        designCrossover();
        smoothStep = 1.0 / (0.010 * fs);
        rampStep   = 1.0 / (0.020 * fs);
        slotCoeff  = 1.0 - std::exp (-(double) hop / fsA / 0.012);
        reset();
    }

    void reset() noexcept
    {
        for (auto& d : decim) d.reset();
        std::fill (frame.begin(), frame.end(), 0.0);
        writePos = 0; sinceHop = 0; filled = 0;
        for (auto& sl : slots) sl = Slot {};
        for (auto& c : sections) c = Section {};
        for (auto& st : state) st = {};
        for (auto& st : apWet) st = {};
        for (auto& st : apDry) st = {};
        voiceAmount = 0.0;
        ratio = target;
        engage = same (target, 1.0) ? 0.0 : 1.0;
        coeffCountdown = 0;
    }

    /** τ (0,8 … 1,2). */
    void setRatio (double r) noexcept { target = std::clamp (r, kMinRatio, kMaxRatio); }
    /** Passage non voisé : retour à l'identité des formants en 10 ms. */
    void setVoiced (bool v) noexcept  { voiced = v; }

    /** true si le modèle ne fait rien (τ = 100 % demandé et atteint) : l'appelant saute process() et processDry(). */
    bool isIdle() const noexcept { return same (target, 1.0) && same (ratio, 1.0) && same (engage, 0.0); }

    /** Wet, en place, n ≤ kMaxBlock échantillons à partir de la position offset du bloc courant. */
    void process (double* const* io, int nc, int n, int offset) noexcept
    {
        nc = std::min (nc, channels);
        for (int i = 0; i < n; ++i)
        {
            // τ revient à 1 avant que le passe-tout se retire ; il s'engage avant que τ ne quitte 1.
            const bool wantOn = ! same (target, 1.0);
            if (wantOn || same (ratio, 1.0)) engage += std::clamp ((wantOn ? 1.0 : 0.0) - engage, -rampStep, rampStep);
            if (engage >= 1.0 || ! wantOn) ratio += std::clamp (target - ratio, -rampStep * 0.4, rampStep * 0.4);
            voiceAmount += std::clamp ((voiced ? 1.0 : 0.0) - voiceAmount, -smoothStep, smoothStep);
            engageBuf[(size_t) (offset + i)] = engage;

            double m = 0.0;
            for (int ch = 0; ch < nc; ++ch) m += io[ch][i];
            pushAnalysis (m / nc);
            if (--coeffCountdown <= 0) { updateCoefficients(); coeffCountdown = 16; }

            for (int ch = 0; ch < nc; ++ch)
            {
                const double x = io[ch][i];
                double a1 = 0.0, a2 = 0.0;
                runAllpass (apWet[(size_t) ch], x, a1, a2);
                double lp = 0.5 * (a1 + a2);
                const double hp = 0.5 * (a1 - a2);
                for (int k = 0; k < kSlots; ++k)
                {
                    const auto& c = sections[(size_t) k];
                    if (! c.on) continue;
                    auto& z = state[(size_t) ch][(size_t) k];
                    const double y = c.g * (lp + c.b1 * z.x1 + c.b2 * z.x2) - c.a1 * z.y1 - c.a2 * z.y2;
                    z.x2 = z.x1; z.x1 = lp; z.y2 = z.y1; z.y1 = y;
                    lp = y;
                }
                io[ch][i] = x + engage * ((lp + hp) - x);
            }
        }
    }

    /** Dry du même bloc (après process()) : même passe-tout, même dosage, pour un Mix cohérent en phase. */
    void processDry (double* const* io, int nc, int n) noexcept
    {
        nc = std::min (nc, channels);
        for (int i = 0; i < n; ++i)
        {
            const double e = engageBuf[(size_t) i];
            for (int ch = 0; ch < nc; ++ch)
            {
                const double x = io[ch][i];
                double a1 = 0.0, a2 = 0.0;
                runAllpass (apDry[(size_t) ch], x, a1, a2);
                io[ch][i] = x + e * (a1 - x);
            }
        }
    }

    /** Déformation de fréquence (Hz) pour τ. */
    static double warp (double f, double tau) noexcept
    {
        double g = 0.0;
        if (f <= 3500.0)      g = 1.0;
        else if (f < 5000.0)  g = 0.5 * (1.0 + std::cos (kPi * (f - 3500.0) / 1500.0));
        return f * std::pow (tau, -g);
    }

    /** Pour les tests : formants suivis (Hz, 0 si inactif). */
    std::array<double, kSlots> getTrackedFormants() const noexcept
    {
        std::array<double, kSlots> f {};
        for (int k = 0; k < kSlots; ++k) f[(size_t) k] = slots[(size_t) k].weight > 0.5 ? slots[(size_t) k].f : 0.0;
        std::sort (f.begin(), f.end());
        return f;
    }

    /** Pour les tests : (fréquence, bande passante) des formants suivis, Hz. */
    std::array<std::pair<double, double>, kSlots> getTrackedFormantShapes() const noexcept
    {
        std::array<std::pair<double, double>, kSlots> r {};
        for (int k = 0; k < kSlots; ++k)
            if (slots[(size_t) k].weight > 0.5) r[(size_t) k] = { slots[(size_t) k].f, slots[(size_t) k].bw };
        std::sort (r.begin(), r.end());
        return r;
    }

private:
    /** Égalité exacte voulue (identité au bit près à τ = 100 %), sans l'avertissement -Wfloat-equal. */
    static bool same (double a, double b) noexcept { return ! (a < b) && ! (a > b); }

    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kMaxStages = 6;
    static constexpr int kXoverOrder = 9;
    struct Slot { double f = 0.0, bw = 0.0, weight = 0.0; bool seen = false; };
    struct Section { double g = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; bool on = false; };
    struct State { double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0; };
    struct AllpassCell { double c = 0.0, r2 = 0.0; bool first = false; };      // 1er ordre : r2 = a
    struct AllpassState { std::array<State, 5> s {}; };
    struct AllpassPair { AllpassState b1, b2; };

    void designCrossover()
    {
        // Butterworth analogique d'ordre 9 -> bilinéaire (préwarping) ; pôles répartis alternativement entre les deux
        // branches passe-tout : LP = (A1 + A2) / 2, HP = (A1 − A2) / 2.
        using C = std::complex<double>;
        const double wc = 2.0 * sampleRate * std::tan (kPi * kCrossoverHz / sampleRate);
        std::array<C, kXoverOrder> zp {};
        for (int k = 0; k < kXoverOrder; ++k)
        {
            const C sp = wc * std::polar (1.0, kPi * (2.0 * k + kXoverOrder + 1) / (2.0 * kXoverOrder));
            zp[(size_t) k] = (2.0 * sampleRate + sp) / (2.0 * sampleRate - sp);
        }
        std::array<C, kXoverOrder> upper {};
        int nu = 0; double realPole = 0.0;
        for (const auto& p : zp)
        {
            if (p.imag() > 1.0e-12)            upper[(size_t) nu++] = p;
            else if (std::abs (p.imag()) <= 1.0e-12) realPole = p.real();
        }
        std::sort (upper.begin(), upper.begin() + nu, [] (C a, C b) { return std::arg (a) < std::arg (b); });
        n1 = n2 = 0;
        branch1[(size_t) n1++] = AllpassCell { 0.0, realPole, true };
        for (int i = 0; i < nu; ++i)
        {
            const AllpassCell cell { 2.0 * upper[(size_t) i].real(), std::norm (upper[(size_t) i]), false };
            if (i % 2 == 0) branch2[(size_t) n2++] = cell;
            else            branch1[(size_t) n1++] = cell;
        }
    }

    static double runCells (const std::array<AllpassCell, 5>& cells, int count, AllpassState& st, double x) noexcept
    {
        for (int k = 0; k < count; ++k)
        {
            const auto& c = cells[(size_t) k];
            auto& z = st.s[(size_t) k];
            double y;
            if (c.first) y = -c.r2 * x + z.x1 + c.r2 * z.y1;                         // (z⁻¹ − a) / (1 − a z⁻¹)
            else         y = c.r2 * x - c.c * z.x1 + z.x2 + c.c * z.y1 - c.r2 * z.y2;   // (r² − c z⁻¹ + z⁻²) / (1 − c z⁻¹ + r² z⁻²)
            z.x2 = z.x1; z.x1 = x; z.y2 = z.y1; z.y1 = y;
            x = y;
        }
        return x;
    }

    void runAllpass (AllpassPair& st, double x, double& a1, double& a2) noexcept
    {
        a1 = runCells (branch1, n1, st.b1, x);
        a2 = runCells (branch2, n2, st.b2, x);
    }

    void pushAnalysis (double x) noexcept
    {
        double y = x;
        for (int s = 0; s < stages; ++s)
            if (! decim[(size_t) s].push (y, y))
                return;
        frame[(size_t) writePos] = y;
        writePos = (writePos + 1) % window;
        filled = std::min (filled + 1, window);
        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            if (filled == window) analyse();
        }
    }

    void analyse() noexcept
    {
        // Trame fenêtrée, pré-accentuée (0,94) : le modèle LPC décrit les formants plutôt que la pente glottique.
        double prev = frame[(size_t) writePos];
        double energy = 0.0;
        for (int i = 0; i < window; ++i)
        {
            const double v = frame[(size_t) ((writePos + i) % window)];
            work[(size_t) i] = (v - 0.94 * prev) * hann[(size_t) i];
            prev = v;
            energy += work[(size_t) i] * work[(size_t) i];
        }
        for (auto& sl : slots) sl.seen = false;

        std::array<double, kSlots> candF {}, candB {};
        int numCand = 0;
        if (energy > 1.0e-12 && voiceAmount > 0.0)
        {
            std::array<double, kOrder + 1> r {};
            for (int k = 0; k <= kOrder; ++k)
            {
                double acc = 0.0;
                for (int i = k; i < window; ++i) acc += work[(size_t) i] * work[(size_t) (i - k)];
                r[(size_t) k] = acc * lagWindow[(size_t) k];
            }
            r[0] *= 1.0001;                         // correction de bruit blanc (−40 dB)

            std::array<double, kOrder + 1> a {};
            if (levinson (r, a))
            {
                std::array<std::complex<double>, kOrder> roots {};
                findRoots (a, roots);
                for (const auto& z : roots)
                {
                    if (z.imag() <= 0.0) continue;
                    const double mag = std::abs (z);
                    if (mag >= 1.0 || mag < 0.5) continue;
                    const double f  = std::arg (z) * fsA / (2.0 * kPi);
                    const double bw = -std::log (mag) * fsA / kPi;
                    if (f < 150.0 || f > 4800.0 || bw > 600.0) continue;
                    if (numCand < kSlots) { candF[(size_t) numCand] = f; candB[(size_t) numCand] = bw; ++numCand; }
                    else
                    {
                        int hi = 0;                 // garder les kSlots formants les plus bas
                        for (int c = 1; c < kSlots; ++c) if (candF[(size_t) c] > candF[(size_t) hi]) hi = c;
                        if (f < candF[(size_t) hi]) { candF[(size_t) hi] = f; candB[(size_t) hi] = bw; }
                    }
                }
            }
        }

        // Appariement par proximité (log-fréquence, ≤ 0,35 octave) : un formant garde sa cellule d'une trame à l'autre.
        std::array<bool, kSlots> used {};
        for (int c = 0; c < numCand; ++c)
        {
            int best = -1; double bestD = 0.35;
            for (int k = 0; k < kSlots; ++k)
            {
                const auto& sl = slots[(size_t) k];
                if (used[(size_t) k] || sl.weight <= 0.0) continue;
                const double d = std::abs (std::log2 (candF[(size_t) c] / sl.f));
                if (d < bestD) { bestD = d; best = k; }
            }
            if (best < 0)
                for (int k = 0; k < kSlots; ++k)
                    if (! used[(size_t) k] && slots[(size_t) k].weight <= 0.0)
                    {
                        best = k; slots[(size_t) k].f = candF[(size_t) c]; slots[(size_t) k].bw = candB[(size_t) c];
                        break;
                    }
            if (best < 0) continue;
            used[(size_t) best] = true;
            auto& sl = slots[(size_t) best];
            sl.seen = true;
            sl.f  += slotCoeff * (candF[(size_t) c] - sl.f);
            sl.bw += slotCoeff * (std::clamp (candB[(size_t) c], 60.0, 400.0) - sl.bw);
        }
        for (auto& sl : slots)
            sl.weight = std::clamp (sl.weight + (sl.seen ? 0.34 : -0.34), 0.0, 1.0);   // entrée / sortie en 3 trames
    }

    void updateCoefficients() noexcept
    {
        const double tau = 1.0 + voiceAmount * (ratio - 1.0);
        for (int k = 0; k < kSlots; ++k)
        {
            auto& c = sections[(size_t) k];
            const auto& sl = slots[(size_t) k];
            if (sl.weight <= 0.0 || same (tau, 1.0))
            {
                if (c.on) { c.b1 = c.a1; c.b2 = c.a2; c.g = 1.0; }     // identité : l'état résiduel s'éteint seul
                if (same (tau, 1.0) && sl.weight <= 0.0) c.on = false;
                continue;
            }
            // f' et B' = B·f'/f (même Q) ; le poids du formant (apparition / disparition) dose le déplacement.
            const double fNew = sl.f + sl.weight * (warp (sl.f, tau) - sl.f);
            const double bNew = sl.bw * fNew / sl.f;
            const double r0 = std::exp (-kPi * sl.bw / sampleRate), r1 = std::exp (-kPi * bNew / sampleRate);
            const double w0 = 2.0 * kPi * sl.f / sampleRate,       w1 = 2.0 * kPi * fNew / sampleRate;
            if (! c.on)
                for (auto& st : state) st[(size_t) k] = State {};
            c.b1 = -2.0 * r0 * std::cos (w0); c.b2 = r0 * r0;
            c.a1 = -2.0 * r1 * std::cos (w1); c.a2 = r1 * r1;
            c.g  = (1.0 + c.a1 + c.a2) / (1.0 + c.b1 + c.b2);                 // gain unité à 0 Hz
            c.on = true;
        }
    }

    static bool levinson (const std::array<double, kOrder + 1>& r, std::array<double, kOrder + 1>& a) noexcept
    {
        a.fill (0.0); a[0] = 1.0;
        double err = r[0];
        if (err <= 0.0) return false;
        std::array<double, kOrder + 1> tmp {};
        for (int i = 1; i <= kOrder; ++i)
        {
            double acc = r[(size_t) i];
            for (int j = 1; j < i; ++j) acc += a[(size_t) j] * r[(size_t) (i - j)];
            const double k = -acc / err;
            tmp = a;
            for (int j = 1; j < i; ++j) a[(size_t) j] = tmp[(size_t) j] + k * tmp[(size_t) (i - j)];
            a[(size_t) i] = k;
            err *= (1.0 - k * k);
            if (err <= 0.0) return false;
        }
        return true;
    }

    /** Racines de z^p + a1 z^(p−1) + … + ap (Durand-Kerner, 80 itérations au plus). */
    static void findRoots (const std::array<double, kOrder + 1>& a, std::array<std::complex<double>, kOrder>& z) noexcept
    {
        using C = std::complex<double>;
        for (int k = 0; k < kOrder; ++k)
            z[(size_t) k] = std::polar (0.9, 2.0 * kPi * k / kOrder + 0.4);
        auto poly = [&] (C x)
        {
            C v (1.0, 0.0);
            for (int j = 1; j <= kOrder; ++j) v = v * x + a[(size_t) j];
            return v;
        };
        for (int it = 0; it < 80; ++it)
        {
            double change = 0.0;
            for (int k = 0; k < kOrder; ++k)
            {
                C den (1.0, 0.0);
                for (int j = 0; j < kOrder; ++j)
                    if (j != k) den *= (z[(size_t) k] - z[(size_t) j]);
                if (std::abs (den) < 1.0e-300) den = C (1.0e-300, 0.0);
                const C d = poly (z[(size_t) k]) / den;
                z[(size_t) k] -= d;
                change = std::max (change, std::abs (d));
            }
            if (change < 1.0e-11) break;
        }
    }

    double sampleRate = 48000.0, fsA = 12000.0;
    int channels = 1, stages = 0, window = 300, hop = 60;
    std::array<detail::HalfBandDecimatorT<12>, kMaxStages> decim {};
    std::vector<double> frame, hann, work, engageBuf;
    std::array<double, kOrder + 1> lagWindow {};
    int writePos = 0, sinceHop = 0, filled = 0, coeffCountdown = 0;

    std::array<AllpassCell, 5> branch1 {}, branch2 {};
    int n1 = 0, n2 = 0;
    std::array<AllpassPair, kMaxChannels> apWet {}, apDry {};

    std::array<Slot, kSlots> slots {};
    std::array<Section, kSlots> sections {};
    std::array<std::array<State, kSlots>, kMaxChannels> state {};
    double target = 1.0, ratio = 1.0, voiceAmount = 0.0, engage = 0.0;
    bool voiced = false;
    double smoothStep = 0.0, rampStep = 0.0, slotCoeff = 0.5;
};

} // namespace aura::pitch

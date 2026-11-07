// AuraPitch — NextDSP · AuraSuite
// ScaleQuantizer.h : quantification de gamme + courbe de correction (Retune Speed / Humanize).
//
// Aucun en-tête JUCE : le module se compile et se teste seul (Tests/PitchDspSelfTest.cpp).
// Tout est temps réel : aucune allocation, aucun verrou, aucune exception. Les réglages se font depuis le
// thread audio (valeurs lues dans l'APVTS au début de chaque bloc) ; les deux classes ne sont pas partagées
// entre threads.
//
// Conventions
//   * hauteur en demi-tons MIDI flottants (69.0 = A4), relative à une référence A4 réglable (415 … 466 Hz) ;
//   * classe de hauteur pc = 0 (C) … 11 (B), absolue (indépendante de Key) ;
//   * masque 12 bits : bit pc = note concernée.
#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace aura::pitch
{

enum class ScaleType : int { Chromatic = 0, Major, Minor, Pentatonic, Custom };

/** État d'une note du clavier 12 notes (mode Custom).
    Active  : la note est une cible de correction.
    Bypass  : quand le chanteur est le plus près de cette note, rien n'est corrigé (passage naturel).
    Removed : la note n'est jamais une cible ; la voix est tirée vers la note active la plus proche. */
enum class NoteState : std::uint8_t { Active = 0, Bypass, Removed };

constexpr double kMinReferenceA4 = 415.0;
constexpr double kMaxReferenceA4 = 466.0;

inline double hzToMidi (double hz, double referenceA4 = 440.0) noexcept { return 69.0 + 12.0 * std::log2 (hz / referenceA4); }
inline double midiToHz (double midi, double referenceA4 = 440.0) noexcept { return referenceA4 * std::exp2 ((midi - 69.0) / 12.0); }
inline double semitonesToRatio (double semitones) noexcept { return std::exp2 (semitones / 12.0); }
constexpr int pitchClassOf (int midiNote) noexcept { return ((midiNote % 12) + 12) % 12; }

//==============================================================================================================
class ScaleQuantizer
{
public:
    using Mask = std::uint16_t;
    static constexpr Mask kAllNotes = 0x0FFF;
    static constexpr int  kNoNote   = INT_MIN;

    /** Intervalles de la gamme par rapport à la tonique (bit 0 = tonique). */
    static constexpr Mask intervalsOf (ScaleType type) noexcept
    {
        switch (type)
        {
            case ScaleType::Major:      return bits ({ 0, 2, 4, 5, 7, 9, 11 });
            case ScaleType::Minor:      return bits ({ 0, 2, 3, 5, 7, 8, 10 });   // mineur naturel (éolien)
            case ScaleType::Pentatonic: return bits ({ 0, 2, 4, 7, 9 });          // pentatonique majeure
            case ScaleType::Chromatic:
            case ScaleType::Custom:     break;
        }
        return kAllNotes;
    }

    /** Masque relatif à la tonique -> masque absolu (C = bit 0) pour la tonalité key (0 = C … 11 = B). */
    static constexpr Mask rotate (Mask relative, int key) noexcept
    {
        key = pitchClassOf (key);
        const unsigned r = relative & kAllNotes;
        return static_cast<Mask> (((r << key) | (r >> (12 - key))) & kAllNotes);
    }

    struct Result
    {
        double inputMidi      = 0.0;
        double targetMidi     = 0.0;   // note cible (entière) ; = inputMidi si bypassed
        double deviationCents = 0.0;   // (entrée − cible) × 100, pour l'indicateur de l'interface
        int    targetNote     = kNoNote;
        bool   bypassed       = true;  // aucune correction à appliquer
    };

    ScaleQuantizer() noexcept { rebuildMasks(); }

    //--- réglages -----------------------------------------------------------------------------------------------
    void setKey (int newKey) noexcept                  { key = pitchClassOf (newKey); rebuildMasks(); }
    void setScale (ScaleType newScale) noexcept        { scale = newScale; rebuildMasks(); }
    void setReferenceA4 (double hz) noexcept           { referenceA4 = std::clamp (hz, kMinReferenceA4, kMaxReferenceA4); }

    /** Marge (cents) que la voix doit gagner sur la note cible courante avant de changer de cible.
        Évite le « battement » entre deux notes quand le chanteur est à mi-chemin. 0 … 45 cents. */
    void setHysteresisCents (double cents) noexcept    { hysteresis = std::clamp (cents, 0.0, 45.0) / 100.0; }

    /** Mode Custom : état d'une note (pc absolu). Sans effet sur les autres gammes tant que Custom n'est pas choisi. */
    void setNoteState (int pc, NoteState state) noexcept { customStates[(size_t) pitchClassOf (pc)] = state; rebuildMasks(); }
    void setCustomStates (const std::array<NoteState, 12>& states) noexcept { customStates = states; rebuildMasks(); }

    /** Copie la gamme courante (Key + Scale) dans le clavier Custom : c'est ce que fait l'interface quand on clique
        une touche alors qu'une gamme prédéfinie est choisie, avant de basculer en Custom. */
    void copyCurrentScaleToCustom() noexcept
    {
        for (int pc = 0; pc < 12; ++pc)
            customStates[(size_t) pc] = ((targetMask >> pc) & 1u) != 0 ? NoteState::Active
                                      : ((bypassMask >> pc) & 1u) != 0 ? NoteState::Bypass
                                                                       : NoteState::Removed;
    }

    int       getKey() const noexcept                  { return key; }
    ScaleType getScale() const noexcept                { return scale; }
    double    getReferenceA4() const noexcept          { return referenceA4; }
    Mask      getTargetMask() const noexcept           { return targetMask; }
    Mask      getBypassMask() const noexcept           { return bypassMask; }
    NoteState getNoteState (int pc) const noexcept     { return customStates[(size_t) pitchClassOf (pc)]; }
    bool      isTarget (int midiNote) const noexcept   { return ((targetMask >> pitchClassOf (midiNote)) & 1u) != 0; }

    /** Oublie la note cible courante (fin de phrase, passage non voisé, changement de gamme). */
    void reset() noexcept { currentNote = kNoNote; }

    //--- quantification -----------------------------------------------------------------------------------------
    Result quantizeHz (double hz) noexcept
    {
        if (! (hz > 0.0) || ! std::isfinite (hz))
            return bypass (0.0);
        return quantize (hzToMidi (hz, referenceA4));
    }

    Result quantize (double inputMidi) noexcept
    {
        if (! std::isfinite (inputMidi) || targetMask == 0)
            return bypass (inputMidi);

        // Note bypass : décidée sur la note chromatique la plus proche (comme sur un clavier Auto-Tune).
        const int nearestChromatic = (int) std::lround (inputMidi);
        if (((bypassMask >> pitchClassOf (nearestChromatic)) & 1u) != 0)
            return bypass (inputMidi);

        // Note active la plus proche. Les 14 notes de floor−6 … floor+7 couvrent toutes les classes de hauteur.
        const int base = (int) std::floor (inputMidi);
        int    best     = kNoNote;
        double bestDist = 1.0e9;
        for (int n = base - 6; n <= base + 7; ++n)
        {
            if (! isTarget (n))
                continue;
            const double d = std::abs (inputMidi - (double) n);
            if (d < bestDist) { bestDist = d; best = n; }
        }

        // Hystérésis : on garde la cible courante tant que la nouvelle n'est pas plus proche d'au moins la marge.
        if (currentNote != kNoNote && best != currentNote && isTarget (currentNote))
        {
            const double dCurrent = std::abs (inputMidi - (double) currentNote);
            if (dCurrent < bestDist + hysteresis)
                best = currentNote;
        }

        currentNote = best;

        Result r;
        r.inputMidi      = inputMidi;
        r.targetMidi     = (double) best;
        r.deviationCents = (inputMidi - (double) best) * 100.0;
        r.targetNote     = best;
        r.bypassed       = false;
        return r;
    }

private:
    static constexpr Mask bits (std::initializer_list<int> intervals) noexcept
    {
        Mask m = 0;
        for (int i : intervals)
            m = static_cast<Mask> (m | (1u << i));
        return m;
    }

    Result bypass (double inputMidi) noexcept
    {
        currentNote = kNoNote;
        Result r;
        r.inputMidi  = inputMidi;
        r.targetMidi = inputMidi;
        r.bypassed   = true;
        return r;
    }

    void rebuildMasks() noexcept
    {
        if (scale == ScaleType::Custom)
        {
            targetMask = 0;
            bypassMask = 0;
            for (int pc = 0; pc < 12; ++pc)
            {
                if (customStates[(size_t) pc] == NoteState::Active) targetMask = static_cast<Mask> (targetMask | (1u << pc));
                if (customStates[(size_t) pc] == NoteState::Bypass) bypassMask = static_cast<Mask> (bypassMask | (1u << pc));
            }
        }
        else
        {
            targetMask = rotate (intervalsOf (scale), key);
            bypassMask = 0;
        }

        if (currentNote != kNoNote && ! isTarget (currentNote))
            currentNote = kNoNote;
    }

    int       key         = 0;
    ScaleType scale       = ScaleType::Chromatic;
    double    referenceA4 = 440.0;
    double    hysteresis  = 0.12;          // 12 cents
    Mask      targetMask  = kAllNotes;
    Mask      bypassMask  = 0;
    int       currentNote = kNoNote;
    std::array<NoteState, 12> customStates {};   // tout Active
};

//==============================================================================================================
/** Courbe de correction : transforme (hauteur entrante, note cible) en décalage de hauteur à appliquer.

    décalage(t) = c(t) + Transpose + FineTune/100      (demi-tons ; ratio = 2^(décalage/12))
    c → (cible − entrée) par un filtre du 1er ordre de constante τ :

      τ = RetuneSpeed / 3 (≈ 95 % de la correction atteinte en RetuneSpeed)  + Humanize × S(t) × 350 ms

    S(t) passe de 0 à 1 entre 80 et 300 ms de note tenue (même cible, voix voisée) : l'attaque est corrigée à la
    vitesse choisie, puis la correction ralentit sur la tenue, ce qui laisse passer le vibrato (5–7 Hz) alors que la
    note moyenne reste juste. RetuneSpeed = 0 et Humanize = 0 : c = cible − entrée à chaque trame (hard-tune).

    Passage non voisé : c est gardé 30 ms (trous brefs du détecteur), puis revient à 0 (τ = 20 ms). */
class CorrectionCurve
{
public:
    static constexpr double kMaxRetuneMs = 400.0;

    /** Courbe logarithmique du potentiomètre Retune Speed : x ∈ [0, 1] -> 0 … 400 ms (401^x − 1).
        x = 0,5 -> 19 ms ; x = 0,75 -> 89 ms : la moitié de la course est consacrée aux réglages rapides. */
    static double retuneMsFromNormalised (double x) noexcept  { return std::pow (kMaxRetuneMs + 1.0, std::clamp (x, 0.0, 1.0)) - 1.0; }
    static double normalisedFromRetuneMs (double ms) noexcept { return std::log1p (std::clamp (ms, 0.0, kMaxRetuneMs)) / std::log (kMaxRetuneMs + 1.0); }

    void setRetuneMs (double ms) noexcept         { retuneTau = std::clamp (ms, 0.0, kMaxRetuneMs) / 3000.0; }
    void setHumanize (double amount01) noexcept   { humanize = std::clamp (amount01, 0.0, 1.0); }
    void setTranspose (double semitones) noexcept { transpose = std::clamp (semitones, -24.0, 24.0); }
    void setFineTuneCents (double cents) noexcept { fineTune = std::clamp (cents, -100.0, 100.0) / 100.0; }

    void reset() noexcept
    {
        correction = 0.0; lastTarget = ScaleQuantizer::kNoNote; noteTime = 0.0; unvoicedTime = 1.0;
    }

    /** Avance la courbe de dt secondes (une trame d'analyse du PitchDetector) et renvoie le décalage total en
        demi-tons. voiced = décision du détecteur ; q = résultat du ScaleQuantizer pour cette trame. */
    double process (const ScaleQuantizer::Result& q, bool voiced, double dtSeconds) noexcept
    {
        dtSeconds = std::max (0.0, dtSeconds);

        if (voiced && ! q.bypassed)
        {
            unvoicedTime = 0.0;
            if (q.targetNote != lastTarget) { lastTarget = q.targetNote; noteTime = 0.0; }
            else                            { noteTime += dtSeconds; }

            const double sustain = smoothstep ((noteTime - kHumanizeStart) / (kHumanizeFull - kHumanizeStart));
            const double tau     = retuneTau + humanize * sustain * kHumanizeTau;
            const double desired = q.targetMidi - q.inputMidi;
            correction += onePole (tau, dtSeconds) * (desired - correction);
        }
        else
        {
            lastTarget = ScaleQuantizer::kNoNote;
            noteTime   = 0.0;
            unvoicedTime += dtSeconds;
            if (q.bypassed && voiced)        // note bypass : retour naturel, à la vitesse de retune
                correction += onePole (std::max (retuneTau, kReleaseTau), dtSeconds) * (0.0 - correction);
            else if (unvoicedTime > kHoldSeconds)
                correction += onePole (kReleaseTau, dtSeconds) * (0.0 - correction);
        }

        return getShiftSemitones();
    }

    double getCorrectionSemitones() const noexcept { return correction; }
    double getShiftSemitones() const noexcept      { return correction + transpose + fineTune; }
    double getPitchRatio() const noexcept          { return semitonesToRatio (getShiftSemitones()); }

private:
    static constexpr double kHumanizeStart = 0.080;
    static constexpr double kHumanizeFull  = 0.300;
    static constexpr double kHumanizeTau   = 0.350;
    static constexpr double kHoldSeconds   = 0.030;
    static constexpr double kReleaseTau    = 0.020;

    static double smoothstep (double x) noexcept { x = std::clamp (x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); }
    static double onePole (double tau, double dt) noexcept { return tau <= 0.0 ? 1.0 : 1.0 - std::exp (-dt / tau); }

    double retuneTau    = 0.0;
    double humanize     = 0.0;
    double transpose    = 0.0;
    double fineTune     = 0.0;
    double correction   = 0.0;
    int    lastTarget   = ScaleQuantizer::kNoNote;
    double noteTime     = 0.0;
    double unvoicedTime = 1.0;
};

} // namespace aura::pitch

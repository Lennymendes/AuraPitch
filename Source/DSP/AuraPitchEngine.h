// AuraPitch — NextDSP · AuraSuite
// AuraPitchEngine.h : chef d'orchestre DSP (sans JUCE). PitchDetector -> ScaleQuantizer -> CorrectionCurve ->
// SpliceShifter / PSOLAShifter (Live) ou StudioDxdShifter (Studio/DXD) -> Mix aligné -> LockFreePitchFifo.
//
// Cœur de transposition (étape 4)
//   * SpliceShifter (cas courant : correction de hauteur, formants conservés naturellement) : une seule tête de
//     lecture, interpolation sinc 48 points, raccords synchrones de la période glottique (T* par corrélation ±10 %,
//     sous-échantillon) : une seule voix à la fois, aucun dédoublement, bande passante intacte jusqu'à 20 kHz.
//   * PSOLAShifter (effets : FormantShift ≠ 0, ou Preserve avec |Transpose| ≥ 2 dt) : grains sinc, marques
//     glottiques par corrélation locale. Le moteur passe de l'un à l'autre par un fondu de 10 ms (usePsola()).
//   * Voisement maintenu 50 ms : une micro-coupure du détecteur ne fait plus basculer la sortie vers le Dry.
//
// Étape 5A (expressif)
//   * Transition (Flex) 0 … 200 ms : lissage inter-notes distinct du Retune (CorrectionCurve).
//   * MIDI Target : cible = note MIDI tenue la plus proche + Pitch Bend ±2 dt (ScaleQuantizer::quantizeToNotes).
//     Les évènements sont horodatés à l'échantillon ; Live les applique à l'instant de la trame traitée, Studio à
//     l'instant du centre de la trame analysée (même alignement que la voix).
//   * Auto-Key : histogramme chromatique des trames voisées -> tonalité + confiance (AutoKeyDetector.h).
//   * Throat 80 … 120 % : formants F1 … F3 déplacés sur le Wet (ThroatModel, FormantProcessor.h), sans latence.
//   * Doubler 0 … 100 % (stéréo) : deux voix supplémentaires lues par des têtes à raccords glottiques à +9 ms / −7 c
//     (gauche) et +14 ms / +7 c (droite). À 0 % : rien n'est calculé.
//
// Étape 5B (GRAPH EDITOR, NoteGraphModel.h)
//   * setTimeline() : position de la timeline du DAW au début de chaque bloc (AudioPlayHead) ; chaque trame est
//     datée sur la timeline (horloge interne -> secondes, segments mémorisés).
//   * Capture : pendant la lecture, trames d'analyse -> CaptureFifo (sans verrou) -> message thread.
//   * Rejeu : là où une note éditée couvre l'instant t, le décalage vient du GraphSnapshot (cible + vibrato dosé +
//     glissé − hauteur chantée capturée), calculé pour l'instant exact du signal lu (Live : position de la tête de
//     lecture, tous les 32 échantillons ; Studio : piste horodatée). Hors des notes, la correction automatique.
//
// Latences (échantillons à la fréquence native, jusqu'à 384 kHz)
//   * Live   : D = 2 ms. La hauteur appliquée est la dernière estimation valide dès sa sortie du détecteur.
//   * Studio : ≈ 55–62 ms selon la fréquence, constante. Retard du détecteur pour l'InputType le plus grave + un
//              saut + découpe multicadence + raccords + filtre de formants différentiel (STFT 1024, module seul,
//              G = E(k/γ) / E(k)). La commande est lue à l'instant exact de l'échantillon traité.
// Mix : sortie = Dry + Mix × (Wet − Dry). Un passage non voisé donne Wet = Dry exactement.
// Temps réel : prepare() alloue ; process() et setParameters() n'allouent rien et ne prennent aucun verrou.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "AutoKeyDetector.h"
#include "FormantProcessor.h"
#include "LockFreePitchFifo.h"
#include "NoteGraphModel.h"
#include "PSOLAShifter.h"
#include "PitchDetector.h"
#include "ScaleQuantizer.h"
#include "SpliceShifter.h"
#include "StudioFormantFilter.h"

namespace aura::pitch
{

enum class TrackingMode : int { Live = 0, Studio };

struct AuraPitchParams
{
    InputType  inputType       = InputType::AltoTenor;
    int        key             = 0;
    ScaleType  scale           = ScaleType::Chromatic;
    std::array<NoteState, 12> customNotes {};
    double     retuneMs        = 20.0;
    double     humanize        = 0.0;      // 0 … 1
    double     formantShift    = 0.0;      // demi-tons
    bool       formantPreserve = true;
    double     transpose       = 0.0;      // demi-tons
    double     fineTuneCents   = 0.0;
    double     referenceA4     = 440.0;
    TrackingMode mode          = TrackingMode::Live;
    double     mix             = 1.0;      // 0 … 1
    // étape 5A
    double     transitionMs    = 0.0;      // 0 … 200 ms
    double     throat          = 1.0;      // 0,8 … 1,2
    double     doubler         = 0.0;      // 0 … 1
    bool       midiTarget      = false;
};

/** Évènement MIDI pour le mode MIDI Target (horodaté dans le bloc suivant). */
struct MidiTargetEvent
{
    enum Type : std::uint8_t { NoteOn, NoteOff, PitchBend, AllNotesOff };
    Type   type = NoteOn;
    int    note = 0;          // NoteOn / NoteOff
    double bend = 0.0;        // PitchBend, −1 … +1
};

class AuraPitchEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kChunk       = 1024;

    //--- préparation ----------------------------------------------------------------------------------------------
    void prepare (double newSampleRate, int numChannels)
    {
        sampleRate = newSampleRate;
        channels   = std::clamp (numChannels, 1, kMaxChannels);

        detector.prepare (sampleRate);
        psola.prepare (sampleRate, channels, 0.002);
        splice.prepare (sampleRate, channels, 0.002);
        holdSamples = (std::int64_t) (0.050 * sampleRate);
        studio.prepare (sampleRate, channels);
        throatModel.prepare (sampleRate, channels);
        prepareDoubler();

        double worst = 0.0;
        for (auto t : { InputType::Soprano, InputType::AltoTenor, InputType::LowMale, InputType::Instrument })
        {
            detector.setInputType (t);
            worst = std::max (worst, detector.getAnalysisLatencySamples());
        }
        detectionDelay = (int) std::ceil (worst) + detector.getHopInputSamples() + 2;
        liveLatency    = psola.getLatencySamples();
        studioLatency  = detectionDelay + studio.getLatencySamples();

        int size = 1;
        while (size < studioLatency + kChunk + 16) size <<= 1;
        ringMask = size - 1;
        for (auto& r : rings) r.assign ((size_t) size, 0.0);
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            inBuf[(size_t) ch].assign (kChunk, 0.0);
            wetBuf[(size_t) ch].assign (kChunk, 0.0);
            psolaBuf[(size_t) ch].assign (kChunk, 0.0);
            studioIn[(size_t) ch].assign (kChunk, 0.0);
            dryBuf[(size_t) ch].assign (kChunk, 0.0);
        }
        mono.assign (kChunk, 0.0);
        doubleIn.assign (kChunk, 0.0);
        doubleOutL.assign (kChunk, 0.0);
        doubleOutR.assign (kChunk, 0.0);
        events.assign (kChunk + 4, Event {});
        graphLead = 0.001 * sampleRate;
        midiQueue.assign (kMidiQueue, QueuedMidi {});

        applyParameters (params, true);
        reset();
    }

    void reset() noexcept
    {
        for (auto& r : rings) std::fill (r.begin(), r.end(), 0.0);
        detector.reset();
        quantizer.reset();
        curve.reset();
        psola.reset();
        splice.reset();
        studio.reset();
        track->reset();
        throatModel.reset();
        resetDoubler();
        doublerAmount = 0.0;
        timelineCount = 0;
        graphFormantSmoothed = 0.0;
        midiHead = midiTail = 0;            // horloge remise à zéro : la file part vide ; les notes tenues restent tenues
        autoKey.reset(); keyAnalyseCountdown = 0.0;
        lastVoicedPos = -(std::int64_t) 1 << 40;
        lastPeriod = 0.0; lastFreq = 0.0; lastQ = ScaleQuantizer::Result {};
        psolaBlend = usePsola (params) ? 1.0 : 0.0;
        counter = 0;
        studioEpoch = 0;
        activeMode = pendingMode = params.mode;
        modeGain = 1.0;
        mixValue = params.mix;
        formantSemis = params.formantShift;
    }

    //--- paramètres (thread audio, début de bloc) ---------------------------------------------------------------------
    void setParameters (const AuraPitchParams& p) noexcept { applyParameters (p, false); }
    const AuraPitchParams& getParameters() const noexcept { return params; }

    int getLatencySamples() const noexcept                   { return getLatencySamples (pendingMode); }
    int getLatencySamples (TrackingMode m) const noexcept    { return m == TrackingMode::Live ? liveLatency : studioLatency; }
    TrackingMode getActiveMode() const noexcept              { return activeMode; }
    LockFreePitchFifo& getPitchFifo() noexcept               { return *fifo; }
    const PitchDetector& getDetector() const noexcept        { return detector; }
    StudioDxdShifter& getStudioShifter() noexcept            { return studio; }
    const ThroatModel& getThroatModel() const noexcept       { return throatModel; }

    //--- MIDI Target (thread audio, avant process()) ------------------------------------------------------------------
    /** Évènement à la position sampleOffset du prochain bloc passé à process(). */
    void addMidiEvent (const MidiTargetEvent& ev, int sampleOffset) noexcept
    {
        if (midiTail - midiHead >= kMidiQueue)               // file pleine : l'évènement le plus ancien s'applique tout de suite
            applyMidi (midiQueue[(size_t) (midiHead++ % kMidiQueue)].event);
        midiQueue[(size_t) (midiTail++ % kMidiQueue)] = QueuedMidi { counter + std::max (0, sampleOffset), ev };

        // État vu par l'interface (immédiat)
        auto bits = heldForUi;
        applyTo (ev, bits, nullptr);
        for (int w = 0; w < 2; ++w) uiHeld[(size_t) w].store (bits[(size_t) w], std::memory_order_relaxed);
        heldForUi = bits;
    }
    /** Interface : notes MIDI tenues (bit n = note n). */
    std::array<std::uint64_t, 2> getHeldMidiNotes() const noexcept
    {
        return { uiHeld[0].load (std::memory_order_relaxed), uiHeld[1].load (std::memory_order_relaxed) };
    }

    //--- GRAPH EDITOR (étape 5B) -------------------------------------------------------------------------------------
    /** Thread audio, avant process() : position de la timeline du DAW au début du bloc (s), lecture en cours. */
    void setTimeline (double seconds, bool playing) noexcept
    {
        auto& seg = timeline[(size_t) (timelineCount % kTimelineSegments)];
        seg = { counter, seconds, playing };
        ++timelineCount;
    }
    /** Message thread : nouvelles notes (remplace les précédentes ; un snapshot vide = aucune note). */
    void publishGraph (std::unique_ptr<GraphSnapshot> s) { graphExchange.publish (std::move (s)); }
    /** Message thread : libère les snapshots rendus par le thread audio. */
    void collectGraphGarbage() { graphExchange.collect(); }
    /** Capture des trames pendant la lecture (les notes ne sont pas rejouées pendant la capture). */
    void setCapture (bool on) noexcept { capturing.store (on, std::memory_order_release); }
    bool isCapturing() const noexcept  { return capturing.load (std::memory_order_acquire); }
    /** Message thread : trames capturées. */
    template <typename Fn> int drainCapture (Fn&& fn) { return captureFifo->drain (std::forward<Fn> (fn)); }

    //--- Auto-Key (commandes : thread interface ; analyse : thread audio) ------------------------------------------
    void startKeyDetection() noexcept { keyCommand.store (1, std::memory_order_release); }
    void stopKeyDetection() noexcept  { keyCommand.store (2, std::memory_order_release); }
    bool isKeyDetectionListening() const noexcept { return keyListening.load (std::memory_order_acquire); }
    AutoKeyDetector::Result getKeyDetection() const noexcept
    {
        AutoKeyDetector::Result r;
        const int k = keyResult.load (std::memory_order_acquire);
        r.key = k < 0 ? -1 : k % 12;
        r.minor = k >= 12;
        r.confidence = keyConfidence.load (std::memory_order_relaxed);
        r.seconds = keySeconds.load (std::memory_order_relaxed);
        return r;
    }

    //--- traitement (en place) ---------------------------------------------------------------------------------------
    template <typename SampleType>
    void process (SampleType* const* io, int numChannels, int numSamples) noexcept
    {
        const int nc = std::min (numChannels, channels);
        for (int pos = 0; pos < numSamples; pos += kChunk)
        {
            const int n = std::min (kChunk, numSamples - pos);
            std::array<SampleType*, kMaxChannels> ptr {};
            for (int ch = 0; ch < nc; ++ch) ptr[(size_t) ch] = io[ch] + pos;
            for (int ch = nc; ch < channels; ++ch) ptr[(size_t) ch] = io[nc - 1] + pos;   // mono -> stéréo
            processChunk (ptr.data(), n);
        }
    }

    static constexpr int    kGraphGrid = 32;           // Live : décalage recalculé tous les 32 échantillons sous une note
    static constexpr double kDoublerDelayLeft  = 0.0075, kDoublerDelayRight = 0.0125;   // retard moyen ≈ D + 0,6 T : 9 / 14 ms
    static constexpr double kDoublerCents      = 7.0;
    static constexpr double kBendRangeSemitones = 2.0;

private:
    struct Event { int index = 0; double period = 0.0, ratio = 1.0; bool voiced = false; };
    struct QueuedMidi { std::int64_t time = 0; MidiTargetEvent event; };
    static constexpr std::int64_t kMidiQueue = 512;

    static void applyTo (const MidiTargetEvent& ev, std::array<std::uint64_t, 2>& bits, double* bend) noexcept
    {
        const int n = std::clamp (ev.note, 0, 127);
        const std::uint64_t mask = (std::uint64_t) 1 << (n & 63);
        switch (ev.type)
        {
            case MidiTargetEvent::NoteOn:      bits[(size_t) (n >> 6)] |= mask; break;
            case MidiTargetEvent::NoteOff:     bits[(size_t) (n >> 6)] &= ~mask; break;
            case MidiTargetEvent::AllNotesOff: bits = { 0, 0 }; break;
            case MidiTargetEvent::PitchBend:   if (bend != nullptr) *bend = std::clamp (ev.bend, -1.0, 1.0) * kBendRangeSemitones; break;
        }
    }
    /** Instant de la timeline (s) de l'échantillon c (horloge interne). false hors lecture ou inconnu. */
    bool timelineAt (std::int64_t c, double& t) const noexcept
    {
        const auto avail = std::min<std::int64_t> (timelineCount, kTimelineSegments);
        for (std::int64_t j = 1; j <= avail; ++j)
        {
            const auto& seg = timeline[(size_t) ((timelineCount - j) % kTimelineSegments)];
            if (seg.counter <= c)
            {
                if (! seg.playing) return false;
                t = seg.seconds + (double) (c - seg.counter) / sampleRate;
                return true;
            }
        }
        return false;
    }
    bool graphAt (std::int64_t c, GraphSnapshot::Result& r) const noexcept
    {
        double t = 0.0;
        return graphOn && timelineAt (c, t) && graph->evaluate (t, r);
    }

    void applyMidi (const MidiTargetEvent& ev) noexcept { applyTo (ev, held, &bendSemis); }
    void consumeMidiUntil (std::int64_t t) noexcept
    {
        while (midiHead < midiTail && midiQueue[(size_t) (midiHead % kMidiQueue)].time <= t)
            applyMidi (midiQueue[(size_t) (midiHead++ % kMidiQueue)].event);
    }

    /** Live : PSOLA seulement pour les effets de formant et les grandes transpositions à formants préservés ;
        la correction de hauteur passe par la tête unique (aucun peigne, aucune voix fantôme). */
    static bool usePsola (const AuraPitchParams& p) noexcept
    {
        return std::abs (p.formantShift) > 0.01 || (p.formantPreserve && std::abs (p.transpose) >= 2.0);
    }

    void applyParameters (const AuraPitchParams& p, bool force) noexcept
    {
        if (force || p.key != params.key)                 quantizer.setKey (p.key);
        if (force || p.customNotes != params.customNotes) quantizer.setCustomStates (p.customNotes);
        if (force || p.scale != params.scale)             quantizer.setScale (p.scale);
        quantizer.setReferenceA4 (p.referenceA4);
        if (force || p.inputType != params.inputType)     detector.setInputType (p.inputType);
        curve.setRetuneMs (p.retuneMs);
        curve.setHumanize (p.humanize);
        curve.setTranspose (p.transpose);
        curve.setFineTuneCents (p.fineTuneCents);
        curve.setTransitionMs (p.transitionMs);
        throatModel.setRatio (p.throat);
        pendingMode = p.mode;
        params = p;
    }

    template <typename SampleType>
    void processChunk (SampleType* const* io, int n) noexcept
    {
        // 1. Entrée -> anneau (Dry, tap Studio) et somme mono
        for (int i = 0; i < n; ++i)
        {
            double m = 0.0;
            const size_t slot = (size_t) ((counter + i) & ringMask);
            for (int ch = 0; ch < channels; ++ch)
            {
                double v = (double) io[ch][i];
                if (! std::isfinite (v)) v = 0.0;
                inBuf[(size_t) ch][(size_t) i] = v;
                rings[(size_t) ch][slot] = v;
                m += v;
            }
            mono[(size_t) i] = m / channels;
        }

        // GRAPH : snapshot courant (valable jusqu'au prochain bloc), capture
        graph = graphExchange.acquire();
        const bool capture = capturing.load (std::memory_order_acquire);
        graphOn = graph != nullptr && ! graph->notes.empty() && ! capture;

        // Auto-Key : commandes de l'interface
        if (const int cmd = keyCommand.exchange (0, std::memory_order_acq_rel); cmd != 0)
        {
            if (cmd == 1) { autoKey.reset(); keyResult.store (-1); keyConfidence.store (0.0f); keySeconds.store (0.0f); }
            keyListening.store (cmd == 1, std::memory_order_release);
        }
        const bool listening = keyListening.load (std::memory_order_relaxed);

        // 2. Détection -> gamme -> courbe -> événements (Live), piste horodatée (Studio), FIFO (interface)
        int numEvents = 0;
        const double dt = (double) detector.getHopInputSamples() / sampleRate;
        const double formantCoeff = 1.0 - std::exp (-dt / 0.030);
        detector.process (mono.data(), n, [&] (const PitchEstimate& e, int index) noexcept
        {
            formantSemis += formantCoeff * (params.formantShift - formantSemis);

            // Maintien du voisement : une trame non voisée isolée au milieu d'une note (souffle, voix éraillée, fry)
            // ne fait plus basculer le moteur vers la voix d'origine — c'est ce va-et-vient, sur une vraie voix, qui
            // faisait entendre la note chantée et la note corrigée en même temps. 50 ms sans voisement = vraie
            // consonne ou silence.
            const std::int64_t now = counter + index;
            if (e.voiced) { lastVoicedPos = now; lastPeriod = e.periodSamples; lastFreq = e.frequencyHz; }
            const bool isHeld = e.voiced || now - lastVoicedPos < holdSamples;

            // MIDI Target : Live applique l'état MIDI de l'instant traité, Studio celui du centre de la trame analysée.
            consumeMidiUntil (activeMode == TrackingMode::Live ? now : e.frameCentre);
            if (listening && e.voiced)
                autoKey.addFrame (hzToMidi (e.frequencyHz, params.referenceA4), e.clarity, e.levelDb, dt);

            ScaleQuantizer::Result q;
            if (e.voiced)
            {
                q = params.midiTarget ? quantizer.quantizeToNotes (hzToMidi (e.frequencyHz, params.referenceA4), held, bendSemis)
                                      : quantizer.quantizeHz (e.frequencyHz);
                lastQ = q;
            }
            else if (isHeld)   { q = lastQ; }
            else             { quantizer.reset(); }
            const double shift = curve.process (q, isHeld, dt);
            const double ratio = isHeld ? std::exp2 (shift / 12.0) : 1.0;     // non voisé : rapport 1 (bypass)

            events[(size_t) numEvents++] = Event { index, isHeld ? lastPeriod : e.periodSamples, ratio, isHeld };

            // GRAPH : capture de la trame (datée au centre de sa fenêtre), puis note éditée à cet instant
            double frameTime = 0.0;
            const bool timed = timelineAt (e.frameCentre, frameTime);
            if (capture && timed)
                captureFifo->push ({ frameTime, e.voiced ? (float) hzToMidi (e.frequencyHz, params.referenceA4) : 0.0f, e.levelDb, e.voiced });
            GraphSnapshot::Result gr;
            const bool edited = graphOn && timed && graph->evaluate (frameTime, gr);
            graphFormantSmoothed += formantCoeff * ((edited ? gr.formantSemitones : 0.0) - graphFormantSmoothed);

            PitchControlTrack::Point pt;
            pt.time = e.frameCentre; pt.shiftSemitones = isHeld ? shift : 0.0; pt.formantSemitones = formantSemis + graphFormantSmoothed;
            pt.f0 = isHeld ? lastFreq : e.frequencyHz; pt.voiced = isHeld; pt.preserve = params.formantPreserve;
            if (edited)
            {
                pt.shiftSemitones = gr.shiftSemitones;
                pt.voiced = true;
                pt.f0 = params.referenceA4 * std::exp2 ((gr.inputMidi - 69.0) / 12.0);
            }
            track->push (pt);

            PitchFrame f;
            f.samplePosition = e.frameCentre;
            f.voiced   = e.voiced;
            f.clarity  = e.clarity;
            f.levelDb  = e.levelDb;
            if (e.voiced)
            {
                f.inputMidi      = (float) hzToMidi (e.frequencyHz, params.referenceA4);
                f.outputMidi     = (float) (f.inputMidi + shift);
                f.targetMidi     = (float) q.targetMidi;
                f.deviationCents = (float) q.deviationCents;
                if (edited)
                {
                    f.outputMidi = (float) (f.inputMidi + gr.shiftSemitones);
                    f.targetMidi = (float) std::round (gr.outputMidi);
                    f.deviationCents = (float) ((f.inputMidi - f.targetMidi) * 100.0);
                }
            }
            fifo->push (f);
        });

        if (listening)
        {
            keyAnalyseCountdown -= (double) n / sampleRate;
            if (keyAnalyseCountdown <= 0.0)
            {
                keyAnalyseCountdown = 0.1;
                const auto r = autoKey.analyse();
                keyResult.store (r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0), std::memory_order_release);
                keyConfidence.store ((float) r.confidence, std::memory_order_relaxed);
                keySeconds.store ((float) r.seconds, std::memory_order_relaxed);
            }
        }

        // 3. Wet
        if (activeMode == TrackingMode::Live)
        {
            // Correction : tête unique (SpliceShifter). Effets de formant / grandes transpositions avec Preserve : PSOLA.
            // Les deux tournent en permanence (historique et marques à jour) ; la bascule est un fondu de 10 ms.
            // Rendu seulement de ce qui s'entend : PSOLA hors fondu et hors effet est coupé, la tête unique aussi
            // quand PSOLA a entièrement la main (leur état avance quand même : la bascule reste un fondu propre).
            const bool wantPsola = usePsola (params) || (graphOn && graph->anyFormant);
            psola.setActive (wantPsola || psolaBlend > 0.0);
            splice.setRenderEnabled (! wantPsola || psolaBlend < 1.0);
            int cursor = 0;
            auto run = [&] (int end)
            {
                if (end <= cursor) return;
                std::array<const double*, kMaxChannels> in {};
                std::array<double*, kMaxChannels> outS {}, outP {};
                for (int ch = 0; ch < channels; ++ch)
                {
                    in[(size_t) ch]   = inBuf[(size_t) ch].data() + cursor;
                    outS[(size_t) ch] = wetBuf[(size_t) ch].data() + cursor;
                    outP[(size_t) ch] = psolaBuf[(size_t) ch].data() + cursor;
                }
                splice.process (in.data(), outS.data(), end - cursor);
                psola.process (in.data(), outP.data(), end - cursor);
                cursor = end;
            };
            // Évènements du détecteur, et sous une note éditée une grille de 32 échantillons : le décalage est calculé
            // pour l'instant d'entrée que la tête lit vraiment (position − retard de lecture + 1 ms d'avance pour le
            // lissage du rapport), d'après la hauteur capturée — sans le retard du détecteur.
            int k = 0;
            int nextGrid = graphOn ? 0 : n;
            while (k < numEvents || nextGrid < n)
            {
                const bool isEvent = k < numEvents && (nextGrid >= n || events[(size_t) k].index <= nextGrid);
                const int index = isEvent ? events[(size_t) k].index : nextGrid;
                run (index + 1);
                GraphSnapshot::Result gr;
                const auto readPos = counter + index - (std::int64_t) std::lround (splice.getCurrentOffset() - graphLead);
                if (graphAt (readPos, gr))
                {
                    const double period = sampleRate / (params.referenceA4 * std::exp2 ((gr.inputMidi - 69.0) / 12.0));
                    const double r = std::exp2 (gr.shiftSemitones / 12.0);
                    splice.setPitch (period, true);
                    splice.setRatio (r);
                    psola.setPitch (period, true);
                    psola.setShift (r, std::exp2 ((formantSemis + graphFormantSmoothed) / 12.0), params.formantPreserve);
                }
                else if (isEvent)
                {
                    const auto& ev = events[(size_t) k];
                    splice.setPitch (ev.period, ev.voiced);
                    splice.setRatio (ev.ratio);
                    psola.setPitch (ev.period, ev.voiced);
                    psola.setShift (ev.ratio, std::exp2 ((formantSemis + graphFormantSmoothed) / 12.0), params.formantPreserve);
                }
                if (isEvent) ++k; else nextGrid += kGraphGrid;
            }
            run (n);

            const double target = wantPsola ? 1.0 : 0.0;
            const double blendStep = 1.0 / (0.010 * sampleRate);
            for (int i = 0; i < n; ++i)
            {
                psolaBlend += std::clamp (target - psolaBlend, -blendStep, blendStep);
                if (psolaBlend <= 0.0) continue;
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto& w = wetBuf[(size_t) ch][(size_t) i];
                    w += psolaBlend * (psolaBuf[(size_t) ch][(size_t) i] - w);
                }
            }
        }
        else
        {
            std::array<const double*, kMaxChannels> in {};
            std::array<double*, kMaxChannels> out {};
            for (int ch = 0; ch < channels; ++ch)
            {
                for (int i = 0; i < n; ++i)
                    studioIn[(size_t) ch][(size_t) i] = rings[(size_t) ch][(size_t) ((counter + i - detectionDelay) & ringMask)];
                in[(size_t) ch]  = studioIn[(size_t) ch].data();
                out[(size_t) ch] = wetBuf[(size_t) ch].data();
            }
            const std::int64_t shiftToInput = studioEpoch - detectionDelay;
            studio.setUsePsola (usePsola (params) || (graphOn && graph->anyFormant));
            studio.process (in.data(), out.data(), n, [&] (std::int64_t centre, std::int64_t span) noexcept
            {
                return track->at (centre + shiftToInput, span);
            });
            // Wet = base retardée de L + Δ
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    wetBuf[(size_t) ch][(size_t) i] += rings[(size_t) ch][(size_t) ((counter + i - studioLatency) & ringMask)];
        }

        const int latency = getLatencySamples (activeMode);
        const bool throatOn = ! throatModel.isIdle();
        applyThroatAndDoubler (n, latency);
        if (throatOn)
        {
            // Dry par le même passe-tout que le Wet (Throat engagé) : Mix < 100 % sans peigne.
            std::array<double*, kMaxChannels> d {};
            for (int ch = 0; ch < channels; ++ch)
            {
                for (int i = 0; i < n; ++i)
                    dryBuf[(size_t) ch][(size_t) i] = rings[(size_t) ch][(size_t) ((counter + i - latency) & ringMask)];
                d[(size_t) ch] = dryBuf[(size_t) ch].data();
            }
            throatModel.processDry (d.data(), channels, n);
        }

        // 4. Mix aligné + fondu de changement de mode
        const double mixStep = 1.0 / (0.020 * sampleRate);
        const double modeStep = 1.0 / (0.005 * sampleRate);
        for (int i = 0; i < n; ++i)
        {
            mixValue += std::clamp (params.mix - mixValue, -mixStep, mixStep);
            if (pendingMode != activeMode) modeGain = std::max (0.0, modeGain - modeStep);
            else                           modeGain = std::min (1.0, modeGain + modeStep);

            const size_t drySlot = (size_t) ((counter + i - latency) & ringMask);
            for (int ch = 0; ch < channels; ++ch)
            {
                const double dry = throatOn ? dryBuf[(size_t) ch][(size_t) i] : rings[(size_t) ch][drySlot];
                const double wet = wetBuf[(size_t) ch][(size_t) i];
                writeBack (io, ch, i, (dry + mixValue * (wet - dry)) * modeGain);
            }
        }
        counter += n;

        if (pendingMode != activeMode && modeGain <= 0.0)
        {
            activeMode = pendingMode;
            psola.reset();
            splice.reset();
            studio.reset();
            throatModel.reset();
            doubleLeft.reset(); doubleRight.reset();
            studioEpoch = counter;
        }
    }

    /** Throat puis Doubler, sur le Wet. La commande (voisement, période de sortie) est lue sur la piste horodatée à
        l'instant d'entrée qui correspond au Wet (t − latence) : Live et Studio restent alignés sur la voix. */
    void applyThroatAndDoubler (int n, int latency) noexcept
    {
        const bool throatOn = ! throatModel.isIdle();
        const bool doublerWanted = channels == 2 && params.doubler > 0.0;
        if (! throatOn && ! doublerWanted && doublerAmount <= 0.0)
            return;

        if (doublerWanted && doublerAmount <= 0.0)          // reprise après repos : historique neuf
            resetDoubler();

        constexpr int kSub = 32;
        for (int start = 0; start < n; start += kSub)
        {
            const int len = std::min (kSub, n - start);
            const auto c = track->at (counter + start - latency, 0);
            const double outPeriod = c.voiced && c.f0 > 0.0 ? sampleRate / (c.f0 * c.ratio) : 0.0;

            std::array<double*, kMaxChannels> w {};
            for (int ch = 0; ch < channels; ++ch) w[(size_t) ch] = wetBuf[(size_t) ch].data() + start;
            if (throatOn)
            {
                throatModel.setVoiced (c.voiced);
                throatModel.process (w.data(), channels, len, start);
            }
            if (channels == 2 && (doublerWanted || doublerAmount > 0.0))
            {
                for (int i = 0; i < len; ++i) doubleIn[(size_t) i] = 0.5 * (w[0][i] + w[1][i]);
                renderDoubles (len, outPeriod);
                const double step = 1.0 / (0.020 * sampleRate);
                for (int i = 0; i < len; ++i)
                {
                    doublerAmount += std::clamp ((doublerWanted ? params.doubler : 0.0) - doublerAmount, -step, step);
                    // voix doublées à −3 dB au maximum, niveau global conservé (voix décorrélées : somme en puissance)
                    const double g = 0.7071 * doublerAmount;
                    const double norm = 1.0 / std::sqrt (1.0 + g * g);
                    w[0][i] = (w[0][i] + g * doubleOutL[(size_t) i]) * norm;
                    w[1][i] = (w[1][i] + g * doubleOutR[(size_t) i]) * norm;
                }
            }
        }
    }

    //--- Doubler : têtes à raccords à ≤ 96 kHz (les voix doublées n'ont pas besoin de l'air du DXD) ----------------------
    static constexpr int kDoublerMaxStages = 3;

    void prepareDoubler()
    {
        doublerStages = 0;
        while (sampleRate / (double) (1 << doublerStages) > 96500.0 && doublerStages < kDoublerMaxStages) ++doublerStages;
        doublerFactor = 1 << doublerStages;
        for (auto& d : dblDecim) d.prepare (9.0);
        for (auto& c : dblInterp) for (auto& st : c) st.prepare (9.0);

        // Retard de la chaîne ÷2^k -> ×2^k (+ file de 2^k échantillons), mesuré sur une impulsion, retiré des 9 / 14 ms.
        double chain = 0.0;
        if (doublerStages > 0)
        {
            std::vector<double> resp;
            resetDoubler();
            for (int i = 0; i < 4096; ++i)
            {
                double low = 0.0;
                if (decimate (i == 0 ? 1.0 : 0.0, low))
                {
                    std::array<double, 1 << kDoublerMaxStages> up {};
                    interpolate (0, low, up.data());
                    for (int k = 0; k < doublerFactor; ++k) resp.push_back (up[(size_t) k]);
                }
            }
            size_t peak = 0;
            for (size_t i = 1; i + 1 < resp.size(); ++i) if (std::abs (resp[i]) > std::abs (resp[peak])) peak = i;
            const double a = resp[peak > 0 ? peak - 1 : 0], b = resp[peak], c = resp[peak + 1];
            const double den = a - 2.0 * b + c;
            chain = (double) peak + (std::abs (den) > 1.0e-12 ? 0.5 * (a - c) / den : 0.0) + (double) doublerFactor - 1.0;
        }
        const double fsD = sampleRate / doublerFactor;
        doubleLeft.prepare (fsD, 1, std::max (0.002, kDoublerDelayLeft - chain / sampleRate));
        doubleRight.prepare (fsD, 1, std::max (0.002, kDoublerDelayRight - chain / sampleRate));
        dblFifo.assign ((size_t) (4 * doublerFactor + 8), 0.0);
        resetDoubler();
    }

    void resetDoubler() noexcept
    {
        doubleLeft.reset(); doubleRight.reset();
        for (auto& d : dblDecim) d.reset();
        for (auto& c : dblInterp) for (auto& st : c) st.reset();
        std::fill (dblFifo.begin(), dblFifo.end(), 0.0);
        dblFifoRead = 0;
        dblFifoWrite = doublerStages > 0 ? (std::int64_t) doublerFactor - 1 : 0;     // file amorcée : sortie régulière
    }

    bool decimate (double x, double& y) noexcept
    {
        y = x;
        for (int s = 0; s < doublerStages; ++s)
            if (! dblDecim[(size_t) s].push (y, y))
                return false;
        return true;
    }

    /** Une valeur basse cadence -> 2^k valeurs à la fréquence native (canal c). */
    void interpolate (int c, double u, double* out) noexcept
    {
        std::array<double, 1 << kDoublerMaxStages> a {}, b {};
        a[0] = u;
        int count = 1;
        for (int s = 0; s < doublerStages; ++s)
        {
            for (int i = 0; i < count; ++i)
                dblInterp[(size_t) c][(size_t) s].push (a[(size_t) i], b[(size_t) (2 * i)], b[(size_t) (2 * i + 1)]);
            count *= 2;
            a = b;
        }
        for (int i = 0; i < count; ++i) out[i] = a[(size_t) i];
    }

    /** doubleIn[0 … len) -> doubleOutL / doubleOutR. */
    void renderDoubles (int len, double outPeriod) noexcept
    {
        const double periodLow = outPeriod / doublerFactor;
        doubleLeft.setPitch (periodLow, outPeriod > 0.0);
        doubleRight.setPitch (periodLow, outPeriod > 0.0);
        doubleLeft.setRatio (std::exp2 (-kDoublerCents / 1200.0));
        doubleRight.setRatio (std::exp2 (kDoublerCents / 1200.0));
        if (doublerStages == 0)
        {
            const double* inPtr[1] = { doubleIn.data() };
            double* outL[1] = { doubleOutL.data() };
            double* outR[1] = { doubleOutR.data() };
            doubleLeft.process (inPtr, outL, len);
            doubleRight.process (inPtr, outR, len);
            return;
        }
        const auto size = (std::int64_t) dblFifo.size() / 2;
        for (int i = 0; i < len; ++i)
        {
            double low = 0.0;
            if (decimate (doubleIn[(size_t) i], low))
            {
                double l = 0.0, r = 0.0;
                const double* inPtr[1] = { &low };
                double* pl[1] = { &l };
                double* pr[1] = { &r };
                doubleLeft.process (inPtr, pl, 1);
                doubleRight.process (inPtr, pr, 1);
                std::array<double, 1 << kDoublerMaxStages> ul {}, ur {};
                interpolate (0, l, ul.data());
                interpolate (1, r, ur.data());
                for (int k = 0; k < doublerFactor; ++k)
                {
                    const auto slot = (size_t) ((dblFifoWrite + k) % size);
                    dblFifo[2 * slot] = ul[(size_t) k];
                    dblFifo[2 * slot + 1] = ur[(size_t) k];
                }
                dblFifoWrite += doublerFactor;
            }
            const auto slot = (size_t) (dblFifoRead % size);
            doubleOutL[(size_t) i] = dblFifo[2 * slot];
            doubleOutR[(size_t) i] = dblFifo[2 * slot + 1];
            ++dblFifoRead;
        }
    }

    template <typename SampleType>
    void writeBack (SampleType* const* io, int ch, int i, double v) noexcept
    {
        // Un canal dupliqué (hôte mono, moteur stéréo) partage le tampon du canal réel : on n'écrit que le premier.
        if (ch > 0 && io[ch] == io[ch - 1]) return;
        io[ch][i] = (SampleType) v;
    }

    double sampleRate = 48000.0;
    int channels = 1;
    int detectionDelay = 0, liveLatency = 0, studioLatency = 0;

    PitchDetector     detector;
    ScaleQuantizer    quantizer;
    CorrectionCurve   curve;
    PSOLAShifter      psola;
    SpliceShifter     splice;
    StudioDxdShifter  studio;
    // Les deux plus gros membres (≈ 160 ko) sont sur le tas, alloués à la construction : un moteur peut vivre sur la
    // pile d'un hôte ou d'un test sans risquer le débordement (pile de 1 Mo sous Windows).
    std::unique_ptr<PitchControlTrack> track = std::make_unique<PitchControlTrack>();
    std::unique_ptr<LockFreePitchFifo> fifo  = std::make_unique<LockFreePitchFifo>();

    AuraPitchParams params;
    TrackingMode activeMode = TrackingMode::Live, pendingMode = TrackingMode::Live;
    double modeGain = 1.0, mixValue = 1.0, formantSemis = 0.0;
    std::int64_t studioEpoch = 0;

    std::int64_t ringMask = 0, counter = 0;
    std::array<std::vector<double>, kMaxChannels> rings, inBuf, wetBuf, psolaBuf, studioIn, dryBuf;
    std::int64_t holdSamples = 2400, lastVoicedPos = 0;
    double lastPeriod = 0.0, lastFreq = 0.0, psolaBlend = 0.0;
    ScaleQuantizer::Result lastQ;
    std::vector<double> mono;
    std::vector<Event> events;

    // étape 5A
    ThroatModel throatModel;
    SpliceShifter doubleLeft, doubleRight;
    int doublerStages = 0, doublerFactor = 1;
    std::array<detail::HalfBandDecimatorT<12>, kDoublerMaxStages> dblDecim {};
    std::array<std::array<detail::HalfBandInterpolatorT<12>, kDoublerMaxStages>, 2> dblInterp {};
    std::vector<double> dblFifo;
    std::int64_t dblFifoRead = 0, dblFifoWrite = 0;
    std::vector<double> doubleIn, doubleOutL, doubleOutR;
    double doublerAmount = 0.0;

    std::vector<QueuedMidi> midiQueue;
    std::int64_t midiHead = 0, midiTail = 0;
    std::array<std::uint64_t, 2> held {}, heldForUi {};
    std::array<std::atomic<std::uint64_t>, 2> uiHeld {};
    double bendSemis = 0.0;

    // étape 5B
    struct TimelineSegment { std::int64_t counter = 0; double seconds = 0.0; bool playing = false; };
    static constexpr std::int64_t kTimelineSegments = 256;
    std::array<TimelineSegment, kTimelineSegments> timeline {};
    std::int64_t timelineCount = 0;
    GraphExchange graphExchange;
    const GraphSnapshot* graph = nullptr;
    bool graphOn = false;
    double graphLead = 48.0, graphFormantSmoothed = 0.0;
    std::atomic<bool> capturing { false };
    std::unique_ptr<CaptureFifo> captureFifo = std::make_unique<CaptureFifo>();

    AutoKeyDetector autoKey;
    double keyAnalyseCountdown = 0.0;
    std::atomic<int>   keyCommand { 0 }, keyResult { -1 };
    std::atomic<bool>  keyListening { false };
    std::atomic<float> keyConfidence { 0.0f }, keySeconds { 0.0f };
};

} // namespace aura::pitch

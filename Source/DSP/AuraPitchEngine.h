// AuraPitch — NextDSP · AuraSuite
// AuraPitchEngine.h : chef d'orchestre DSP (sans JUCE). PitchDetector -> ScaleQuantizer -> CorrectionCurve ->
// PSOLAShifter (Live) ou StudioDxdShifter (Studio/DXD) -> Mix aligné -> LockFreePitchFifo.
//
// Latences (échantillons à la fréquence native, jusqu'à 384 kHz)
//   * Live   : D = 2 ms (PSOLA). La hauteur appliquée est la dernière estimation valide dès sa sortie du détecteur
//              (aucun retard ajouté pour l'attendre). Dry = entrée retardée de D.
//   * Studio : L = Ldet + Lvoc. Ldet = retard du détecteur (retard de groupe + demi-fenêtre) pour l'InputType le plus
//              grave + un saut d'analyse : constant, si bien que changer d'InputType ne change pas la latence annoncée.
//              Le vocodeur lit chaque trame la commande horodatée à l'instant de son centre : la correction tombe à
//              l'échantillon près sur l'attaque. Dry = entrée retardée de L.
// Mix : sortie = Dry + Mix × (Wet − Dry). Dans les deux modes, un passage non voisé donne Wet = Dry exactement :
// Mix < 100 % ne crée ni peigne ni dédoublement sur les consonnes.
// Temps réel : prepare() alloue ; process() et setParameters() n'allouent rien et ne prennent aucun verrou.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "LockFreePitchFifo.h"
#include "PSOLAShifter.h"
#include "PitchDetector.h"
#include "ScaleQuantizer.h"
#include "StudioPhaseVocoder.h"

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
        studio.prepare (sampleRate, channels);

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
            studioIn[(size_t) ch].assign (kChunk, 0.0);
        }
        mono.assign (kChunk, 0.0);
        events.assign (kChunk + 4, Event {});

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
        studio.reset();
        track.reset();
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
    LockFreePitchFifo& getPitchFifo() noexcept               { return fifo; }
    const PitchDetector& getDetector() const noexcept        { return detector; }
    StudioDxdShifter& getStudioShifter() noexcept            { return studio; }

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

private:
    struct Event { int index = 0; double period = 0.0, ratio = 1.0; bool voiced = false; };

    void applyParameters (const AuraPitchParams& p, bool force) noexcept
    {
        if (force || p.key != params.key)                 quantizer.setKey (p.key);
        if (force || p.customNotes != params.customNotes) quantizer.setCustomStates (p.customNotes);
        if (force || p.scale != params.scale)             quantizer.setScale (p.scale);
        quantizer.setReferenceA4 (p.referenceA4);
        if (force || p.inputType != params.inputType)
        {
            detector.setInputType (p.inputType);
            const bool low = p.inputType == InputType::LowMale || p.inputType == InputType::Instrument;
            studio.setResolution (low ? StudioDxdShifter::Resolution::Fine : StudioDxdShifter::Resolution::Fast);
        }
        curve.setRetuneMs (p.retuneMs);
        curve.setHumanize (p.humanize);
        curve.setTranspose (p.transpose);
        curve.setFineTuneCents (p.fineTuneCents);
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

        // 2. Détection -> gamme -> courbe -> événements (Live), piste horodatée (Studio), FIFO (interface)
        int numEvents = 0;
        const double dt = (double) detector.getHopInputSamples() / sampleRate;
        const double formantCoeff = 1.0 - std::exp (-dt / 0.030);
        detector.process (mono.data(), n, [&] (const PitchEstimate& e, int index) noexcept
        {
            formantSemis += formantCoeff * (params.formantShift - formantSemis);
            ScaleQuantizer::Result q;
            if (e.voiced) q = quantizer.quantizeHz (e.frequencyHz);
            else          quantizer.reset();
            const double shift = curve.process (q, e.voiced, dt);

            events[(size_t) numEvents++] = Event { index, e.periodSamples, std::exp2 (shift / 12.0), e.voiced };

            PitchControlTrack::Point pt;
            pt.time = e.frameCentre; pt.shiftSemitones = shift; pt.formantSemitones = formantSemis;
            pt.f0 = e.frequencyHz; pt.voiced = e.voiced; pt.preserve = params.formantPreserve;
            track.push (pt);

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
            }
            fifo.push (f);
        });

        // 3. Wet
        if (activeMode == TrackingMode::Live)
        {
            int cursor = 0;
            auto run = [&] (int end)
            {
                if (end <= cursor) return;
                std::array<const double*, kMaxChannels> in {};
                std::array<double*, kMaxChannels> out {};
                for (int ch = 0; ch < channels; ++ch)
                {
                    in[(size_t) ch]  = inBuf[(size_t) ch].data() + cursor;
                    out[(size_t) ch] = wetBuf[(size_t) ch].data() + cursor;
                }
                psola.process (in.data(), out.data(), end - cursor);
                cursor = end;
            };
            for (int k = 0; k < numEvents; ++k)
            {
                const auto& ev = events[(size_t) k];
                run (ev.index + 1);
                psola.setPitch (ev.period, ev.voiced);
                psola.setShift (ev.ratio, std::exp2 (formantSemis / 12.0), params.formantPreserve);
            }
            run (n);
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
            studio.process (in.data(), out.data(), n, [&] (std::int64_t centre, std::int64_t span) noexcept
            {
                return track.at (centre + shiftToInput, span);
            });
            // Wet = base retardée de L + Δ
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    wetBuf[(size_t) ch][(size_t) i] += rings[(size_t) ch][(size_t) ((counter + i - studioLatency) & ringMask)];
        }

        // 4. Mix aligné + fondu de changement de mode
        const int latency = getLatencySamples (activeMode);
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
                const double dry = rings[(size_t) ch][drySlot];
                const double wet = wetBuf[(size_t) ch][(size_t) i];
                writeBack (io, ch, i, (dry + mixValue * (wet - dry)) * modeGain);
            }
        }
        counter += n;

        if (pendingMode != activeMode && modeGain <= 0.0)
        {
            activeMode = pendingMode;
            psola.reset();
            studio.reset();
            studioEpoch = counter;
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
    StudioDxdShifter  studio;
    PitchControlTrack track;
    LockFreePitchFifo fifo;

    AuraPitchParams params;
    TrackingMode activeMode = TrackingMode::Live, pendingMode = TrackingMode::Live;
    double modeGain = 1.0, mixValue = 1.0, formantSemis = 0.0;
    std::int64_t studioEpoch = 0;

    std::int64_t ringMask = 0, counter = 0;
    std::array<std::vector<double>, kMaxChannels> rings, inBuf, wetBuf, studioIn;
    std::vector<double> mono;
    std::vector<Event> events;
};

} // namespace aura::pitch

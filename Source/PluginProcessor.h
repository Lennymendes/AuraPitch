// AuraPitch — NextDSP · AuraSuite
// PluginProcessor.h : enveloppe JUCE 8 d'AuraPitchEngine (VST3).
//
//   * double précision native : processBlock (float) et processBlock (double) passent par processTemplated<T> ;
//     le moteur calcule toujours en 64 bits, seules les E/S changent de type ;
//   * mono -> mono, stéréo -> stéréo ; en stéréo, détection liée (somme mono, image conservée) ou double mono
//     (un moteur et un détecteur par canal) ;
//   * bypass sans clic : Mix lissé vers 0 sur 20 ms, Dry retardé de la latence (la PDC de l'hôte reste juste) ;
//   * latence : annoncée dans prepareToPlay(), puis tenue à jour par un timer du message thread (changement de
//     TrackingMode) avec setLatencySamples() + updateHostDisplay (latencyChanged) ;
//   * MIDI (étape 5A) : NoteOn / NoteOff / Pitch Bend / All Notes Off de tous les canaux, horodatés à l'échantillon,
//     transmis aux moteurs pour le mode MIDI Target ; Auto-Key piloté depuis l'interface ;
//   * GRAPH EDITOR (étape 5B) : position de l'AudioPlayHead transmise aux moteurs à chaque bloc ; CAPTURE (trames
//     -> NoteGraphModel, message thread) ; notes publiées sans verrou ; notes sauvegardées dans l'état de session.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <mutex>

#include "Parameters.h"

class AuraPitchProcessor final : public juce::AudioProcessor,
                                 private juce::Timer
{
public:
    AuraPitchProcessor();
    ~AuraPitchProcessor() override;

    //--- AudioProcessor -------------------------------------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    //--- interface ------------------------------------------------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }

    struct Status
    {
        double sampleRate      = 0.0;
        int    latencySamples  = 0;
        bool   doublePrecision = false;
        bool   studioActive    = false;
        bool   ultraActive     = false;    // qualité Ultra en service (réglage, ou export hors ligne)
        bool   offlineRender   = false;    // l'hôte rend hors temps réel (bounce / export) : Ultra engagé d'office
    };
    Status getStatus() const noexcept;

    /** Message thread : vide la FIFO de hauteur du moteur affiché (canal gauche en double mono). */
    template <typename Fn>
    int drainPitchFrames (Fn&& fn)
    {
        const bool dual = activeChannelMode.load() == (int) aura::params::ChannelMode::DualMono;
        auto& shown = dual ? *left : *linked;
        auto& hidden1 = dual ? *linked : *left;
        hidden1.getPitchFifo().drain ([] (const aura::pitch::PitchFrame&) {});
        right->getPitchFifo().drain ([] (const aura::pitch::PitchFrame&) {});
        return shown.getPitchFifo().drain (std::forward<Fn> (fn));
    }

    //--- étape 5A -------------------------------------------------------------------------------------------------
    /** Auto-Key (message thread) : écoute de la voix, puis lecture du résultat du moteur affiché. */
    void startKeyDetection();
    void stopKeyDetection();
    bool isKeyDetectionListening() const noexcept;
    aura::pitch::AutoKeyDetector::Result getKeyDetection() const noexcept;
    /** Applique Key + Scale (Major / Minor) aux paramètres, avec notification de l'hôte (undo / automation). */
    void applyKey (int key, bool minor);

    //--- GRAPH EDITOR (message thread) -------------------------------------------------------------------------------
    aura::pitch::NoteGraphModel& getGraphModel() noexcept { return graphModel; }
    /** CAPTURE : enregistre les passages lus (les notes ne sont pas rejouées pendant la capture). */
    void setGraphCapture (bool on);
    bool isGraphCapturing() const noexcept { return graphCapture.load(); }
    /** Range les trames capturées et segmente les passages complets (final : tout). true si les notes ont changé. */
    bool pumpGraph (bool final);
    /** À appeler après chaque édition du modèle : publie les notes vers le thread audio. */
    void graphEdited();
    /** Notes de la gamme courante (bit = classe de hauteur), pour l'aimantation et QUANTIZE ALL. */
    std::uint16_t currentScaleMask() const;
    /** Position de lecture vue au dernier bloc (s), transport, tempo. */
    double getPlayheadSeconds() const noexcept { return playheadSeconds.load(); }
    bool isTransportPlaying() const noexcept   { return transportPlaying.load(); }
    double getBpm() const noexcept             { return transportBpm.load(); }

    /** Lot 2 : échelle de l'interface (1 = 920 × 640), sauvegardée avec le projet (fil message seulement). */
    double editorScale = 1.0;

    /** Étape 6 : position de la fenêtre agrandie du GRAPH EDITOR, retrouvée à la réouverture (fil message seulement). */
    juce::Rectangle<int> graphWindowBounds;

    /** Notes MIDI tenues (bit n = note n), pour l'interface. */
    std::array<std::uint64_t, 2> getHeldMidiNotes() const noexcept;

    /** Latence (échantillons) d'un mode, à la fréquence préparée. */
    int latencyFor (aura::pitch::TrackingMode mode) const noexcept;

    /** Message thread : annonce à l'hôte la latence du TrackingMode choisi (appelée par le timer à 20 Hz). */
    void syncLatency();

private:
    template <typename T> void processTemplated (juce::AudioBuffer<T>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept;
    const aura::pitch::AuraPitchEngine& shownEngine() const noexcept;
    void timerCallback() override;

    juce::AudioProcessorValueTreeState state;
    aura::params::Snapshot snapshot;

    std::unique_ptr<aura::pitch::AuraPitchEngine> linked, left, right;
    std::atomic<bool>   prepared { false };
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<int>    activeChannelMode { 0 };
    std::atomic<bool>   studioActive { false }, ultraActive { false }, offlineRender { false };
    int    pendingChannelMode = 0;
    int    preparedChannels = 2;
    double transitionGain = 1.0;
    int    currentProgram = 0;

    aura::pitch::NoteGraphModel graphModel;
    std::atomic<bool> graphCapture { false }, transportPlaying { false };
    std::atomic<double> playheadSeconds { 0.0 }, transportBpm { 120.0 };
    std::vector<aura::pitch::CaptureFrame> captureBatch;
    // Copie sérialisée des notes, tenue à jour par le message thread : getStateInformation() peut être appelé
    // depuis un autre thread par certains hôtes, il ne lit jamais le modèle en cours d'édition.
    std::mutex graphStateLock;
    std::vector<std::uint8_t> graphStateBytes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraPitchProcessor)
};

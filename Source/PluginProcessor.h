// AuraPitch — NextDSP · AuraSuite
// PluginProcessor.h : enveloppe JUCE 8 d'AuraPitchEngine (VST3 / AU / Standalone).
//
//   * double précision native : processBlock (float) et processBlock (double) passent par processTemplated<T> ;
//     le moteur calcule toujours en 64 bits, seules les E/S changent de type ;
//   * mono -> mono, stéréo -> stéréo ; en stéréo, détection liée (somme mono, image conservée) ou double mono
//     (un moteur et un détecteur par canal) ;
//   * bypass sans clic : Mix lissé vers 0 sur 20 ms, Dry retardé de la latence (la PDC de l'hôte reste juste) ;
//   * latence : annoncée dans prepareToPlay(), puis tenue à jour par un timer du message thread (changement de
//     TrackingMode) avec setLatencySamples() + updateHostDisplay (latencyChanged).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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
    bool acceptsMidi() const override  { return false; }
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

    /** Latence (échantillons) d'un mode, à la fréquence préparée. */
    int latencyFor (aura::pitch::TrackingMode mode) const noexcept;

    /** Message thread : annonce à l'hôte la latence du TrackingMode choisi (appelée par le timer à 20 Hz). */
    void syncLatency();

private:
    template <typename T> void processTemplated (juce::AudioBuffer<T>& buffer, bool forceBypass) noexcept;
    void timerCallback() override;

    juce::AudioProcessorValueTreeState state;
    aura::params::Snapshot snapshot;

    std::unique_ptr<aura::pitch::AuraPitchEngine> linked, left, right;
    std::atomic<bool>   prepared { false };
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<int>    activeChannelMode { 0 };
    std::atomic<bool>   studioActive { false };
    int    pendingChannelMode = 0;
    int    preparedChannels = 2;
    double transitionGain = 1.0;
    int    currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraPitchProcessor)
};

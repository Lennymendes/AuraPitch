// AuraPitch — NextDSP · AuraSuite
// Parameters.h : disposition APVTS (identifiants figés), lecture temps réel vers AuraPitchParams, presets d'usine.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/AuraPitchEngine.h"

namespace aura::params
{
    // Identifiants figés : ne jamais les renommer (sessions et automations des utilisateurs).
    inline constexpr const char* inputType       = "input_type";
    inline constexpr const char* key             = "key";
    inline constexpr const char* scale           = "scale";
    inline constexpr const char* retuneSpeed     = "retune_speed";
    inline constexpr const char* humanize        = "humanize";
    inline constexpr const char* formantShift    = "formant_shift";
    inline constexpr const char* formantPreserve = "formant_preserve";
    inline constexpr const char* transpose       = "transpose";
    inline constexpr const char* fineTune        = "fine_tune";
    inline constexpr const char* referenceA4     = "reference_a4";
    inline constexpr const char* trackingMode    = "tracking_mode";
    inline constexpr const char* mix             = "mix";
    inline constexpr const char* channelMode     = "channel_mode";
    inline constexpr const char* bypass          = "bypass";

    juce::String noteId (int pitchClass);               // "note_0" … "note_11"

    const juce::StringArray& noteNames();               // C, C#, D, …
    const juce::StringArray& inputTypeNames();
    const juce::StringArray& scaleNames();
    const juce::StringArray& noteStateNames();          // Active, Bypass, Removed
    const juce::StringArray& trackingModeNames();
    const juce::StringArray& channelModeNames();        // Linked Stereo, Dual Mono

    enum class ChannelMode : int { Linked = 0, DualMono };

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    /** Pointeurs vers les valeurs atomiques de l'APVTS : lecture sans verrou sur le thread audio. */
    class Snapshot
    {
    public:
        void attach (juce::AudioProcessorValueTreeState& state);

        /** Lit toutes les valeurs (thread audio, sans allocation). */
        pitch::AuraPitchParams read() const noexcept;
        ChannelMode channelModeValue() const noexcept;
        bool bypassed() const noexcept;

    private:
        std::atomic<float>* p (juce::AudioProcessorValueTreeState& s, const juce::String& id);

        std::atomic<float>* inputTypeV = nullptr;
        std::atomic<float>* keyV = nullptr;
        std::atomic<float>* scaleV = nullptr;
        std::atomic<float>* retuneV = nullptr;
        std::atomic<float>* humanizeV = nullptr;
        std::atomic<float>* formantV = nullptr;
        std::atomic<float>* preserveV = nullptr;
        std::atomic<float>* transposeV = nullptr;
        std::atomic<float>* fineV = nullptr;
        std::atomic<float>* a4V = nullptr;
        std::atomic<float>* modeV = nullptr;
        std::atomic<float>* mixV = nullptr;
        std::atomic<float>* channelV = nullptr;
        std::atomic<float>* bypassV = nullptr;
        std::array<std::atomic<float>*, 12> notesV {};
    };

    //--- presets d'usine ------------------------------------------------------------------------------------------
    struct FactoryPreset
    {
        const char* name;
        std::vector<std::pair<const char*, float>> values;   // valeurs réelles (pas normalisées)
    };

    const std::vector<FactoryPreset>& factoryPresets();

    /** Applique un preset (message thread) : chaque paramètre est notifié à l'hôte (undo / automation). Les notes du
        clavier Custom repassent à Active, les paramètres absents du preset reprennent leur valeur par défaut. */
    void applyPreset (juce::AudioProcessorValueTreeState& state, const FactoryPreset& preset);
}

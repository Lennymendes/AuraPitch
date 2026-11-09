// AuraKey — NextDSP · AuraSuite
// PluginProcessor.h : analyseur de tonalité VST3 (AuraKey).
//
//   * Flux live : le signal traverse le plug-in sans aucune modification (sortie = entrée, au bit près, 32 ou 64
//     bits) ; LiveKeyAnalyzer en tire la tonalité sur le thread audio, sans allocation ni verrou.
//   * Import de fichier : analyseFile() lance un fil de travail (priorité basse) qui décode le fichier par blocs de
//     64 k échantillons (WAV, AIFF, FLAC, MP3, Ogg ; jusqu'au DXD 384 kHz) et l'analyse hors ligne (clé, tempo, accord,
//     sections). Le thread audio ne partage rien avec lui. Un nouveau fichier annule l'analyse en cours.
//   * L'analyse du dernier fichier est sauvegardée avec le projet (état du plug-in), pas le fichier lui-même.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/LiveKeyAnalyzer.h"
#include "DSP/OfflineKeyAnalyzer.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace akparams
{
    inline const juce::String memory { "memory" };
    inline const juce::StringArray memoryLabels { "8 s", "30 s", juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e")) };
    inline constexpr double memorySeconds[] = { 8.0, 30.0, 0.0 };
}

/** Résultat de l'analyse d'un fichier, avec ses caractéristiques. Immuable une fois publié. */
struct FileAnalysis
{
    juce::String fileName, filePath, formatName;
    double sampleRate = 0.0;
    int bitsPerSample = 0, numChannels = 0;
    bool floatingPoint = false;
    double analysisSeconds = 0.0;           // durée du décodage + analyse
    aura::key::KeyAnalysisResult result;

    juce::ValueTree toValueTree() const;
    static std::shared_ptr<const FileAnalysis> fromValueTree (const juce::ValueTree&);
};

class AuraKeyProcessor final : public juce::AudioProcessor
{
public:
    enum class FileState { Idle, Running, Done, Failed, Cancelled };

    AuraKeyProcessor();
    ~AuraKeyProcessor() override;

    //--- AudioProcessor ------------------------------------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AuraKey"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //--- Analyse live (lecture depuis n'importe quel fil) --------------------------------------------------------
    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    bool getLiveState (aura::key::LiveKeyState& s) const noexcept { return live.getState (s); }
    void resetLive() noexcept { live.requestReset(); }
    double getCurrentSampleRate() const noexcept { return currentRate.load(); }
    bool isDoublePrecisionActive() const noexcept { return lastDouble.load(); }

    //--- Fichiers (fil message) ----------------------------------------------------------------------------------
    /** Extensions acceptées (« *.wav;*.aiff;… »). */
    juce::String getSupportedWildcard() const;
    bool isSupportedFile (const juce::File&) const;
    /** Lance l'analyse (annule la précédente). false si le fichier n'existe pas. */
    bool analyseFile (const juce::File&);
    void cancelFileAnalysis();
    FileState getFileState() const noexcept { return fileState.load(); }
    float getFileProgress() const noexcept { return fileProgress.load(); }
    juce::String getFileMessage() const;
    juce::String getPendingFileName() const;
    std::shared_ptr<const FileAnalysis> getFileAnalysis() const;
    /** Attend la fin du fil d'analyse (bancs d'essai). */
    bool waitForFileAnalysis (int timeoutMs);

private:
    class AnalysisJob;
    template <typename Sample> void process (juce::AudioBuffer<Sample>&);
    void jobFinished (AnalysisJob*, std::shared_ptr<const FileAnalysis>, FileState, const juce::String& message);
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState state;
    std::atomic<float>* memoryParam = nullptr;
    aura::key::LiveKeyAnalyzer live;
    std::atomic<double> currentRate { 0.0 };
    std::atomic<bool> lastDouble { false };
    int lastMemoryIndex = -1;

    juce::AudioFormatManager formats;       // fil message : types reconnus ; chaque analyse a le sien
    std::unique_ptr<AnalysisJob> job;
    std::atomic<FileState> fileState { FileState::Idle };
    std::atomic<float> fileProgress { 0.0f };
    mutable std::mutex fileLock;            // résultat et messages : fil message <-> fil d'analyse (jamais l'audio)
    std::shared_ptr<const FileAnalysis> fileAnalysis;
    juce::String fileMessage, pendingName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraKeyProcessor)
};

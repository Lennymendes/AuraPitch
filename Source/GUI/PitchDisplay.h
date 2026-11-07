// AuraPitch — NextDSP · AuraSuite
// PitchDisplay.h : graphe de hauteur défilant (entrée vs corrigée sur grille de notes) et accordeur en arc.
// Alimentés par l'éditeur (timer 60 Hz) à partir de la LockFreePitchFifo ; rien ici ne touche au thread audio.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../DSP/LockFreePitchFifo.h"
#include "NextDSPLookAndFeel.h"

namespace nextdsp
{
juce::String midiNoteName (int midiNote);

class PitchGraph final : public juce::Component
{
public:
    PitchGraph();

    void push (const aura::pitch::PitchFrame& f, double sampleRate);
    void setScale (std::uint16_t activeMask, int tonic) { mask = activeMask; key = tonic; }
    void tick (double dtSeconds);            // défilement et cadrage, une fois par image

    void paint (juce::Graphics&) override;

private:
    struct Sample { double t; float in, out, target; bool voiced; };

    juce::Rectangle<float> plotArea() const;
    float yFor (float midi, juce::Rectangle<float> area) const;

    std::vector<Sample> points;           // anneau
    size_t head = 0, count = 0;
    double latestTime = 0.0, displayTime = 0.0;
    double windowSeconds = 4.0;
    float viewCentre = 57.0f, wantedCentre = 57.0f;
    std::uint16_t mask = 0x0fff;
    int key = 0;
};

class TunerArc final : public juce::Component
{
public:
    void push (const aura::pitch::PitchFrame& f, double referenceA4);
    void tick (double dtSeconds);
    void paint (juce::Graphics&) override;

private:
    float shownIn = 0.0f, shownOut = 0.0f, wantedIn = 0.0f, wantedOut = 0.0f;
    float inputMidi = 0.0f;
    int   target = -1;
    double inputHz = 0.0, silence = 1.0, activity = 0.0;
};
}

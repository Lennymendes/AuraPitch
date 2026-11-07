// AuraPitch — NextDSP · AuraSuite
// PluginEditor.h : interface NextDSP (920 × 600 par défaut, redimensionnable).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../PluginProcessor.h"
#include "Controls.h"
#include "NextDSPLookAndFeel.h"
#include "PitchDisplay.h"
#include "ScaleKeyboard.h"

class AuraPitchEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit AuraPitchEditor (AuraPitchProcessor&);
    ~AuraPitchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Pour le banc d'essai : une image de l'interface (vide la FIFO et avance l'animation d'un pas). */
    void pullFramesForTest (double dtSeconds) { update (dtSeconds); }

private:
    void timerCallback() override;
    void update (double dtSeconds);
    void refreshStatus();

    AuraPitchProcessor& pitchProcessor;
    nextdsp::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    nextdsp::PitchGraph graph;
    nextdsp::TunerArc tuner;
    nextdsp::ScaleKeyboard keyboard;

    juce::ComboBox presetBox, keyBox, scaleBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> keyAttachment, scaleAttachment;

    nextdsp::SegmentedControl inputType, trackingMode, channelMode;
    nextdsp::Knob retune, humanize, formant, transpose, fine, a4, mix;
    juce::ToggleButton preserve { "PRESERVE" };
    juce::TextButton bypass { "BYPASS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> preserveAttachment, bypassAttachment;

    juce::Rectangle<int> headerArea, scalePanel, controlPanel, statusArea;
    juce::String rateText, precisionText, latencyText;
    bool dxd = false, studio = false;
    double lastTick = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuraPitchEditor)
};

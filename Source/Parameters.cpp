// AuraPitch — NextDSP · AuraSuite
#include "Parameters.h"

namespace aura::params
{
using APVTS = juce::AudioProcessorValueTreeState;

juce::String noteId (int pitchClass) { return "note_" + juce::String (pitchClass); }

const juce::StringArray& noteNames()
{
    static const juce::StringArray n { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n;
}
const juce::StringArray& inputTypeNames()
{
    static const juce::StringArray n { "Soprano", "Alto/Tenor", "Low Male", "Instrument" };
    return n;
}
const juce::StringArray& scaleNames()
{
    static const juce::StringArray n { "Chromatic", "Major", "Minor", "Pentatonic", "Custom" };
    return n;
}
const juce::StringArray& noteStateNames()
{
    static const juce::StringArray n { "Active", "Bypass", "Removed" };
    return n;
}
const juce::StringArray& trackingModeNames()
{
    static const juce::StringArray n { "Live 2 ms", "Studio DXD" };
    return n;
}
const juce::StringArray& channelModeNames()
{
    static const juce::StringArray n { "Linked Stereo", "Dual Mono" };
    return n;
}

namespace
{
    juce::String signedText (float v, int decimals, const char* unit)
    {
        const auto t = juce::String (std::abs (v), decimals);
        return (v > 0.0f ? "+" : v < 0.0f ? "-" : "") + t + " " + unit;
    }

    juce::NormalisableRange<float> retuneRange()
    {
        // 401^x − 1 : la moitié de la course couvre 0 … 19 ms (réglages rapides, hard-tune).
        return { 0.0f, 400.0f,
                 [] (float, float, float x)  { return (float) pitch::CorrectionCurve::retuneMsFromNormalised (x); },
                 [] (float, float, float ms) { return (float) pitch::CorrectionCurve::normalisedFromRetuneMs (ms); },
                 [] (float, float, float ms) { return std::round (ms * 10.0f) / 10.0f; } };
    }
}

APVTS::ParameterLayout createLayout()
{
    using namespace juce;
    APVTS::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { inputType, 1 }, "Input Type", inputTypeNames(), 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { key, 1 }, "Key", noteNames(), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { scale, 1 }, "Scale", scaleNames(), 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { retuneSpeed, 1 }, "Retune Speed", retuneRange(), 20.0f,
        AudioParameterFloatAttributes().withLabel ("ms")
            .withStringFromValueFunction ([] (float v, int) { return v < 10.0f ? String (v, 1) + " ms" : String (juce::roundToInt (v)) + " ms"; })
            .withValueFromStringFunction ([] (const String& t) { return t.getFloatValue(); })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { humanize, 1 }, "Humanize", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v)) + " %"; })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { formantShift, 1 }, "Formant Shift", NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("st")
            .withStringFromValueFunction ([] (float v, int) { return signedText (v, 2, "st"); })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { formantPreserve, 1 }, "Formant Preserve", true));

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { transpose, 1 }, "Transpose", -24, 24, 0,
        AudioParameterIntAttributes().withLabel ("st")
            .withStringFromValueFunction ([] (int v, int) { return signedText ((float) v, 0, "st"); })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { fineTune, 1 }, "Fine Tune", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("cents")
            .withStringFromValueFunction ([] (float v, int) { return signedText (v, 1, "ct"); })));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { referenceA4, 1 }, "A4 Reference", NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " Hz"; })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { trackingMode, 1 }, "Tracking Mode", trackingModeNames(), 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { mix, 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v)) + " %"; })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { channelMode, 1 }, "Channel Mode", channelModeNames(), 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { bypass, 1 }, "Bypass", false));

    for (int pc = 0; pc < 12; ++pc)
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { noteId (pc), 1 }, "Note " + noteNames()[pc],
                                                            noteStateNames(), 0));
    return layout;
}

//==============================================================================================================
std::atomic<float>* Snapshot::p (APVTS& s, const juce::String& id)
{
    auto* v = s.getRawParameterValue (id);
    jassert (v != nullptr);
    return v;
}

void Snapshot::attach (APVTS& s)
{
    inputTypeV = p (s, inputType);   keyV = p (s, key);             scaleV = p (s, scale);
    retuneV    = p (s, retuneSpeed); humanizeV = p (s, humanize);   formantV = p (s, formantShift);
    preserveV  = p (s, formantPreserve); transposeV = p (s, transpose); fineV = p (s, fineTune);
    a4V        = p (s, referenceA4); modeV = p (s, trackingMode);   mixV = p (s, mix);
    channelV   = p (s, channelMode); bypassV = p (s, bypass);
    for (int pc = 0; pc < 12; ++pc)
        notesV[(size_t) pc] = p (s, noteId (pc));
}

pitch::AuraPitchParams Snapshot::read() const noexcept
{
    using namespace pitch;
    auto idx = [] (const std::atomic<float>* v) { return (int) std::lround (v->load (std::memory_order_relaxed)); };
    auto val = [] (const std::atomic<float>* v) { return (double) v->load (std::memory_order_relaxed); };

    AuraPitchParams r;
    r.inputType       = (InputType) std::clamp (idx (inputTypeV), 0, 3);
    r.key             = std::clamp (idx (keyV), 0, 11);
    r.scale           = (ScaleType) std::clamp (idx (scaleV), 0, 4);
    r.retuneMs        = val (retuneV);
    r.humanize        = val (humanizeV) / 100.0;
    r.formantShift    = val (formantV);
    r.formantPreserve = val (preserveV) > 0.5;
    r.transpose       = val (transposeV);
    r.fineTuneCents   = val (fineV);
    r.referenceA4     = val (a4V);
    r.mode            = idx (modeV) == 1 ? TrackingMode::Studio : TrackingMode::Live;
    r.mix             = bypassed() ? 0.0 : val (mixV) / 100.0;
    for (int pc = 0; pc < 12; ++pc)
        r.customNotes[(size_t) pc] = (NoteState) std::clamp (idx (notesV[(size_t) pc]), 0, 2);
    return r;
}

ChannelMode Snapshot::channelModeValue() const noexcept
{
    return channelV->load (std::memory_order_relaxed) > 0.5f ? ChannelMode::DualMono : ChannelMode::Linked;
}

bool Snapshot::bypassed() const noexcept { return bypassV->load (std::memory_order_relaxed) > 0.5f; }

//==============================================================================================================
const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> presets
    {
        { "Natural Vocal",
          { { inputType, 1.0f }, { scale, 0.0f }, { retuneSpeed, 45.0f }, { humanize, 40.0f }, { formantPreserve, 1.0f },
            { trackingMode, 0.0f }, { mix, 100.0f } } },
        { "0ms Hard-Tune Urban",
          { { inputType, 1.0f }, { key, 9.0f }, { scale, 2.0f }, { retuneSpeed, 0.0f }, { humanize, 0.0f }, { formantPreserve, 1.0f },
            { trackingMode, 0.0f }, { mix, 100.0f } } },
        { "DXD Mastering Surgical",
          { { inputType, 1.0f }, { scale, 0.0f }, { retuneSpeed, 25.0f }, { humanize, 30.0f }, { formantPreserve, 1.0f },
            { trackingMode, 1.0f }, { mix, 100.0f } } },
        { "Deep Octave Formant",
          { { inputType, 1.0f }, { scale, 0.0f }, { retuneSpeed, 15.0f }, { humanize, 20.0f }, { transpose, -12.0f },
            { formantShift, -3.0f }, { formantPreserve, 1.0f }, { trackingMode, 0.0f }, { mix, 50.0f } } },
    };
    return presets;
}

void applyPreset (APVTS& state, const FactoryPreset& preset)
{
    for (auto* base : state.processor.getParameters())
    {
        auto* param = dynamic_cast<juce::RangedAudioParameter*> (base);
        if (param == nullptr || param->getParameterID() == bypass)
            continue;

        float target = param->getDefaultValue();                  // normalisée
        for (const auto& [id, value] : preset.values)
            if (param->getParameterID() == id)
                target = param->convertTo0to1 (value);

        param->beginChangeGesture();
        param->setValueNotifyingHost (target);
        param->endChangeGesture();
    }
}
}

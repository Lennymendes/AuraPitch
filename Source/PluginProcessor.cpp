// AuraPitch — NextDSP · AuraSuite
#include "PluginProcessor.h"
#include "GUI/PluginEditor.h"

using namespace aura;

AuraPitchProcessor::AuraPitchProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "AuraPitch", params::createLayout()),
      linked (std::make_unique<pitch::AuraPitchEngine>()),
      left (std::make_unique<pitch::AuraPitchEngine>()),
      right (std::make_unique<pitch::AuraPitchEngine>())
{
    snapshot.attach (state);
    startTimerHz (20);
}

AuraPitchProcessor::~AuraPitchProcessor()
{
    stopTimer();
}

//==============================================================================================================
bool AuraPitchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void AuraPitchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);       // le moteur découpe en tranches de 1024 : toute taille de bloc convient
    prepared = false;

    preparedChannels = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    linked->prepare (sampleRate, preparedChannels);
    left->prepare (sampleRate, 1);
    right->prepare (sampleRate, 1);

    const auto p = snapshot.read();
    for (auto* e : { linked.get(), left.get(), right.get() })
    {
        e->setParameters (p);
        e->reset();
    }

    const int channelMode = preparedChannels == 2 ? (int) snapshot.channelModeValue() : 0;
    activeChannelMode = channelMode;
    pendingChannelMode = channelMode;
    transitionGain = 1.0;
    preparedRate = sampleRate;
    studioActive = p.mode == pitch::TrackingMode::Studio;
    prepared = true;

    setLatencySamples (latencyFor (p.mode));
}

void AuraPitchProcessor::releaseResources() {}

int AuraPitchProcessor::latencyFor (pitch::TrackingMode mode) const noexcept
{
    return prepared.load() ? linked->getLatencySamples (mode) : 0;
}

//==============================================================================================================
void AuraPitchProcessor::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m)          { processTemplated (b, m, false); }
void AuraPitchProcessor::processBlock (juce::AudioBuffer<double>& b, juce::MidiBuffer& m)         { processTemplated (b, m, false); }
void AuraPitchProcessor::processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer& m)  { processTemplated (b, m, true); }
void AuraPitchProcessor::processBlockBypassed (juce::AudioBuffer<double>& b, juce::MidiBuffer& m) { processTemplated (b, m, true); }

template <typename T>
void AuraPitchProcessor::processTemplated (juce::AudioBuffer<T>& buffer, juce::MidiBuffer& midi, bool forceBypass) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, numSamples);

    if (! prepared.load() || numSamples == 0)
        return;

    const int nc = juce::jlimit (1, preparedChannels, juce::jmin (numIn, numOut, buffer.getNumChannels()));
    auto p = snapshot.read();
    if (forceBypass)
    {
        p.mix = 0.0; p.throat = 1.0; p.doubler = 0.0;
    }

    // MIDI -> moteurs (horodaté dans ce bloc). Tous les canaux MIDI ; NoteOn de vélocité 0 = NoteOff.
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        pitch::MidiTargetEvent ev;
        if (msg.isNoteOn())                                         ev = { pitch::MidiTargetEvent::NoteOn, msg.getNoteNumber(), 0.0 };
        else if (msg.isNoteOff (true))                              ev = { pitch::MidiTargetEvent::NoteOff, msg.getNoteNumber(), 0.0 };
        else if (msg.isPitchWheel())                                ev = { pitch::MidiTargetEvent::PitchBend, 0, (msg.getPitchWheelValue() - 8192) / 8192.0 };
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())        ev = { pitch::MidiTargetEvent::AllNotesOff, 0, 0.0 };
        else continue;
        const int offset = juce::jlimit (0, juce::jmax (0, numSamples - 1), meta.samplePosition);
        for (auto* e : { linked.get(), left.get(), right.get() })
            e->addMidiEvent (ev, offset);
    }

    linked->setParameters (p);
    left->setParameters (p);
    right->setParameters (p);

    // Changement Lié <-> Double mono : fondu de sortie de 5 ms, bascule, remise à zéro des moteurs entrants.
    pendingChannelMode = nc == 2 ? (int) snapshot.channelModeValue() : 0;
    auto io = buffer.getArrayOfWritePointers();
    const int active = activeChannelMode.load();

    if (active == (int) params::ChannelMode::DualMono && nc == 2)
    {
        T* l[1] = { io[0] };
        T* r[1] = { io[1] };
        left->process (l, 1, numSamples);
        right->process (r, 1, numSamples);
    }
    else
    {
        linked->process (io, nc, numSamples);
    }

    if (pendingChannelMode != active || transitionGain < 1.0)
    {
        const double step = 1.0 / (0.005 * preparedRate.load());
        for (int i = 0; i < numSamples; ++i)
        {
            transitionGain = pendingChannelMode != active ? juce::jmax (0.0, transitionGain - step)
                                                          : juce::jmin (1.0, transitionGain + step);
            for (int ch = 0; ch < nc; ++ch)
                io[ch][i] = (T) (io[ch][i] * transitionGain);
        }
        if (pendingChannelMode != active && transitionGain <= 0.0)
        {
            if (pendingChannelMode == (int) params::ChannelMode::DualMono) { left->reset(); right->reset(); }
            else                                                            { linked->reset(); }
            activeChannelMode = pendingChannelMode;
        }
    }

    const auto& shown = activeChannelMode.load() == (int) params::ChannelMode::DualMono ? *left : *linked;
    studioActive = shown.getActiveMode() == pitch::TrackingMode::Studio;
}

//==============================================================================================================
const pitch::AuraPitchEngine& AuraPitchProcessor::shownEngine() const noexcept
{
    return activeChannelMode.load() == (int) params::ChannelMode::DualMono ? *left : *linked;
}

void AuraPitchProcessor::startKeyDetection()
{
    for (auto* e : { linked.get(), left.get(), right.get() }) e->startKeyDetection();
}

void AuraPitchProcessor::stopKeyDetection()
{
    for (auto* e : { linked.get(), left.get(), right.get() }) e->stopKeyDetection();
}

bool AuraPitchProcessor::isKeyDetectionListening() const noexcept { return shownEngine().isKeyDetectionListening(); }

pitch::AutoKeyDetector::Result AuraPitchProcessor::getKeyDetection() const noexcept { return shownEngine().getKeyDetection(); }

std::array<std::uint64_t, 2> AuraPitchProcessor::getHeldMidiNotes() const noexcept { return shownEngine().getHeldMidiNotes(); }

void AuraPitchProcessor::applyKey (int keyIndex, bool minor)
{
    auto set = [this] (const char* id, float value)
    {
        if (auto* prm = state.getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
            prm->endChangeGesture();
        }
    };
    set (params::key, (float) juce::jlimit (0, 11, keyIndex));
    set (params::scale, (float) (minor ? (int) pitch::ScaleType::Minor : (int) pitch::ScaleType::Major));
}

void AuraPitchProcessor::timerCallback() { syncLatency(); }

void AuraPitchProcessor::syncLatency()
{
    if (! prepared.load())
        return;
    const int wanted = latencyFor (snapshot.read().mode);
    if (wanted != getLatencySamples())
    {
        setLatencySamples (wanted);
        updateHostDisplay (ChangeDetails().withLatencyChanged (true));
    }
}

AuraPitchProcessor::Status AuraPitchProcessor::getStatus() const noexcept
{
    Status s;
    s.sampleRate      = preparedRate.load();
    s.latencySamples  = getLatencySamples();
    s.doublePrecision = getProcessingPrecision() == doublePrecision;
    s.studioActive    = studioActive.load();
    return s;
}

juce::AudioProcessorParameter* AuraPitchProcessor::getBypassParameter() const
{
    return state.getParameter (params::bypass);
}

//==============================================================================================================
int AuraPitchProcessor::getNumPrograms() { return (int) params::factoryPresets().size(); }

void AuraPitchProcessor::setCurrentProgram (int index)
{
    const auto& presets = params::factoryPresets();
    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;
    currentProgram = index;
    params::applyPreset (state, presets[(size_t) index]);
}

const juce::String AuraPitchProcessor::getProgramName (int index)
{
    const auto& presets = params::factoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? juce::String (presets[(size_t) index].name) : juce::String();
}

void AuraPitchProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto tree = state.copyState();
    tree.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);
    tree.setProperty ("program", currentProgram, nullptr);
    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void AuraPitchProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (state.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = tree.getProperty ("program", 0);
            state.replaceState (tree);
        }
}

juce::AudioProcessorEditor* AuraPitchProcessor::createEditor()
{
    return new AuraPitchEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AuraPitchProcessor();
}

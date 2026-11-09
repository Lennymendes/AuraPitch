// AuraKey — NextDSP · AuraSuite
#include "PluginProcessor.h"
#include "GUI/AuraKeyEditor.h"

using namespace juce;

//==============================================================================================================
// Fil d'analyse d'un fichier : décodage par blocs, analyse hors ligne, publication du résultat.
class AuraKeyProcessor::AnalysisJob final : public Thread
{
public:
    AnalysisJob (AuraKeyProcessor& p, File f) : Thread ("AuraKey file analysis"), owner (p), file (std::move (f))
    {
        formats.registerBasicFormats();
    }
    ~AnalysisJob() override { stopThread (10000); }

    void run() override
    {
        const auto started = Time::getMillisecondCounterHiRes();
        std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
        {
            owner.jobFinished (this, nullptr, FileState::Failed, "Unsupported or unreadable audio file");
            return;
        }
        const int channels = (int) reader->numChannels;
        const auto length = reader->lengthInSamples;
        constexpr int block = 65536;
        AudioBuffer<float> buffer (channels, block);
        aura::key::OfflineKeyAnalyzer analyzer (reader->sampleRate);
        for (int64 pos = 0; pos < length; pos += block)
        {
            if (threadShouldExit())
            {
                owner.jobFinished (this, nullptr, FileState::Cancelled, "Analysis cancelled");
                return;
            }
            const int len = (int) jmin ((int64) block, length - pos);
            if (! reader->read (&buffer, 0, len, pos, true, true))
            {
                owner.jobFinished (this, nullptr, FileState::Failed, "Read error in the audio file");
                return;
            }
            analyzer.process (buffer.getArrayOfReadPointers(), channels, len);
            owner.fileProgress.store ((float) ((double) (pos + len) / (double) jmax ((int64) 1, length)));
        }

        auto a = std::make_shared<FileAnalysis>();
        a->fileName = file.getFileName();
        a->filePath = file.getFullPathName();
        a->formatName = reader->getFormatName();
        a->sampleRate = reader->sampleRate;
        a->bitsPerSample = (int) reader->bitsPerSample;
        a->numChannels = channels;
        a->floatingPoint = reader->usesFloatingPointData;
        a->result = analyzer.finish();
        a->analysisSeconds = (Time::getMillisecondCounterHiRes() - started) * 0.001;
        if (! a->result.valid)
        {
            owner.jobFinished (this, a, FileState::Failed, "No tonal content found in the file");
            return;
        }
        owner.jobFinished (this, a, FileState::Done, {});
    }

private:
    AuraKeyProcessor& owner;
    File file;
    AudioFormatManager formats;
};

//==============================================================================================================
AuraKeyProcessor::AuraKeyProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "AuraKey", createLayout())
{
    memoryParam = state.getRawParameterValue (akparams::memory);
    formats.registerBasicFormats();
}

AuraKeyProcessor::~AuraKeyProcessor()
{
    if (job != nullptr)
        job->signalThreadShouldExit();
    job.reset();
}

AudioProcessorValueTreeState::ParameterLayout AuraKeyProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { akparams::memory, 1 }, "Memory",
                                                        akparams::memoryLabels, 1));
    return layout;
}

bool AuraKeyProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == AudioChannelSet::mono() || out == AudioChannelSet::stereo());
}

void AuraKeyProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    live.prepare (sampleRate, jmax (32, samplesPerBlock));
    currentRate.store (sampleRate);
    lastMemoryIndex = -1;
}

template <typename Sample>
void AuraKeyProcessor::process (AudioBuffer<Sample>& buffer)
{
    ScopedNoDenormals noDenormals;
    const int inputs = getTotalNumInputChannels();
    for (int c = inputs; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    const int memoryIndex = jlimit (0, 2, (int) memoryParam->load());
    if (memoryIndex != lastMemoryIndex)
    {
        live.setMemorySeconds (akparams::memorySeconds[memoryIndex]);
        lastMemoryIndex = memoryIndex;
    }
    // analyse seulement : le tampon n'est pas modifié
    live.process (buffer.getArrayOfReadPointers(), jmin (inputs, buffer.getNumChannels()), buffer.getNumSamples());
}

void AuraKeyProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    lastDouble.store (false, std::memory_order_relaxed);
    process (buffer);
}

void AuraKeyProcessor::processBlock (AudioBuffer<double>& buffer, MidiBuffer&)
{
    lastDouble.store (true, std::memory_order_relaxed);
    process (buffer);
}

AudioProcessorEditor* AuraKeyProcessor::createEditor() { return new AuraKeyEditor (*this); }

//==============================================================================================================
String AuraKeyProcessor::getSupportedWildcard() const { return formats.getWildcardForAllFormats(); }

bool AuraKeyProcessor::isSupportedFile (const File& f) const
{
    if (! f.existsAsFile()) return false;
    for (auto ext : StringArray::fromTokens (getSupportedWildcard(), ";", {}))
        if (f.hasFileExtension (ext.fromFirstOccurrenceOf ("*.", false, false)))
            return true;
    return false;
}

bool AuraKeyProcessor::analyseFile (const File& f)
{
    cancelFileAnalysis();
    if (! f.existsAsFile())
        return false;
    {
        std::lock_guard<std::mutex> l (fileLock);
        fileMessage = {};
        pendingName = f.getFileName();
    }
    fileProgress.store (0.0f);
    fileState.store (FileState::Running);
    job = std::make_unique<AnalysisJob> (*this, f);
    job->startThread (Thread::Priority::low);
    return true;
}

void AuraKeyProcessor::cancelFileAnalysis()
{
    if (job == nullptr) return;
    job->signalThreadShouldExit();
    job.reset();                                       // attend la fin du bloc en cours
}

void AuraKeyProcessor::jobFinished (AnalysisJob*, std::shared_ptr<const FileAnalysis> a, FileState s, const String& message)
{
    std::lock_guard<std::mutex> l (fileLock);
    if (s == FileState::Done)
        fileAnalysis = std::move (a);
    fileMessage = message;
    fileState.store (s);
}

String AuraKeyProcessor::getFileMessage() const
{
    std::lock_guard<std::mutex> l (fileLock);
    return fileMessage;
}

String AuraKeyProcessor::getPendingFileName() const
{
    std::lock_guard<std::mutex> l (fileLock);
    return pendingName;
}

std::shared_ptr<const FileAnalysis> AuraKeyProcessor::getFileAnalysis() const
{
    std::lock_guard<std::mutex> l (fileLock);
    return fileAnalysis;
}

bool AuraKeyProcessor::waitForFileAnalysis (int timeoutMs)
{
    if (job == nullptr) return true;
    return job->waitForThreadToExit (timeoutMs);
}

//==============================================================================================================
static String joinDoubles (const double* v, int n)
{
    StringArray s;
    for (int i = 0; i < n; ++i) s.add (String (v[i], 6));
    return s.joinIntoString (" ");
}

static void splitDoubles (const String& text, double* v, int n)
{
    const auto parts = StringArray::fromTokens (text, " ", {});
    for (int i = 0; i < n; ++i) v[i] = i < parts.size() ? parts[i].getDoubleValue() : 0.0;
}

ValueTree FileAnalysis::toValueTree() const
{
    ValueTree t ("FileAnalysis");
    const auto& r = result;
    t.setProperty ("fileName", fileName, nullptr)
     .setProperty ("filePath", filePath, nullptr)
     .setProperty ("format", formatName, nullptr)
     .setProperty ("sampleRate", sampleRate, nullptr)
     .setProperty ("bits", bitsPerSample, nullptr)
     .setProperty ("channels", numChannels, nullptr)
     .setProperty ("float", floatingPoint, nullptr)
     .setProperty ("analysisSeconds", analysisSeconds, nullptr)
     .setProperty ("valid", r.valid, nullptr)
     .setProperty ("tonic", r.tonic, nullptr)
     .setProperty ("minor", r.minor, nullptr)
     .setProperty ("confidence", r.confidence, nullptr)
     .setProperty ("bpm", r.bpm, nullptr)
     .setProperty ("bpmConfidence", r.bpmConfidence, nullptr)
     .setProperty ("tuningCents", r.tuningCents, nullptr)
     .setProperty ("duration", r.durationSec, nullptr)
     .setProperty ("tonalSeconds", r.tonalSeconds, nullptr)
     .setProperty ("posteriors", joinDoubles (r.posteriors.data(), 24), nullptr)
     .setProperty ("chroma", joinDoubles (r.chroma.data(), 12), nullptr);
    for (const auto& s : r.sections)
    {
        ValueTree c ("Section");
        c.setProperty ("start", s.start, nullptr).setProperty ("end", s.end, nullptr).setProperty ("tonic", s.tonic, nullptr)
         .setProperty ("minor", s.minor, nullptr).setProperty ("confidence", s.confidence, nullptr);
        t.appendChild (c, nullptr);
    }
    return t;
}

std::shared_ptr<const FileAnalysis> FileAnalysis::fromValueTree (const ValueTree& t)
{
    if (! t.hasType ("FileAnalysis"))
        return nullptr;
    auto a = std::make_shared<FileAnalysis>();
    auto& r = a->result;
    a->fileName = t["fileName"].toString();
    a->filePath = t["filePath"].toString();
    a->formatName = t["format"].toString();
    a->sampleRate = t["sampleRate"];
    a->bitsPerSample = t["bits"];
    a->numChannels = t["channels"];
    a->floatingPoint = t["float"];
    a->analysisSeconds = t["analysisSeconds"];
    r.valid = t["valid"];
    r.tonic = t["tonic"];
    r.minor = t["minor"];
    r.confidence = t["confidence"];
    r.bpm = t["bpm"];
    r.bpmConfidence = t["bpmConfidence"];
    r.tuningCents = t["tuningCents"];
    r.durationSec = t["duration"];
    r.tonalSeconds = t["tonalSeconds"];
    splitDoubles (t["posteriors"].toString(), r.posteriors.data(), 24);
    splitDoubles (t["chroma"].toString(), r.chroma.data(), 12);
    for (const auto& c : t)
        if (c.hasType ("Section"))
        {
            aura::key::Section s;
            s.start = c["start"]; s.end = c["end"]; s.tonic = c["tonic"]; s.minor = c["minor"]; s.confidence = c["confidence"];
            r.sections.push_back (s);
        }
    return r.valid ? a : nullptr;
}

void AuraKeyProcessor::getStateInformation (MemoryBlock& dest)
{
    auto root = state.copyState();
    root.removeChild (root.getChildWithName ("FileAnalysis"), nullptr);
    if (auto a = getFileAnalysis())
        root.appendChild (a->toValueTree(), nullptr);
    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, dest);
}

void AuraKeyProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (state.state.getType()))
        return;
    auto root = ValueTree::fromXml (*xml);
    auto restored = FileAnalysis::fromValueTree (root.getChildWithName ("FileAnalysis"));
    root.removeChild (root.getChildWithName ("FileAnalysis"), nullptr);
    state.replaceState (root);
    std::lock_guard<std::mutex> l (fileLock);
    fileAnalysis = restored;
    if (restored != nullptr && fileState.load() != FileState::Running)
        fileState.store (FileState::Done);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AuraKeyProcessor(); }

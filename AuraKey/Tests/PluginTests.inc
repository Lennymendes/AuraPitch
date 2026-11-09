// AuraKey — bancs du plug-in (inclus par AuraKeyTests.cpp quand AURAKEY_PLUGIN = 1).

#if defined (__linux__)
#include <dlfcn.h>
/** Xvfb nu : crée les atomes EWMH avant que JUCE n'ouvre l'affichage (voir Tests/Step5BTests.cpp d'AuraPitch). */
static void prepareBareXServer()
{
    void* x11 = dlopen ("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (x11 == nullptr) return;
    using OpenFn = void* (*) (const char*);
    using InternFn = unsigned long (*) (void*, const char*, int);
    auto open = (OpenFn) dlsym (x11, "XOpenDisplay");
    auto intern = (InternFn) dlsym (x11, "XInternAtom");
    if (open != nullptr && intern != nullptr)
        if (void* d = open (nullptr))
            for (const char* atom : { "WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_DELETE_WINDOW", "_NET_WM_PING", "WM_CHANGE_STATE",
                                      "WM_STATE", "_NET_WM_WINDOW_TYPE", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN",
                                      "_NET_WM_WINDOW_TYPE_NORMAL", "_MOTIF_WM_HINTS", "_NET_FRAME_EXTENTS", "_NET_WORKAREA" })
                intern (d, atom, 0);
}
#else
static void prepareBareXServer() {}
#endif

//--- outils --------------------------------------------------------------------------------------------------------
static std::unique_ptr<AuraKeyProcessor> makeKeyProcessor (double fs, int block, bool doublePrecision)
{
    auto p = std::make_unique<AuraKeyProcessor>();
    p->setPlayConfigDetails (2, 2, fs, block);
    p->setProcessingPrecision (doublePrecision ? juce::AudioProcessor::doublePrecision : juce::AudioProcessor::singlePrecision);
    p->prepareToPlay (fs, block);
    return p;
}

/** Fait passer `audio` dans le processeur ; renvoie true si la sortie est identique à l'entrée au bit près. */
template <typename Sample>
static bool streamThrough (AuraKeyProcessor& p, const std::vector<std::vector<float>>& audio, int block, long& allocs)
{
    juce::AudioBuffer<Sample> buf (2, block);
    juce::MidiBuffer midi;
    bool identical = true;
    const int n = (int) audio[0].size();
    for (int s = 0; s < n; s += block)
    {
        const int len = std::min (block, n - s);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i) buf.setSample (c, i, (Sample) audio[(size_t) c][(size_t) (s + i)]);
        gAllocations.store (0);
        gCountAllocations = true;
        p.processBlock (buf, midi);
        gCountAllocations = false;
        allocs += gAllocations.load();
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i)
            {
                const Sample in = (Sample) audio[(size_t) c][(size_t) (s + i)], out = buf.getSample (c, i);
                if (std::memcmp (&in, &out, sizeof (Sample)) != 0) identical = false;      // au bit près
            }
    }
    return identical;
}

static bool writeAudioFile (const juce::File& f, juce::AudioFormat& format, const std::vector<std::vector<float>>& audio,
                            double fs, int bits, bool floating = false)
{
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
    auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (fs).withNumChannels (2).withBitsPerSample (bits)
                    .withSampleFormat (floating ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                : juce::AudioFormatWriterOptions::SampleFormat::automatic);
    auto w = format.createWriterFor (os, opts);
    if (w == nullptr) return false;
    const float* ch[2] = { audio[0].data(), audio[1].data() };
    return w->writeFromFloatArrays (ch, 2, (int) audio[0].size());
}

static juce::File testDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aurakey_test_files");
    d.createDirectory();
    return d;
}

//==================================================================================================================
static void testPlugin()
{
    std::printf ("\n[6] Processeur VST3 : transparence au bit près, clé live, aucune allocation, état\n");
    Song song;
    song.bpm = 110.0;
    song.parts = { cadence (11, true, 25.0) };
    for (auto [fs, dbl] : { std::pair<double, bool> { 48000.0, false }, { 384000.0, true }, { 44100.0, true } })
    {
        const auto audio = render (song, fs);
        auto p = makeKeyProcessor (fs, 512, dbl);
        long allocs = 0;
        const bool same = dbl ? streamThrough<double> (*p, audio, 512, allocs) : streamThrough<float> (*p, audio, 512, allocs);
        LiveKeyState st;
        p->getLiveState (st);
        std::printf ("  %6.1f kHz %s : sortie %s · %ld allocation(s) · %s (%.1f %%)\n", fs / 1000, dbl ? "64 bits" : "32 bits",
                     same ? "identique à l'entrée" : "MODIFIÉE", allocs, name (st.tonic, st.minor).c_str(), 100.0 * st.confidence);
        check (same, "transparence au bit près");
        check (allocs == 0, "processBlock sans allocation");
        check (st.tonic == 11 && st.minor && st.confidence > 0.9, "clé live : B mineur");
        check (p->isDoublePrecisionActive() == dbl, "précision signalée à l'interface");
    }
    // bus mono, et paramètre MEMORY dans l'état
    {
        auto p = std::make_unique<AuraKeyProcessor>();
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p->setBusesLayout (mono), "bus mono accepté");
        auto* mem = p->getState().getParameter (akparams::memory);
        mem->setValueNotifyingHost (mem->convertTo0to1 (0.0f));       // 8 s
        juce::MemoryBlock block;
        p->getStateInformation (block);
        auto q = std::make_unique<AuraKeyProcessor>();
        q->setStateInformation (block.getData(), (int) block.getSize());
        check ((int) q->getState().getRawParameterValue (akparams::memory)->load() == 0, "MEMORY restauré depuis l'état");
    }
}

//==================================================================================================================
static void testFiles()
{
    std::printf ("\n[7] Import de fichiers : décodage en tâche de fond pendant que le thread audio tourne\n");
    const auto dir = testDir();
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;

    auto songOf = [] (double bpm, std::vector<Part> parts) { Song s; s.bpm = bpm; s.parts = std::move (parts); return s; };
    struct Case { juce::String file; Song song; double fs; juce::AudioFormat* format; int bits; bool floating; int tonic; bool minor; size_t sections; };
    std::vector<Case> cases = {
        { "d_major_128.wav",      songOf (128.0, { cadence (2, false, 40.0) }), 44100.0,  &wav,  16, false, 2, false, 1 },
        { "b_minor_96_dxd.wav",   songOf (96.0,  { cadence (11, true, 30.0) }), 384000.0, &wav,  32, true, 11, true, 1 },
        { "b_minor_96_dxd24.wav", songOf (96.0,  { cadence (11, true, 30.0) }), 352800.0, &wav,  24, false, 11, true, 1 },
        { "eb_major_140.aiff",    songOf (140.0, { pop (3, false, 40.0) }),     96000.0,  &aiff, 24, false, 3, false, 1 },
        { "modulation_120.flac",  songOf (120.0, { cadence (7, false, 30.0), cadence (4, true, 30.0) }), 48000.0, &flac, 24, false, 7, false, 2 },
        { "f_minor_100_dxd.flac", songOf (100.0, { cadence (5, true, 30.0) }), 384000.0, &flac, 24, false, 5, true, 1 },
    };

    // MP3 : JUCE n'a pas d'encodeur ; ffmpeg (s'il est installé) en fabrique un à partir d'un WAV
    {
        const auto src = dir.getChildFile ("a_major_100_src.wav");
        const auto mp3 = dir.getChildFile ("a_major_100.mp3");
        const auto s = songOf (100.0, { cadence (9, false, 30.0) });
        writeAudioFile (src, wav, render (s, 44100.0), 44100.0, 16);
        mp3.deleteFile();
        const auto cmd = "ffmpeg -loglevel error -y -i \"" + src.getFullPathName() + "\" -codec:a libmp3lame -b:a 192k \"" + mp3.getFullPathName() + "\"";
        if (std::system (cmd.toRawUTF8()) == 0 && mp3.existsAsFile())
            cases.push_back ({ "a_major_100.mp3", s, 44100.0, nullptr, 0, false, 9, false, 1 });
        else
            std::printf ("  (ffmpeg absent : pas de fichier MP3 de test)\n");
    }

    Song liveSong;
    liveSong.parts = { cadence (0, false, 6.0) };
    const auto liveAudio = render (liveSong, 48000.0);
    auto p = makeKeyProcessor (48000.0, 256, false);

    for (const auto& c : cases)
    {
        const auto f = dir.getChildFile (c.file);
        if (c.format != nullptr)
            check (writeAudioFile (f, *c.format, render (c.song, c.fs), c.fs, c.bits, c.floating), "écriture du fichier de test");
        // le thread audio tourne pendant tout le décodage
        std::atomic<bool> stop { false };
        std::atomic<long> audioAllocs { 0 }, audioBlocks { 0 };
        std::atomic<bool> audioIdentical { true };
        std::thread audio ([&]
        {
            while (! stop.load())
            {
                long a = 0;
                if (! streamThrough<float> (*p, liveAudio, 256, a)) audioIdentical.store (false);
                audioAllocs.fetch_add (a);
                audioBlocks.fetch_add ((long) liveAudio[0].size() / 256);
            }
        });
        const auto t0 = std::chrono::steady_clock::now();
        check (p->analyseFile (f), "analyseFile accepte le fichier");
        const bool finished = p->waitForFileAnalysis (120000);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        stop.store (true);
        audio.join();
        const auto a = p->getFileAnalysis();
        const bool ok = finished && p->getFileState() == AuraKeyProcessor::FileState::Done && a != nullptr;
        check (ok, "analyse terminée");
        if (! ok) { std::printf ("  %s : %s\n", c.file.toRawUTF8(), p->getFileMessage().toRawUTF8()); continue; }
        const auto& r = a->result;
        std::printf ("  %-22s %-5s %2d bits%s %6.1f kHz %4.0f s -> %-9s %5.1f %% · %6.2f BPM · %zu section(s) · %.2f s · audio : %ld blocs, %ld alloc.\n",
                     c.file.toRawUTF8(), a->formatName.upToFirstOccurrenceOf (" ", false, false).toRawUTF8(), a->bitsPerSample,
                     a->floatingPoint ? "f" : " ", a->sampleRate / 1000, r.durationSec, name (r.tonic, r.minor).c_str(),
                     100.0 * r.confidence, r.bpm, r.sections.size(), sec, audioBlocks.load(), audioAllocs.load());
        check (r.tonic == c.tonic && r.minor == c.minor, ("clé du fichier " + c.file).toRawUTF8());
        check (std::abs (r.bpm - c.song.bpm) < 0.5, ("tempo ±0,5 BPM " + c.file).toRawUTF8());
        check (r.sections.size() == c.sections, ("nombre de sections " + c.file).toRawUTF8());
        check (std::abs (a->sampleRate - c.fs) < 1.0 && std::abs (r.durationSec - songSeconds (c.song)) < 0.2, "fréquence et durée du fichier");
        check (audioAllocs.load() == 0 && audioIdentical.load(), "thread audio : aucune allocation, transparent, pendant le décodage");
    }

    // annulation : un nouveau fichier remplace l'analyse en cours
    {
        const auto longFile = dir.getChildFile ("long_dxd.wav");
        writeAudioFile (longFile, wav, render (songOf (120.0, { cadence (2, false, 120.0) }), 384000.0), 384000.0, 24);
        p->analyseFile (longFile);
        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        const float progress = p->getFileProgress();
        const auto t0 = std::chrono::steady_clock::now();
        p->analyseFile (dir.getChildFile ("d_major_128.wav"));
        const double cancelMs = 1000.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        p->waitForFileAnalysis (60000);
        const auto a = p->getFileAnalysis();
        std::printf ("  annulation à %.0f %% d'un fichier DXD de 2 min : %.0f ms ; résultat : %s\n", 100.0 * progress, cancelMs,
                     a != nullptr ? a->fileName.toRawUTF8() : "--");
        check (progress < 1.0f && cancelMs < 1000.0, "annulation rapide (< 1 s)");
        check (a != nullptr && a->fileName == "d_major_128.wav", "le nouveau fichier remplace l'analyse annulée");
        longFile.deleteFile();
    }

    // fichier illisible
    {
        const auto bad = dir.getChildFile ("not_audio.wav");
        bad.replaceWithText ("this is not a RIFF file");
        const auto before = p->getFileAnalysis();
        p->analyseFile (bad);
        p->waitForFileAnalysis (10000);
        std::printf ("  fichier illisible : « %s »\n", p->getFileMessage().toRawUTF8());
        check (p->getFileState() == AuraKeyProcessor::FileState::Failed && p->getFileMessage().isNotEmpty()
               && p->getFileAnalysis() == before, "fichier illisible : erreur affichée, résultat précédent conservé");
    }

    // l'analyse est sauvegardée avec le projet
    {
        p->analyseFile (dir.getChildFile ("modulation_120.flac"));
        p->waitForFileAnalysis (60000);
        juce::MemoryBlock block;
        p->getStateInformation (block);
        auto q = std::make_unique<AuraKeyProcessor>();
        q->setStateInformation (block.getData(), (int) block.getSize());
        const auto a = p->getFileAnalysis(), b = q->getFileAnalysis();
        const bool same = a != nullptr && b != nullptr && a->fileName == b->fileName && a->result.tonic == b->result.tonic
                          && a->result.minor == b->result.minor && std::abs (a->result.bpm - b->result.bpm) < 1e-4
                          && a->result.sections.size() == b->result.sections.size()
                          && q->getFileState() == AuraKeyProcessor::FileState::Done;
        std::printf ("  état : %zu octets ; analyse restaurée %s\n", (size_t) block.getSize(), same ? "à l'identique" : "DIFFÉRENTE");
        check (same, "l'analyse du fichier est restaurée avec le projet");
    }
}

//==================================================================================================================
static void testUi()
{
    std::printf ("\n[8] Interface : dépôt de fichier, cercle des quintes, carte des sections\n");
    const auto dir = testDir();
    juce::FlacAudioFormat flac;
    Song fileSong;
    fileSong.bpm = 124.0;
    fileSong.parts = { cadence (2, false, 32.0), cadence (11, true, 32.0), cadence (2, false, 24.0) };
    const auto f = dir.getChildFile ("ui_d_major_b_minor.flac");
    writeAudioFile (f, flac, render (fileSong, 96000.0), 96000.0, 24);

    auto p = makeKeyProcessor (96000.0, 512, true);
    Song liveSong;
    liveSong.parts = { cadence (7, false, 20.0) };
    long allocs = 0;
    streamThrough<double> (*p, render (liveSong, 96000.0), 512, allocs);

    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    auto* editor = dynamic_cast<AuraKeyEditor*> (ed.get());
    check (editor != nullptr, "éditeur AuraKey");
    if (editor == nullptr) return;

    check (! editor->isInterestedInFileDrag ({ dir.getChildFile ("notes.txt").getFullPathName() }), "fichier texte refusé au survol");
    check (editor->isInterestedInFileDrag ({ f.getFullPathName() }), "fichier FLAC accepté au survol");
    editor->fileDragEnter ({ f.getFullPathName() }, 10, 10);
    check (editor->getFilePanel().isDragHover(), "zone de dépôt en surbrillance pendant le survol");
    editor->filesDropped ({ f.getFullPathName() }, 10, 10);
    check (! editor->getFilePanel().isDragHover() && p->getFileState() == AuraKeyProcessor::FileState::Running, "dépôt : analyse lancée");
    p->waitForFileAnalysis (60000);
    editor->refresh();
    const auto a = p->getFileAnalysis();
    check (a != nullptr && a->result.tonic == 2 && ! a->result.minor, "fichier déposé : D majeur");
    if (a != nullptr)
    {
        std::printf ("  fichier déposé : %s (%.0f %%), %.1f BPM, sections :\n", name (a->result.tonic, a->result.minor).c_str(),
                     100.0 * a->result.confidence, a->result.bpm);
        printSections (a->result);
        check (a->result.sections.size() == 3, "trois sections : D majeur, B mineur, D majeur");
    }

    auto& circle = editor->getCircle();
    using MK = juce::ModifierKeys;
    auto mouse = [&] (juce::Component& c, juce::Point<float> pos)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, MK (MK::leftButtonModifier), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, pos, now, 1, false);
    };
    bool allKeys = true;
    for (int k = 0; k < 24; ++k)
        if (circle.keyAt (circle.centreOfKey (k)) != k) allKeys = false;
    check (allKeys, "cercle : les 24 cases répondent à leur clé");
    const int eMinor = keyIndex (4, true);
    circle.mouseMove (mouse (circle, circle.centreOfKey (eMinor)));
    check (circle.getHovered() == eMinor, "survol : E mineur");
    circle.mouseDown (mouse (circle, circle.centreOfKey (eMinor)));
    check (circle.getPinned() == eMinor, "clic : E mineur épinglée");
    circle.mouseDown (mouse (circle, circle.centreOfKey (eMinor)));
    check (circle.getPinned() < 0, "second clic : désépinglée");

    // clic sur la 2e section : sa clé (B mineur) est épinglée
    auto& map = editor->getSectionMap();
    if (a != nullptr && a->result.sections.size() >= 2)
    {
        const auto& s = a->result.sections[1];
        const auto bar = map.barArea();
        const auto pt = juce::Point<float> (bar.getX() + (float) ((s.start + s.end) * 0.5 / a->result.durationSec) * bar.getWidth(), bar.getCentreY());
        map.mouseMove (mouse (map, pt));
        map.mouseDown (mouse (map, pt));
        std::printf ("  survol de la section 2 : « %s »\n", map.getHoverText().toRawUTF8());
        check (circle.getPinned() == keyIndex (11, true) && map.getHoverText().contains ("B Minor"), "clic sur une section : B mineur épinglée");
    }

    // RESET live
    editor->getResetButton().onClick();
    streamThrough<double> (*p, render (liveSong, 96000.0), 512, allocs);   // le flux reprend : G majeur
    editor->refresh();
    LiveKeyState st;
    p->getLiveState (st);
    check (st.tonic == 7 && ! st.minor, "flux live : G majeur");

    map.mouseExit (mouse (map, {}));
    editor->refresh();
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    auto out = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aurakey_editor.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("  capture : %s\n", out.getFullPathName().toRawUTF8());
    check (img.isValid(), "rendu de l'interface");

    // éditeur vide (aucun fichier, silence)
    auto empty = makeKeyProcessor (48000.0, 512, false);
    std::unique_ptr<juce::AudioProcessorEditor> ed2 (empty->createEditor());
    auto img2 = ed2->createComponentSnapshot (ed2->getLocalBounds(), true, 1.0f);
    auto out2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aurakey_editor_empty.png");
    out2.deleteFile();
    juce::FileOutputStream os2 (out2);
    juce::PNGImageFormat().writeImageToStream (img2, os2);
    ed2.reset();
    ed.reset();
}

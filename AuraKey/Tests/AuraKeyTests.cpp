// AuraKey — NextDSP · AuraSuite
// Banc d'essai d'AuraKey (étape 6).
//
//   [1] decim    décimateur : bande utile plate, repliement rejeté, de 44,1 kHz à 384 kHz
//   [2] live     clé en direct sur des morceaux de synthèse (majeur, mineur, désaccordé, DXD), temps d'accroche
//   [3] memory   mémoire 8 s : suit une modulation ; ∞ : garde la clé du morceau ; RESET
//   [4] rt       thread audio : aucune allocation, coût CPU, lecture concurrente de l'état (seqlock)
//   [5] offline  analyse hors ligne : clé, tempo (±0,5 BPM), accord, carte des sections (modulation détectée)
//   Avec le plug-in (AURAKEY_PLUGIN) :
//   [6] plugin   processeur VST3 : transparence au bit près (32 et 64 bits), état, paramètres
//   [7] files    import : WAV 16/24/32f, AIFF, FLAC, MP3 (si ffmpeg est présent), DXD 384 kHz ; décodage en tâche de
//                fond pendant que le thread audio tourne sans allocation ; annulation ; fichier illisible
//   [8] ui       interface : dépôt de fichier, cercle des quintes (survol, clic), capture PNG
//
// Argument facultatif : le nom d'un test (« live », « files »…).
#include "../Source/DSP/LiveKeyAnalyzer.h"
#include "../Source/DSP/OfflineKeyAnalyzer.h"
#include "SyntheticMusic.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>

#if AURAKEY_PLUGIN
#include "../Source/PluginProcessor.h"
#include "../Source/GUI/AuraKeyEditor.h"
#endif

//--- compteur d'allocations (thread courant) ------------------------------------------------------------------------
static std::atomic<long> gAllocations { 0 };
static thread_local bool gCountAllocations = false;
void* operator new (std::size_t n)
{
    if (gCountAllocations) gAllocations.fetch_add (1);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

static int gChecks = 0, gFailures = 0;
static void check (bool ok, const char* what)
{
    ++gChecks;
    if (! ok) { ++gFailures; std::printf ("  ÉCHEC : %s\n", what); }
}

static double cpuBudget (double percent)
{
    double scale = 1.0;
    if (const char* v = std::getenv ("AURAPITCH_CPU_SCALE")) scale = std::max (1.0, std::atof (v));
    return percent * scale;
}

using namespace aura::key;
using namespace aurakey_test;
constexpr double kPi = 3.14159265358979323846;

static std::string name (int tonic, bool minor) { return tonic < 0 ? std::string ("—") : keyName (tonic, minor); }

//==================================================================================================================
static void testDecimator()
{
    std::printf ("\n[1] Décimateur : bande utile (≤ 2 kHz) plate, repliement rejeté\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0, 352800.0, 384000.0 })
    {
        Decimator d;
        d.prepare (fs);
        auto toneOut = [&] (double f)
        {
            d.reset();
            const int n = (int) (fs * 0.5);
            std::vector<double> x ((size_t) n);
            for (int i = 0; i < n; ++i) x[(size_t) i] = std::sin (2.0 * kPi * f * i / fs);
            std::vector<double> y;
            d.push (x.data(), n, [&] (double v) { y.push_back (v); });
            double e = 0.0;
            const size_t skip = y.size() / 4;
            for (size_t i = skip; i < y.size(); ++i) e += y[i] * y[i];
            return 10.0 * std::log10 (2.0 * e / (double) (y.size() - skip) + 1e-30);
        };
        const double fa = d.getOutputRate();
        const double pass = std::max (std::abs (toneOut (440.0)), std::abs (toneOut (2000.0)));
        const double alias = toneOut (fa - 1000.0);         // se replierait sur 1 kHz
        std::printf ("  %6.1f kHz : ÷%-2d -> %7.1f Hz · 440 Hz / 2 kHz %+.3f dB · repliement (%.0f Hz -> 1 kHz) %.1f dB\n",
                     fs / 1000, d.getFactor(), fa, pass, fa - 1000.0, alias);
        check (fa > 10000.0 && fa < 12100.0, "fréquence d'analyse ≈ 11 kHz");
        check (pass < 0.05, "bande utile plate (±0,05 dB)");
        check (alias < -70.0, "repliement < −70 dB");
    }
}

//==================================================================================================================
struct LiveRun { LiveKeyState state; double lockSeconds = -1.0; };

static LiveRun runLive (const std::vector<std::vector<float>>& audio, double fs, double memory, int block = 512,
                        int expectTonic = -1, bool expectMinor = false)
{
    LiveKeyAnalyzer a;
    a.setMemorySeconds (memory);
    a.prepare (fs, block);
    LiveRun r;
    const int n = (int) audio[0].size();
    const float* ch[2];
    for (int s = 0; s < n; s += block)
    {
        const int len = std::min (block, n - s);
        ch[0] = audio[0].data() + s; ch[1] = audio[1].data() + s;
        a.process (ch, 2, len);
        LiveKeyState st;
        a.getState (st);
        const bool ok = st.tonic == expectTonic && st.minor == expectMinor && st.confidence > 0.5;
        if (ok && r.lockSeconds < 0.0) r.lockSeconds = (double) (s + len) / fs;
        if (! ok) r.lockSeconds = -1.0;
    }
    a.getState (r.state);
    return r;
}

static void testLive()
{
    std::printf ("\n[2] Clé en direct (mémoire ∞) : morceaux de synthèse de 30 s\n");
    struct Case { const char* label; Part part; double fs; double detune; bool drums; bool ambiguous = false; };
    const Case cases[] = {
        { "D majeur, I-IV-V-I",            cadence (2, false, 30.0),  48000.0,   0.0, true },
        { "B mineur, i-iv-V-i (DXD)",      cadence (11, true, 30.0),  384000.0,  0.0, true },
        { "F mineur, i-iv-V-i",            cadence (5, true, 30.0),   44100.0,   0.0, true },
        { "E majeur, I-vi-IV-V",           pop (4, false, 30.0),      96000.0,   0.0, true },
        { "Ab majeur, I-vi-IV-V, sans batterie", pop (8, false, 30.0), 48000.0,  0.0, false },
        { "C majeur désaccordé +35 c",     cadence (0, false, 30.0),  48000.0,  35.0, true },
        { "G mineur désaccordé −40 c",     cadence (7, true, 30.0),   88200.0, -40.0, true },
        // Am F C G : exactement les accords de C majeur (vi-IV-I-V). Seuls le début et la fin sur Am le disent mineur ;
        // un histogramme de classes de hauteur ne voit pas l'ordre : la relative est acceptée (limite connue).
        { "A mineur, i-VI-III-VII (ambigu)", pop (9, true, 30.0),     48000.0,   0.0, true, true },
    };
    for (const auto& c : cases)
    {
        Song song;
        song.bpm = 120.0; song.detuneCents = c.detune; song.drums = c.drums;
        song.parts = { c.part };
        const auto audio = render (song, c.fs);
        const auto r = runLive (audio, c.fs, 0.0, 512, c.part.tonic, c.part.minor);
        const bool exact = r.state.tonic == c.part.tonic && r.state.minor == c.part.minor;
        const int relTonic = c.part.minor ? (c.part.tonic + 3) % 12 : (c.part.tonic + 9) % 12;
        const bool relative = r.state.tonic == relTonic && r.state.minor != c.part.minor;
        const bool ok = exact || (c.ambiguous && relative);
        std::printf ("  %-36s %6.1f kHz -> %-10s %5.1f %% · accroche %4.1f s · accord %+5.1f c\n", c.label, c.fs / 1000,
                     name (r.state.tonic, r.state.minor).c_str(), 100.0 * r.state.confidence, r.lockSeconds, r.state.tuningCents);
        check (ok, (std::string ("clé détectée : ") + c.label).c_str());
        check (r.state.confidence > 0.9, (std::string ("confiance > 90 % après 30 s : ") + c.label).c_str());
        if (c.ambiguous)
        {
            std::printf ("      (relative %s : %.1f %%, clé écrite %s : %.1f %%)\n", name (relTonic, ! c.part.minor).c_str(),
                         100.0 * r.state.posteriors[(size_t) keyIndex (relTonic, ! c.part.minor)], name (c.part.tonic, c.part.minor).c_str(),
                         100.0 * r.state.posteriors[(size_t) keyIndex (c.part.tonic, c.part.minor)]);
            continue;
        }
        check (r.lockSeconds >= 0.0 && r.lockSeconds < 15.0, (std::string ("accroche en moins de 15 s : ") + c.label).c_str());
        if (std::abs (c.detune) > 0.0)
            check (std::abs (r.state.tuningCents - c.detune) < 5.0, "accord du flux estimé à ±5 c");
    }
}

//==================================================================================================================
static void testMemory()
{
    std::printf ("\n[3] Mémoire : 40 s en D majeur puis 40 s en F mineur\n");
    const double fs = 48000.0;
    Song song;
    song.parts = { cadence (2, false, 40.0), cadence (5, true, 40.0) };
    const auto audio = render (song, fs);
    const auto shortMem = runLive (audio, fs, 8.0, 512, 5, true);
    const auto infinite = runLive (audio, fs, 0.0, 512, 5, true);
    std::printf ("  mémoire 8 s -> %s (%.1f %%), suit la modulation %.1f s après le changement\n",
                 name (shortMem.state.tonic, shortMem.state.minor).c_str(), 100.0 * shortMem.state.confidence,
                 shortMem.lockSeconds - 40.0);
    std::printf ("  mémoire ∞    -> %s (%.1f %%) : 80 s de morceau cumulées\n",
                 name (infinite.state.tonic, infinite.state.minor).c_str(), 100.0 * infinite.state.confidence);
    check (shortMem.state.tonic == 5 && shortMem.state.minor && shortMem.lockSeconds > 40.0 && shortMem.lockSeconds < 60.0,
           "mémoire 8 s : F mineur moins de 20 s après la modulation");
    check (infinite.lockSeconds < 0.0 || infinite.lockSeconds > shortMem.lockSeconds, "mémoire ∞ : plus lente à changer");

    // RESET : vide l'historique
    LiveKeyAnalyzer a;
    a.prepare (fs, 512);
    const float* ch[2] = { audio[0].data(), audio[1].data() };
    a.process (ch, 2, (int) (20.0 * fs));
    LiveKeyState before, after;
    a.getState (before);
    a.requestReset();
    float silence[64] {};
    const float* sc[2] = { silence, silence };
    a.process (sc, 2, 64);
    a.getState (after);
    std::printf ("  RESET : %s (%u trames) -> %s (%u trames)\n", name (before.tonic, before.minor).c_str(), before.frames,
                 name (after.tonic, after.minor).c_str(), after.frames);
    check (before.tonic == 2 && after.tonic < 0 && after.frames == 0, "RESET vide l'historique");
}

//==================================================================================================================
static void testRealtime()
{
    std::printf ("\n[4] Thread audio : allocations, CPU, lecture concurrente de l'état\n");
    for (double fs : { 48000.0, 384000.0 })
    {
        Song song;
        song.parts = { cadence (11, true, 12.0) };
        const auto audio = render (song, fs);
        LiveKeyAnalyzer a;
        a.prepare (fs, 256);
        std::atomic<bool> stop { false };
        std::atomic<long> reads { 0 }, torn { 0 };
        std::thread ui ([&]
        {
            // l'interface lit l'état en continu ; une lecture valide est toujours cohérente
            while (! stop.load())
            {
                LiveKeyState st;
                if (a.getState (st))
                {
                    double s = 0.0;
                    for (double v : st.posteriors) s += v;
                    if (std::abs (s - 1.0) > 1e-9) torn.fetch_add (1);
                    reads.fetch_add (1);
                }
            }
        });
        const int n = (int) audio[0].size();
        long allocs = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < n;)
        {
            const int len = std::min (n - s, 64 + (s / 64) % 4 * 160);    // blocs irréguliers (64 … 544, plus que préparé)
            const float* ch[2] = { audio[0].data() + s, audio[1].data() + s };
            gAllocations.store (0);
            gCountAllocations = true;
            a.process (ch, 2, len);
            gCountAllocations = false;
            allocs += gAllocations.load();
            s += len;
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        stop.store (true);
        ui.join();
        const double cpu = 100.0 * sec / ((double) n / fs);
        LiveKeyState st;
        a.getState (st);
        std::printf ("  %6.1f kHz : %ld allocation(s) · CPU %.2f %% d'un cœur · %ld lectures de l'interface, %ld incohérente(s) · %s\n",
                     fs / 1000, allocs, cpu, reads.load(), torn.load(), name (st.tonic, st.minor).c_str());
        check (allocs == 0, "aucune allocation sur le thread audio");
        check (cpu < cpuBudget (fs > 200000 ? 3.0 : 1.0), "coût CPU (≤ 1 % à 48 kHz, ≤ 3 % en DXD)");
        check (torn.load() == 0 && reads.load() > 0, "état lu par l'interface toujours cohérent");
    }
}

//==================================================================================================================
static KeyAnalysisResult runOffline (const std::vector<std::vector<float>>& audio, double fs, int block = 65536)
{
    OfflineKeyAnalyzer a (fs);
    const int n = (int) audio[0].size();
    for (int s = 0; s < n; s += block)
    {
        const float* ch[2] = { audio[0].data() + s, audio[1].data() + s };
        a.process (ch, 2, std::min (block, n - s));
    }
    return a.finish();
}

static void printSections (const KeyAnalysisResult& r)
{
    for (const auto& s : r.sections)
        std::printf ("      %5.1f – %5.1f s : %-10s %5.1f %%\n", s.start, s.end, keyName (s.tonic, s.minor).c_str(), 100.0 * s.confidence);
}

static void testOffline()
{
    std::printf ("\n[5] Analyse hors ligne : clé, tempo, accord, sections\n");
    struct Case { const char* label; Song song; double fs; int tonic; bool minor; };
    auto song = [] (double bpm, std::vector<Part> parts, double detune = 0.0)
    {
        Song s; s.bpm = bpm; s.parts = std::move (parts); s.detuneCents = detune; return s;
    };
    const Case cases[] = {
        { "D majeur, 128 BPM",               song (128.0, { cadence (2, false, 45.0) }),  44100.0,  2, false },
        { "B mineur, 96 BPM (DXD)",          song (96.0,  { cadence (11, true, 40.0) }),  384000.0, 11, true },
        { "Eb majeur pop, 140 BPM",          song (140.0, { pop (3, false, 45.0) }),      48000.0,  3, false },
        { "C# mineur, 75 BPM, −25 c",        song (75.0,  { cadence (1, true, 50.0) }, -25.0), 96000.0, 1, true },
    };
    for (const auto& c : cases)
    {
        const auto audio = render (c.song, c.fs);
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = runOffline (audio, c.fs);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  %-30s %6.1f kHz -> %-10s %5.1f %% · %6.2f BPM (conf. %.2f) · accord %+5.1f c · %zu section(s) · analyse %.2f s pour %.0f s\n",
                     c.label, c.fs / 1000, name (r.tonic, r.minor).c_str(), 100.0 * r.confidence, r.bpm, r.bpmConfidence,
                     r.tuningCents, r.sections.size(), sec, r.durationSec);
        check (r.valid && r.tonic == c.tonic && r.minor == c.minor, (std::string ("clé du fichier : ") + c.label).c_str());
        check (std::abs (r.bpm - c.song.bpm) < 0.5, (std::string ("tempo ±0,5 BPM : ") + c.label).c_str());
        check (std::abs (r.tuningCents - c.song.detuneCents) < 5.0, "accord ±5 c");
        check (r.sections.size() == 1 && r.sections[0].tonic == c.tonic && r.sections[0].minor == c.minor,
               "une seule section, dans la clé du morceau");
    }

    std::printf ("  modulations : D majeur 36 s -> F mineur 36 s -> A majeur 36 s, 120 BPM\n");
    const auto audio = render (song (120.0, { cadence (2, false, 36.0), cadence (5, true, 36.0), cadence (9, false, 36.0) }), 48000.0);
    const auto r = runOffline (audio, 48000.0);
    printSections (r);
    const bool three = r.sections.size() == 3;
    check (three && r.sections[0].tonic == 2 && ! r.sections[0].minor && r.sections[1].tonic == 5 && r.sections[1].minor
               && r.sections[2].tonic == 9 && ! r.sections[2].minor, "trois sections : D majeur, F mineur, A majeur");
    if (three)
        check (std::abs (r.sections[1].start - 36.0) <= 3.0 && std::abs (r.sections[2].start - 72.0) <= 3.0,
               "limites des sections à ±3 s");
    check (std::abs (r.bpm - 120.0) < 0.5, "tempo du morceau modulant ±0,5 BPM");

    // silence et bruit : pas de clé inventée
    std::vector<std::vector<float>> silence (2, std::vector<float> ((size_t) (48000 * 10), 0.0f));
    const auto rs = runOffline (silence, 48000.0);
    check (! rs.valid && rs.bpm <= 0.0, "silence : ni clé ni tempo");
}

//==================================================================================================================
#if AURAKEY_PLUGIN
#include "PluginTests.inc"
#endif

int main (int argc, char** argv)
{
#if AURAKEY_PLUGIN
    prepareBareXServer();
    juce::ScopedJuceInitialiser_GUI juceInit;
#endif
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&] (const char* k) { return only.empty() || only == k; };
    if (want ("decim"))   testDecimator();
    if (want ("live"))    testLive();
    if (want ("memory"))  testMemory();
    if (want ("rt"))      testRealtime();
    if (want ("offline")) testOffline();
#if AURAKEY_PLUGIN
    if (want ("plugin"))  testPlugin();
    if (want ("files"))   testFiles();
    if (want ("ui"))      testUi();
#endif
    std::printf ("\n%d / %d vérifications passent\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}

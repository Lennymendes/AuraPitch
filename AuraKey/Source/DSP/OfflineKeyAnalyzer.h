// AuraKey — NextDSP · AuraSuite
// OfflineKeyAnalyzer.h : analyse complète d'un morceau (fichier importé), sur un fil de travail.
//
// Le fichier est lu par blocs (aucune piste entière en mémoire) : chaque bloc est mixé en mono, décimé vers ≈ 11 kHz,
// puis alimente le chromagramme (une trame / 0,19 s, conservée : 36 flottants) et l'enveloppe d'attaques du tempo.
// finish() :
//   * accord du morceau = moyenne circulaire des écarts de tous les pics ;
//   * clé globale = Auto-Key de l'étape 5A (profils de Temperley, a posteriori bayésien) sur le chromagramme cumulé ;
//   * tempo (TempoEstimator) ;
//   * carte des sections : a posteriori des 24 clés sur une fenêtre glissante de 7 s (une valeur par seconde), puis
//     Viterbi (rester dans la même clé ne coûte rien, en changer coûte kSwitchPenalty) : une modulation n'est retenue que
//     si plusieurs secondes l'attestent ; les sections de moins de 6 s sont rattachées à leur voisine.
#pragma once

#include "AutoKeyDetector.h"
#include "ChromaAnalyzer.h"
#include "Decimator.h"
#include "KeyTypes.h"
#include "TempoEstimator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aura::key
{

class OfflineKeyAnalyzer
{
public:
    static constexpr double kWindowHalfSeconds = 3.0, kSwitchPenalty = 18.0, kMinSectionSeconds = 6.0;

    explicit OfflineKeyAnalyzer (double sampleRate)
    {
        fs = sampleRate;
        decimator.prepare (fs);
        chroma.prepare (decimator.getOutputRate());
        tempo.prepare (decimator.getOutputRate());
    }

    /** Un bloc de `n` échantillons sur `numChannels` canaux (float ou double). */
    template <typename Sample>
    void process (const Sample* const* channels, int numChannels, int n)
    {
        if (numChannels <= 0 || n <= 0) return;
        mono.resize ((size_t) n);
        const double g = 1.0 / numChannels;
        for (int i = 0; i < n; ++i)
        {
            double acc = 0.0;
            for (int c = 0; c < numChannels; ++c) acc += (double) channels[c][i];
            mono[(size_t) i] = acc * g;
        }
        decimated.clear();
        decimator.push (mono.data(), n, [this] (double v) { decimated.push_back (v); });
        chroma.push (decimated.data(), (int) decimated.size(), [this] (const ChromaFrame& f) { frames.push_back (f); });
        tempo.push (decimated.data(), (int) decimated.size());
        samplesIn += n;
    }

    double secondsProcessed() const noexcept { return (double) samplesIn / fs; }

    KeyAnalysisResult finish() const
    {
        KeyAnalysisResult r;
        r.durationSec = secondsProcessed();
        double tx = 0.0, ty = 0.0, tw = 0.0;
        for (const auto& f : frames) { tx += f.tuneX; ty += f.tuneY; tw += f.tuneW; }
        const double tuning = ChromaAnalyzer::tuningFrom (tx, ty, tw);
        r.tuningCents = 100.0 * tuning;

        aura::pitch::AutoKeyDetector global;
        global.setEvidenceRate (kChromaEvidenceRate);
        std::vector<std::array<double, 12>> folded (frames.size());
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const auto& f = frames[i];
            if (f.weight <= 0.0) continue;
            folded[i] = ChromaAnalyzer::fold (f.fine, tuning);
            global.addPitchClassWeights (folded[i], f.weight, f.dt);
            for (int k = 0; k < 12; ++k) r.chroma[(size_t) k] += f.weight * folded[i][(size_t) k];
            r.tonalSeconds += f.dt;
        }
        double sum = 0.0;
        for (double v : r.chroma) sum += v;
        if (sum > 0.0)
            for (auto& v : r.chroma) v /= sum;

        const auto t = tempo.estimate();
        r.bpm = t.bpm;
        r.bpmConfidence = t.confidence;
        if (r.tonalSeconds <= 0.0)
            return r;

        const auto g = global.analyse();
        r.valid = g.key >= 0;
        r.tonic = g.key;
        r.minor = g.minor;
        r.confidence = g.confidence;
        global.posteriors (r.posteriors);
        r.sections = mapSections (folded);
        return r;
    }

private:
    std::vector<Section> mapSections (const std::vector<std::array<double, 12>>& folded) const
    {
        const int blocks = std::max (1, (int) std::ceil (secondsProcessed()));
        std::vector<std::array<double, 12>> hist ((size_t) blocks);
        std::vector<double> secs ((size_t) blocks, 0.0);
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const auto& f = frames[i];
            if (f.weight <= 0.0) continue;
            const int b = std::clamp ((int) f.time, 0, blocks - 1);
            for (int k = 0; k < 12; ++k) hist[(size_t) b][(size_t) k] += f.weight * folded[i][(size_t) k];
            secs[(size_t) b] += f.dt;
        }

        // a posteriori par seconde, sur une fenêtre de ±3 s
        const int w = (int) kWindowHalfSeconds;
        std::vector<std::array<double, 24>> post ((size_t) blocks);
        std::vector<std::array<double, 24>> emit ((size_t) blocks);
        for (int b = 0; b < blocks; ++b)
        {
            std::array<double, 12> h {};
            double s = 0.0;
            for (int j = std::max (0, b - w); j <= std::min (blocks - 1, b + w); ++j)
            {
                for (int k = 0; k < 12; ++k) h[(size_t) k] += hist[(size_t) j][(size_t) k];
                s += secs[(size_t) j];
            }
            double total = 0.0;
            for (double v : h) total += v;
            aura::pitch::AutoKeyDetector d;
            d.setEvidenceRate (kChromaEvidenceRate);
            d.addPitchClassWeights (h, total, s);
            d.posteriors (post[(size_t) b]);
            for (int k = 0; k < 24; ++k)
                emit[(size_t) b][(size_t) k] = std::log (post[(size_t) b][(size_t) k] + 1.0e-12);
        }

        // Viterbi
        std::vector<std::array<int, 24>> from ((size_t) blocks);
        std::array<double, 24> score = emit[0];
        for (int b = 1; b < blocks; ++b)
        {
            int bestPrev = 0;
            for (int k = 1; k < 24; ++k)
                if (score[(size_t) k] > score[(size_t) bestPrev]) bestPrev = k;
            std::array<double, 24> next {};
            for (int k = 0; k < 24; ++k)
            {
                const double stay = score[(size_t) k], change = score[(size_t) bestPrev] - kSwitchPenalty;
                from[(size_t) b][(size_t) k] = stay >= change ? k : bestPrev;
                next[(size_t) k] = std::max (stay, change) + emit[(size_t) b][(size_t) k];
            }
            score = next;
        }
        std::vector<int> path ((size_t) blocks);
        int s = 0;
        for (int k = 1; k < 24; ++k)
            if (score[(size_t) k] > score[(size_t) s]) s = k;
        for (int b = blocks - 1; b >= 0; --b)
        {
            path[(size_t) b] = s;
            if (b > 0) s = from[(size_t) b][(size_t) s];
        }

        // segments, puis rattachement des sections trop courtes
        struct Run { int first, last, key; };
        std::vector<Run> runs;
        for (int b = 0; b < blocks; ++b)
            if (runs.empty() || runs.back().key != path[(size_t) b]) runs.push_back ({ b, b, path[(size_t) b] });
            else runs.back().last = b;
        bool merged = true;
        while (merged && runs.size() > 1)
        {
            merged = false;
            size_t shortest = 0;
            for (size_t i = 1; i < runs.size(); ++i)
                if (runs[i].last - runs[i].first < runs[shortest].last - runs[shortest].first) shortest = i;
            if (runs[shortest].last - runs[shortest].first + 1 < (int) kMinSectionSeconds)
            {
                const size_t into = shortest == 0 ? 1 : shortest - 1;
                runs[into].first = std::min (runs[into].first, runs[shortest].first);
                runs[into].last = std::max (runs[into].last, runs[shortest].last);
                runs.erase (runs.begin() + (long) shortest);
                for (size_t i = 1; i < runs.size();)
                    if (runs[i].key == runs[i - 1].key) { runs[i - 1].last = runs[i].last; runs.erase (runs.begin() + (long) i); }
                    else ++i;
                merged = true;
            }
        }

        std::vector<Section> out;
        for (const auto& run : runs)
        {
            Section sec;
            sec.start = run.first;
            sec.end = std::min (secondsProcessed(), (double) run.last + 1.0);
            sec.tonic = run.key % 12;
            sec.minor = run.key >= 12;
            double c = 0.0;
            for (int b = run.first; b <= run.last; ++b) c += post[(size_t) b][(size_t) run.key];
            sec.confidence = c / (run.last - run.first + 1);
            out.push_back (sec);
        }
        return out;
    }

    double fs = 48000.0;
    long long samplesIn = 0;
    Decimator decimator;
    ChromaAnalyzer chroma;
    TempoEstimator tempo;
    std::vector<double> mono, decimated;
    std::vector<ChromaFrame> frames;
};

} // namespace aura::key

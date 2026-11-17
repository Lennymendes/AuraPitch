// AuraPitch — NextDSP · AuraSuite
// NoteGraphModel.h : GRAPH EDITOR (étape 5B) — courbe de hauteur éditée, calée sur la timeline du DAW, sans audio en RAM.
//
//   * Capture : pendant la lecture, le moteur pousse ses trames d'analyse (temps timeline, hauteur MIDI, voisement,
//     niveau) dans une FIFO sans verrou ; le message thread les range ici (≈ 670 trames/s, 16 octets chacune).
//   * Segmentation : passages voisés (trous < 40 ms comblés) -> hauteur lissée sur 150 ms (le vibrato s'efface) ->
//     notes par hystérésis de 0,6 dt -> frontière placée où la hauteur brute franchit le milieu des deux notes
//     (liaisons legato) -> NoteBlob. Chaque note garde sa micro-courbe (écart au centre, en cents, à 200 Hz) et son
//     enveloppe de niveau : de quoi dessiner la vraie forme et rejouer la correction sans relire l'audio.
//   * Rejeu : à l'instant t de la timeline, hauteur voulue = cible + vibratoDepth × écart(t) + glissé d'attaque ;
//     hauteur chantée = centre + écart(t) (connue d'avance, mesurée au passage de capture) ; décalage = différence.
//     La correction n'attend donc pas le détecteur : pas de retard, même en Live.
//   * Le thread audio ne lit qu'un GraphSnapshot immuable, échangé sans verrou (GraphExchange) ; les snapshots
//     remplacés sont rendus au message thread pour y être libérés.
//
// Tout ce fichier est sans JUCE. NoteGraphModel n'est utilisé que par le message thread.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace aura::pitch
{

/** Trame capturée (thread audio -> FIFO -> message thread). */
struct CaptureFrame
{
    double time   = 0.0;      // s, timeline du DAW
    float  midi   = 0.0f;     // hauteur chantée (MIDI fractionnaire), 0 si non voisé
    float  levelDb = -120.0f; // niveau RMS de la trame
    bool   voiced = false;
};

/** FIFO SPSC statique de trames de capture (thread audio -> message thread). */
class CaptureFifo
{
public:
    static constexpr int kSize = 16384;

    bool push (const CaptureFrame& f) noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        if (w - read.load (std::memory_order_acquire) >= kSize) return false;   // pleine : la trame est perdue
        buffer[(size_t) (w % kSize)] = f;
        write.store (w + 1, std::memory_order_release);
        return true;
    }

    template <typename Fn>
    int drain (Fn&& fn)
    {
        auto r = read.load (std::memory_order_relaxed);
        const auto w = write.load (std::memory_order_acquire);
        int n = 0;
        for (; r < w; ++r, ++n) fn (buffer[(size_t) (r % kSize)]);
        read.store (r, std::memory_order_release);
        return n;
    }

private:
    std::array<CaptureFrame, kSize> buffer {};
    std::atomic<std::int64_t> write { 0 }, read { 0 };
};

//==============================================================================================================
/** Note éditable. */
struct NoteBlob
{
    double startTimeSec          = 0.0;
    double durationSec           = 0.0;
    double originalCenterPitch   = 60.0;   // MIDI fractionnaire
    double targetPitch           = 60.0;
    double vibratoDepth          = 1.0;    // 0 … 2 : 0 = droite, 1 = naturel, > 1 = accentué
    double transitionInRatio     = 0.0;    // 0 … 1 : glissé depuis la note précédente (0 = aucun, la liaison chantée reste)
    double formantOffsetSemitones = 0.0;   // −12 … +12
    std::vector<float> pitchDeviationsCents;   // écart au centre, à kCurveRate Hz depuis startTimeSec
    std::vector<float> levelDb;                // enveloppe de niveau, même cadence

    static constexpr double kCurveRate = 200.0;

    double endTimeSec() const noexcept { return startTimeSec + durationSec; }

    /** Écart chanté au centre (cents) à l'instant t, interpolé. */
    double deviationAt (double t) const noexcept
    {
        if (pitchDeviationsCents.empty()) return 0.0;
        const double x = std::clamp ((t - startTimeSec) * kCurveRate, 0.0, (double) pitchDeviationsCents.size() - 1.0);
        const size_t i = std::min ((size_t) x, pitchDeviationsCents.size() - 1);
        const size_t j = std::min (i + 1, pitchDeviationsCents.size() - 1);
        const double f = x - (double) i;
        return pitchDeviationsCents[i] + f * (pitchDeviationsCents[j] - pitchDeviationsCents[i]);
    }
    double levelAt (double t) const noexcept
    {
        if (levelDb.empty()) return -120.0;
        const size_t i = std::min ((size_t) std::max (0.0, (t - startTimeSec) * kCurveRate), levelDb.size() - 1);
        return levelDb[i];
    }
};

//==============================================================================================================
/** Vue immuable des notes pour le thread audio (aucune allocation à la lecture). */
struct GraphSnapshot
{
    struct Note
    {
        double start = 0.0, end = 0.0, target = 0.0, center = 0.0, depth = 1.0, formant = 0.0;
        double glideTime = 0.0, glideFrom = 0.0;     // glissé d'attaque : durée (s), écart initial (dt)
        std::uint32_t curveOffset = 0, curveCount = 0;
    };
    struct Result
    {
        double shiftSemitones = 0.0;   // décalage à appliquer à la voix chantée
        double formantSemitones = 0.0; // décalage de formant propre à la note
        double inputMidi = 0.0;        // hauteur chantée (capture)
        double outputMidi = 0.0;       // hauteur voulue
    };

    std::vector<Note> notes;          // triées par début, sans chevauchement
    std::vector<float> curves;        // micro-courbes concaténées (cents)
    bool anyFormant = false;

    /** Note couvrant t : décalage voulu. false hors des notes (la correction automatique reprend). */
    bool evaluate (double t, Result& r) const noexcept
    {
        if (notes.empty() || t < notes.front().start) return false;
        // dernière note qui commence avant t
        size_t lo = 0, hi = notes.size();
        while (hi - lo > 1)
        {
            const size_t mid = (lo + hi) / 2;
            if (notes[mid].start <= t) lo = mid; else hi = mid;
        }
        const Note& n = notes[lo];
        if (t >= n.end) return false;

        double dev = 0.0;
        if (n.curveCount > 0)
        {
            const double x = std::clamp ((t - n.start) * NoteBlob::kCurveRate, 0.0, (double) n.curveCount - 1.0);
            const std::uint32_t i = std::min ((std::uint32_t) x, n.curveCount - 1), j = std::min (i + 1, n.curveCount - 1);
            const double f = x - (double) i;
            const float* c = curves.data() + n.curveOffset;
            dev = (c[i] + f * (c[j] - c[i])) / 100.0;
        }
        double glide = 0.0;
        if (n.glideTime > 0.0 && t - n.start < n.glideTime)
        {
            const double u = (t - n.start) / n.glideTime;
            glide = n.glideFrom * (1.0 - u * u * (3.0 - 2.0 * u));
        }
        r.inputMidi  = n.center + dev;
        r.outputMidi = n.target + n.depth * dev + glide;
        r.shiftSemitones = r.outputMidi - r.inputMidi;
        r.formantSemitones = n.formant;
        return true;
    }
};

/** Échange sans verrou d'un GraphSnapshot (message thread -> thread audio). Les snapshots remplacés reviennent au
    message thread par une file SPSC et y sont libérés : le thread audio ne libère ni n'alloue jamais. */
class GraphExchange
{
public:
    GraphExchange() = default;
    GraphExchange (const GraphExchange&) = delete;
    GraphExchange& operator= (const GraphExchange&) = delete;
    ~GraphExchange()
    {
        delete pending.exchange (nullptr);
        delete current;
        collect();
    }

    //--- message thread
    void publish (std::unique_ptr<GraphSnapshot> s)
    {
        collect();
        delete pending.exchange (s.release(), std::memory_order_acq_rel);   // jamais vu par l'audio : libérable
    }
    void collect()
    {
        auto r = retireRead.load (std::memory_order_relaxed);
        const auto w = retireWrite.load (std::memory_order_acquire);
        for (; r < w; ++r) delete retired[(size_t) (r % kRetire)];
        retireRead.store (r, std::memory_order_release);
    }

    //--- thread audio (début de bloc) : le snapshot rendu reste valable jusqu'au prochain acquire()
    const GraphSnapshot* acquire() noexcept
    {
        if (pending.load (std::memory_order_relaxed) == nullptr)
            return current;
        const auto w = retireWrite.load (std::memory_order_relaxed);
        if (w - retireRead.load (std::memory_order_acquire) >= kRetire)
            return current;                          // file de retour pleine : on garde l'ancien un bloc de plus
        if (auto* p = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (current != nullptr)
            {
                retired[(size_t) (w % kRetire)] = current;
                retireWrite.store (w + 1, std::memory_order_release);
            }
            current = p;
        }
        return current;
    }

private:
    static constexpr std::int64_t kRetire = 64;
    std::atomic<GraphSnapshot*> pending { nullptr };
    GraphSnapshot* current = nullptr;
    std::array<GraphSnapshot*, kRetire> retired {};
    std::atomic<std::int64_t> retireWrite { 0 }, retireRead { 0 };
};

//==============================================================================================================
/** Modèle du GRAPH EDITOR (message thread). */
class NoteGraphModel
{
public:
    static constexpr double kBridgeGap   = 0.040;   // trou non voisé comblé à l'intérieur d'une phrase
    static constexpr double kSmoothSpan  = 0.150;   // lissage de la hauteur pour la segmentation (≈ 1 période de vibrato)
    static constexpr double kHysteresis  = 0.60;    // dt
    static constexpr double kMinNote     = 0.060;   // s
    static constexpr double kSettle      = 0.080;   // s : un passage est complet quand la capture l'a dépassé d'autant

    //--- capture ----------------------------------------------------------------------------------------------------
    /** Trames d'une passe de lecture, dans l'ordre. Une passe qui repasse sur une zone déjà capturée la remplace. */
    void addFrames (const std::vector<CaptureFrame>& batch)
    {
        if (batch.empty()) return;
        const double a = batch.front().time, b = batch.back().time;
        if (passEnd < 0.0 || a < passEnd - 0.1 || a > passEnd + 1.0)   // nouvelle passe (début, boucle, saut)
            passStart = a;
        // remplace ce que les passes précédentes avaient capturé sur [a, b]
        const auto lo = std::lower_bound (frames.begin(), frames.end(), a - 1.0e-9, byTime);
        const auto hi = std::upper_bound (frames.begin(), frames.end(), b + 1.0e-9, byTimeR);
        const auto pos = frames.erase (lo, hi);
        frames.insert (pos, batch.begin(), batch.end());
        passEnd = b;
        dirtyLo = std::min (dirtyLo, a);
        dirtyHi = std::max (dirtyHi, b);
        latest = b;
    }

    /** Segmente les passages complets de la zone capturée depuis le dernier appel (final : tout, capture arrêtée).
        Renvoie true si des notes ont changé. scaleMask (bit pc) / key : cible initiale = note de la gamme la plus proche. */
    bool segment (bool final, std::uint16_t scaleMask = 0x0fff)
    {
        if (dirtyLo > dirtyHi) return false;
        // étend la zone aux passages entiers qui la touchent
        double from = dirtyLo, to = dirtyHi;
        auto runs = voicedRuns (from - 1.0, to + 1.0);
        bool changed = false;
        double doneUntil = from;
        bool pendingLeft = false;
        for (const auto& [rs, re] : runs)
        {
            if (re < from || rs > to) continue;
            if (! final && re > latest - kSettle) { pendingLeft = true; dirtyLo = std::min (rs, dirtyLo); break; }
            removeBlobs (rs, re);
            buildBlobs (rs, re, scaleMask);
            doneUntil = std::max (doneUntil, re);
            changed = true;
        }
        // les notes d'une zone recapturée devenue silencieuse disparaissent aussi
        const double clearTo = pendingLeft ? dirtyLo : (final ? to : std::min (to, latest - kSettle));
        for (size_t i = 0; i < blobs.size();)
        {
            const auto& n = blobs[i];
            const bool inZone = n.startTimeSec < clearTo && n.endTimeSec() > from;
            const bool backed = hasVoicedFrames (n.startTimeSec, n.endTimeSec());
            if (inZone && ! backed) { blobs.erase (blobs.begin() + (long) i); changed = true; }
            else ++i;
        }
        if (! pendingLeft && (final || to <= latest - kSettle)) { dirtyLo = 1.0e300; dirtyHi = -1.0e300; }
        else if (! pendingLeft) dirtyLo = std::max (dirtyLo, doneUntil);
        return changed;
    }

    void clear()
    {
        blobs.clear(); frames.clear();
        dirtyLo = 1.0e300; dirtyHi = -1.0e300; passEnd = -1.0; latest = 0.0;
    }

    //--- édition ------------------------------------------------------------------------------------------------------
    std::vector<NoteBlob>& getBlobs() noexcept             { return blobs; }
    const std::vector<NoteBlob>& getBlobs() const noexcept { return blobs; }
    const std::vector<CaptureFrame>& getFrames() const noexcept { return frames; }

    /** Index de la note qui couvre t (−1 si aucune). */
    int noteAt (double t) const noexcept
    {
        for (size_t i = 0; i < blobs.size(); ++i)
            if (t >= blobs[i].startTimeSec && t < blobs[i].endTimeSec()) return (int) i;
        return -1;
    }

    /** Note de la gamme la plus proche (mask : bit pc, 0 = chromatique). */
    static double nearestInScale (double midi, std::uint16_t mask) noexcept
    {
        if (mask == 0) mask = 0x0fff;
        double best = std::round (midi), bestD = 1.0e9;
        for (int n = (int) std::floor (midi) - 6; n <= (int) std::floor (midi) + 7; ++n)
            if ((mask >> (((n % 12) + 12) % 12)) & 1u)
            {
                const double d = std::abs (midi - n);
                if (d < bestD) { bestD = d; best = n; }
            }
        return best;
    }

    void quantize (int index, std::uint16_t mask)
    {
        if (index < 0 || index >= (int) blobs.size()) return;
        blobs[(size_t) index].targetPitch = nearestInScale (blobs[(size_t) index].targetPitch, mask);
    }
    void quantizeAll (std::uint16_t mask)
    {
        for (auto& n : blobs) n.targetPitch = nearestInScale (n.originalCenterPitch, mask);
    }

    /** Scinde la note index à l'instant t : deux notes indépendantes (centres recalculés, mêmes réglages). */
    bool split (int index, double t)
    {
        if (index < 0 || index >= (int) blobs.size()) return false;
        NoteBlob& a = blobs[(size_t) index];
        if (t <= a.startTimeSec + kMinNote * 0.5 || t >= a.endTimeSec() - kMinNote * 0.5) return false;
        NoteBlob b = a;
        const size_t cut = std::min (a.pitchDeviationsCents.size(), (size_t) std::lround ((t - a.startTimeSec) * NoteBlob::kCurveRate));
        b.startTimeSec = t;
        b.durationSec = a.endTimeSec() - t;
        a.durationSec = t - a.startTimeSec;
        b.pitchDeviationsCents.assign (a.pitchDeviationsCents.begin() + (long) cut, a.pitchDeviationsCents.end());
        b.levelDb.assign (a.levelDb.begin() + (long) std::min (cut, a.levelDb.size()), a.levelDb.end());
        a.pitchDeviationsCents.resize (cut);
        a.levelDb.resize (std::min (cut, a.levelDb.size()));
        b.transitionInRatio = 0.0;
        recentre (a);
        recentre (b);
        const double shiftA = a.targetPitch - a.originalCenterPitch;   // garde l'écart cible / chanté de chaque moitié
        b.targetPitch = std::round (b.originalCenterPitch + shiftA);
        a.targetPitch = std::round (a.originalCenterPitch + shiftA);
        blobs.insert (blobs.begin() + index + 1, std::move (b));
        return true;
    }

    /** Hauteur rejouée (MIDI) de la note index à l'instant t : même formule que GraphSnapshot (pour l'affichage). */
    double resultPitchAt (int index, double t) const noexcept
    {
        const auto& n = blobs[(size_t) index];
        const double dev = n.deviationAt (t) / 100.0;
        double glide = 0.0;
        if (index > 0 && n.transitionInRatio > 0.0 && n.startTimeSec - blobs[(size_t) index - 1].endTimeSec() < 0.060)
        {
            const double gt = std::clamp (n.transitionInRatio, 0.0, 1.0) * std::min (0.200, 0.5 * n.durationSec);
            if (gt > 0.0 && t - n.startTimeSec < gt)
            {
                const double u = std::max (0.0, (t - n.startTimeSec) / gt);
                glide = (blobs[(size_t) index - 1].targetPitch - n.targetPitch) * (1.0 - u * u * (3.0 - 2.0 * u));
            }
        }
        return n.targetPitch + std::clamp (n.vibratoDepth, 0.0, 2.0) * dev + glide;
    }

    //--- vue audio ----------------------------------------------------------------------------------------------------
    std::unique_ptr<GraphSnapshot> buildSnapshot() const
    {
        auto s = std::make_unique<GraphSnapshot>();
        s->notes.reserve (blobs.size());
        size_t total = 0;
        for (const auto& n : blobs) total += n.pitchDeviationsCents.size();
        s->curves.reserve (total);
        for (size_t i = 0; i < blobs.size(); ++i)
        {
            const auto& n = blobs[i];
            GraphSnapshot::Note r;
            r.start = n.startTimeSec; r.end = n.endTimeSec();
            r.target = n.targetPitch; r.center = n.originalCenterPitch;
            r.depth = std::clamp (n.vibratoDepth, 0.0, 2.0);
            r.formant = std::clamp (n.formantOffsetSemitones, -12.0, 12.0);
            s->anyFormant = s->anyFormant || std::abs (r.formant) > 0.01;
            r.curveOffset = (std::uint32_t) s->curves.size();
            r.curveCount = (std::uint32_t) n.pitchDeviationsCents.size();
            s->curves.insert (s->curves.end(), n.pitchDeviationsCents.begin(), n.pitchDeviationsCents.end());
            if (i > 0 && n.transitionInRatio > 0.0 && n.startTimeSec - blobs[i - 1].endTimeSec() < 0.060)
            {
                r.glideTime = std::clamp (n.transitionInRatio, 0.0, 1.0) * std::min (0.200, 0.5 * n.durationSec);
                r.glideFrom = blobs[i - 1].targetPitch - n.targetPitch;
            }
            s->notes.push_back (r);
        }
        return s;
    }

    //--- sauvegarde (binaire compact, petit-boutiste, doubles exacts) -------------------------------------------------
    std::vector<std::uint8_t> serialize() const
    {
        std::vector<std::uint8_t> out;
        auto put = [&out] (const void* p, size_t n) { auto* b = (const std::uint8_t*) p; out.insert (out.end(), b, b + n); };
        auto u32 = [&] (std::uint32_t v) { std::uint8_t b[4] = { (std::uint8_t) v, (std::uint8_t) (v >> 8), (std::uint8_t) (v >> 16), (std::uint8_t) (v >> 24) }; put (b, 4); };
        auto f64 = [&] (double v) { std::uint64_t u; std::memcpy (&u, &v, 8); u32 ((std::uint32_t) u); u32 ((std::uint32_t) (u >> 32)); };
        auto f32 = [&] (float v) { std::uint32_t u; std::memcpy (&u, &v, 4); u32 (u); };
        put ("APGM", 4);
        u32 (1);
        u32 ((std::uint32_t) blobs.size());
        for (const auto& n : blobs)
        {
            for (double v : { n.startTimeSec, n.durationSec, n.originalCenterPitch, n.targetPitch, n.vibratoDepth,
                              n.transitionInRatio, n.formantOffsetSemitones }) f64 (v);
            u32 ((std::uint32_t) n.pitchDeviationsCents.size());
            for (float v : n.pitchDeviationsCents) f32 (v);
            u32 ((std::uint32_t) n.levelDb.size());
            for (float v : n.levelDb) f32 (v);
        }
        return out;
    }

    bool deserialize (const std::uint8_t* data, size_t size)
    {
        size_t pos = 0;
        bool ok = true;
        auto get = [&] (void* p, size_t n) { if (pos + n > size) { ok = false; std::memset (p, 0, n); return; } std::memcpy (p, data + pos, n); pos += n; };
        auto u32 = [&]() { std::uint8_t b[4]; get (b, 4); return (std::uint32_t) b[0] | ((std::uint32_t) b[1] << 8) | ((std::uint32_t) b[2] << 16) | ((std::uint32_t) b[3] << 24); };
        auto f64 = [&]() { std::uint64_t lo = u32(), hi = u32(); const std::uint64_t u = lo | (hi << 32); double v; std::memcpy (&v, &u, 8); return v; };
        auto f32 = [&]() { const std::uint32_t u = u32(); float v; std::memcpy (&v, &u, 4); return v; };
        char magic[4]; get (magic, 4);
        if (! ok || std::memcmp (magic, "APGM", 4) != 0 || u32() != 1) return false;
        const std::uint32_t count = u32();
        std::vector<NoteBlob> loaded;
        for (std::uint32_t k = 0; k < count && ok; ++k)
        {
            NoteBlob n;
            n.startTimeSec = f64(); n.durationSec = f64(); n.originalCenterPitch = f64(); n.targetPitch = f64();
            n.vibratoDepth = f64(); n.transitionInRatio = f64(); n.formantOffsetSemitones = f64();
            const std::uint32_t nc = u32();
            if (nc > (size - pos) / 4) return false;
            n.pitchDeviationsCents.resize (nc);
            for (auto& v : n.pitchDeviationsCents) v = f32();
            const std::uint32_t nl = u32();
            if (nl > (size - pos) / 4) return false;
            n.levelDb.resize (nl);
            for (auto& v : n.levelDb) v = f32();
            loaded.push_back (std::move (n));
        }
        if (! ok) return false;
        clear();
        blobs = std::move (loaded);
        return true;
    }

private:
    static bool byTime (const CaptureFrame& f, double t) { return f.time < t; }
    static bool byTimeR (double t, const CaptureFrame& f) { return t < f.time; }

    bool hasVoicedFrames (double a, double b) const
    {
        auto it = std::lower_bound (frames.begin(), frames.end(), a, byTime);
        for (; it != frames.end() && it->time < b; ++it) if (it->voiced) return true;
        return false;
    }

    std::vector<std::pair<double, double>> voicedRuns (double a, double b) const
    {
        std::vector<std::pair<double, double>> runs;
        auto it = std::lower_bound (frames.begin(), frames.end(), a, byTime);
        // remonte au vrai début du passage qui contient a (sinon il serait tronqué)
        double lastVoiced = it != frames.end() ? it->time : a;
        while (it != frames.begin())
        {
            const auto prev = it - 1;
            if (prev->voiced)
            {
                if (lastVoiced - prev->time > kBridgeGap) break;
                lastVoiced = prev->time;
            }
            else if (lastVoiced - prev->time > kBridgeGap) break;
            it = prev;
        }
        double rs = -1.0, last = -1.0;
        for (; it != frames.end() && it->time <= b; ++it)
        {
            if (! it->voiced) continue;
            if (rs < 0.0) rs = it->time;
            else if (it->time - last > kBridgeGap) { runs.emplace_back (rs, last); rs = it->time; }
            last = it->time;
        }
        if (rs >= 0.0) runs.emplace_back (rs, last);
        return runs;
    }

    void removeBlobs (double a, double b)
    {
        blobs.erase (std::remove_if (blobs.begin(), blobs.end(), [&] (const NoteBlob& n)
                                     { return n.startTimeSec < b + 1.0e-6 && n.endTimeSec() > a - 1.0e-6; }), blobs.end());
    }

    /** Hauteur chantée interpolée (trames voisées seulement) à l'instant t. */
    static double pitchAt (const std::vector<CaptureFrame>& v, double t)
    {
        auto it = std::lower_bound (v.begin(), v.end(), t, byTime);
        if (it == v.begin()) return v.front().midi;
        if (it == v.end()) return v.back().midi;
        const auto& b = *it; const auto& a = *(it - 1);
        const double f = b.time > a.time ? (t - a.time) / (b.time - a.time) : 0.0;
        return a.midi + f * (b.midi - a.midi);
    }
    static double levelAt (const std::vector<CaptureFrame>& v, double t)
    {
        auto it = std::lower_bound (v.begin(), v.end(), t, byTime);
        if (it == v.end()) return v.back().levelDb;
        return it->levelDb;
    }

    void buildBlobs (double rs, double re, std::uint16_t mask)
    {
        // trames voisées du passage
        std::vector<CaptureFrame> v;
        for (auto it = std::lower_bound (frames.begin(), frames.end(), rs, byTime); it != frames.end() && it->time <= re; ++it)
            if (it->voiced) v.push_back (*it);
        if (v.size() < 4) return;
        const double hop = (re - rs) / (double) std::max<size_t> (1, v.size() - 1);
        const double start = rs - 0.5 * hop, end = re + 0.5 * hop;
        if (end - start < kMinNote) return;

        // hauteur lissée (moyenne glissante de 150 ms) puis notes par hystérésis
        const size_t n = v.size();
        std::vector<double> smooth (n), light (n);
        {
            size_t a = 0, b = 0, la = 0, lb = 0;
            double sum = 0.0, lsum = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                while (b < n && v[b].time <= v[i].time + 0.5 * kSmoothSpan) sum += v[b++].midi;
                while (v[a].time < v[i].time - 0.5 * kSmoothSpan) sum -= v[a++].midi;
                smooth[i] = sum / (double) (b - a);
                while (lb < n && v[lb].time <= v[i].time + 0.010) lsum += v[lb++].midi;
                while (v[la].time < v[i].time - 0.010) lsum -= v[la++].midi;
                light[i] = lsum / (double) (lb - la);
            }
        }
        std::vector<int> label (n);
        int cur = (int) std::lround (smooth[0]);
        for (size_t i = 0; i < n; ++i)
        {
            if (std::abs (smooth[i] - cur) > kHysteresis) cur = (int) std::lround (smooth[i]);
            label[i] = cur;
        }
        // segments ; les trop courts rejoignent le voisin le plus proche en hauteur
        struct Seg { size_t a, b; int note; };
        std::vector<Seg> segs;
        for (size_t i = 0; i < n;)
        {
            size_t j = i;
            while (j < n && label[j] == label[i]) ++j;
            segs.push_back ({ i, j, label[i] });
            i = j;
        }
        for (bool merged = true; merged && segs.size() > 1;)
        {
            merged = false;
            for (size_t k = 0; k < segs.size(); ++k)
            {
                const double dur = v[segs[k].b - 1].time - v[segs[k].a].time;
                // segment de passage d'un glissé (hauteur entre ses deux voisins, < 150 ms) : il appartient aux voisins
                const bool transit = k > 0 && k + 1 < segs.size() && dur < 0.150
                                  && (segs[k - 1].note - segs[k].note) * (segs[k].note - segs[k + 1].note) > 0;
                if (dur >= kMinNote && ! transit) continue;
                size_t into = k == 0 ? 1 : k + 1 == segs.size() ? k - 1
                            : (std::abs (segs[k - 1].note - segs[k].note) <= std::abs (segs[k + 1].note - segs[k].note) ? k - 1 : k + 1);
                if (into < k) segs[into].b = segs[k].b; else segs[into].a = segs[k].a;
                segs.erase (segs.begin() + (long) k);
                merged = true;
                break;
            }
            for (size_t k = 1; k < segs.size();)       // voisins devenus identiques
            {
                if (segs[k].note == segs[k - 1].note) { segs[k - 1].b = segs[k].b; segs.erase (segs.begin() + (long) k); }
                else ++k;
            }
        }

        // frontières : là où la hauteur (lissée 20 ms) franchit le milieu des deux notes, près du changement
        std::vector<double> bounds { start };
        for (size_t k = 1; k < segs.size(); ++k)
        {
            const double mid = 0.5 * (segs[k - 1].note + segs[k].note);
            const bool up = segs[k].note > segs[k - 1].note;
            const size_t sw = segs[k].a;
            double t = v[sw].time;
            double bestD = 1.0e9;
            for (size_t i = segs[k - 1].a + 1; i < segs[k].b; ++i)
            {
                const bool crossed = up ? (light[i - 1] < mid && light[i] >= mid) : (light[i - 1] > mid && light[i] <= mid);
                if (! crossed) continue;
                const double f = (mid - light[i - 1]) / (light[i] - light[i - 1]);
                const double tc = v[i - 1].time + f * (v[i].time - v[i - 1].time);
                const double d = std::abs (tc - v[sw].time);
                if (d < bestD && d < 0.15) { bestD = d; t = tc; }
            }
            bounds.push_back (std::clamp (t, bounds.back() + 0.5 * kMinNote, end - 0.5 * kMinNote));
        }
        bounds.push_back (end);

        for (size_t k = 0; k + 1 < bounds.size(); ++k)
        {
            NoteBlob b;
            b.startTimeSec = bounds[k];
            b.durationSec = bounds[k + 1] - bounds[k];
            const size_t count = std::max<size_t> (1, (size_t) std::ceil (b.durationSec * NoteBlob::kCurveRate));
            std::vector<double> p (count);
            b.levelDb.resize (count);
            for (size_t i = 0; i < count; ++i)
            {
                // moyenne des trames du pas de 5 ms (le bruit de mesure baisse ; le vibrato n'est pas touché) ;
                // sinon interpolation
                const double t = std::min (b.startTimeSec + (double) i / NoteBlob::kCurveRate, re);
                const double half = 0.5 / NoteBlob::kCurveRate;
                double sum = 0.0; int cnt = 0;
                for (auto it = std::lower_bound (v.begin(), v.end(), t - half, byTime); it != v.end() && it->time < t + half; ++it)
                {
                    sum += it->midi; ++cnt;
                }
                p[i] = cnt >= 2 ? sum / cnt : pitchAt (v, std::max (t, rs));
                b.levelDb[i] = (float) levelAt (v, t);
            }
            b.pitchDeviationsCents.resize (count);
            b.originalCenterPitch = centreOf (p);
            for (size_t i = 0; i < count; ++i) b.pitchDeviationsCents[i] = (float) ((p[i] - b.originalCenterPitch) * 100.0);
            b.targetPitch = nearestInScale (b.originalCenterPitch, mask);
            blobs.push_back (std::move (b));
        }
        std::sort (blobs.begin(), blobs.end(), [] (const NoteBlob& x, const NoteBlob& y) { return x.startTimeSec < y.startTimeSec; });
    }

    /** Centre : moyenne de la partie centrale (60 %), hors attaque et relâchement. */
    static double centreOf (const std::vector<double>& p)
    {
        const size_t n = p.size();
        const size_t a = n >= 10 ? n / 5 : 0, b = n >= 10 ? n - n / 5 : n;
        double s = 0.0;
        for (size_t i = a; i < b; ++i) s += p[i];
        return s / (double) std::max<size_t> (1, b - a);
    }

    /** Recalcule le centre d'une note (après scission) : les écarts suivent. */
    static void recentre (NoteBlob& n)
    {
        std::vector<double> p (n.pitchDeviationsCents.size());
        for (size_t i = 0; i < p.size(); ++i) p[i] = n.originalCenterPitch + n.pitchDeviationsCents[i] / 100.0;
        if (p.empty()) return;
        const double c = centreOf (p);
        for (size_t i = 0; i < p.size(); ++i) n.pitchDeviationsCents[i] = (float) ((p[i] - c) * 100.0);
        n.originalCenterPitch = c;
    }

    std::vector<CaptureFrame> frames;
    std::vector<NoteBlob> blobs;
    double dirtyLo = 1.0e300, dirtyHi = -1.0e300, passStart = 0.0, passEnd = -1.0, latest = 0.0;
};

} // namespace aura::pitch

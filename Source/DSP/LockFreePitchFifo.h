// AuraPitch — NextDSP · AuraSuite
// LockFreePitchFifo.h : file SPSC sans verrou ni allocation, thread audio -> interface (courbe de pitch).
//
// Un seul producteur (processBlock) et un seul consommateur (timer 60/120 Hz de l'éditeur). Stockage statique
// (std::array) : aucune allocation, même à la construction. File pleine : la trame est perdue et comptée (l'interface
// ne doit jamais ralentir l'audio). Indices 64 bits monotones : pas de problème de rebouclage.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace aura::pitch
{

/** Une trame affichée par le graphe défilant : hauteur entrante (ambre/violet), hauteur corrigée (cyan),
    note cible et déviation en cents. */
struct PitchFrame
{
    std::int64_t samplePosition = 0;     // horloge d'entrée absolue (frameCentre du détecteur)
    float inputMidi      = 0.0f;         // hauteur entrante (demi-tons MIDI, référence A4 réglée)
    float outputMidi     = 0.0f;         // hauteur après correction, transposition et fine-tune
    float targetMidi     = 0.0f;         // note cible du ScaleQuantizer
    float deviationCents = 0.0f;         // entrée − cible
    float clarity        = 0.0f;
    float levelDb        = -200.0f;
    bool  voiced         = false;
};

template <typename T, int CapacityPow2>
class LockFreeSpscFifo
{
    static_assert (CapacityPow2 >= 2 && (CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacité : puissance de 2");
    static_assert (std::is_trivially_copyable_v<T>, "T doit être copiable trivialement");

public:
    static constexpr int kCapacity = CapacityPow2;

    /** Producteur uniquement. */
    bool push (const T& item) noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);
        const auto r = readIndex.load (std::memory_order_acquire);
        if (w - r >= (std::uint64_t) kCapacity)
        {
            dropped.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
        slots[(size_t) (w & kMask)] = item;
        writeIndex.store (w + 1, std::memory_order_release);
        return true;
    }

    /** Consommateur uniquement. */
    bool pop (T& item) noexcept
    {
        const auto r = readIndex.load (std::memory_order_relaxed);
        const auto w = writeIndex.load (std::memory_order_acquire);
        if (r == w)
            return false;
        item = slots[(size_t) (r & kMask)];
        readIndex.store (r + 1, std::memory_order_release);
        return true;
    }

    /** Consommateur : vide la file en appelant f(item) pour chaque élément, dans l'ordre. Renvoie le nombre lu. */
    template <typename F>
    int drain (F&& f) noexcept
    {
        const auto r0 = readIndex.load (std::memory_order_relaxed);
        const auto w  = writeIndex.load (std::memory_order_acquire);
        for (auto r = r0; r != w; ++r)
            f (static_cast<const T&> (slots[(size_t) (r & kMask)]));
        readIndex.store (w, std::memory_order_release);
        return (int) (w - r0);
    }

    int  getNumReady() const noexcept     { return (int) (writeIndex.load (std::memory_order_acquire) - readIndex.load (std::memory_order_acquire)); }
    std::uint64_t getNumDropped() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    static constexpr std::uint64_t kMask = (std::uint64_t) kCapacity - 1;

    std::array<T, (size_t) kCapacity> slots {};
    alignas (64) std::atomic<std::uint64_t> writeIndex { 0 };
    alignas (64) std::atomic<std::uint64_t> readIndex  { 0 };
    alignas (64) std::atomic<std::uint64_t> dropped    { 0 };
};

/** 2048 trames : à une trame toutes les 1,5 ms, 3 s d'avance si l'interface est gelée (redimensionnement, menu). */
using LockFreePitchFifo = LockFreeSpscFifo<PitchFrame, 2048>;

} // namespace aura::pitch

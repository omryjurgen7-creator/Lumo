#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>

// Shared, lock-free table living inside the plugin binary.
// Every Lumo instance in Live's process (same DLL / bundle) sees the same table:
// "Tap" instances write into it by name, the "Master" instance reads from it.
struct TapSlot
{
    std::atomic<uint64_t> id    { 0 };      // hash of the lower-cased, trimmed name (0 = free)
    std::atomic<float>    peak  { 0.0f };   // linear peak with decay
    std::atomic<float>    rmsSq { 0.0f };   // mean square, ~300 ms
    std::atomic<uint32_t> stamp { 0 };      // last update (ms counter)
};

class TapRegistry
{
public:
    static constexpr int numSlots = 64;

    static TapRegistry& get()
    {
        static TapRegistry instance;
        return instance;
    }

    static uint64_t hashName (const juce::String& name)
    {
        const auto s = name.trim().toLowerCase();
        if (s.isEmpty()) return 0;
        const auto h = (uint64_t) s.hashCode64();
        return h == 0 ? 1 : h;
    }

    int find (uint64_t id) const
    {
        if (id == 0) return -1;
        for (int i = 0; i < numSlots; ++i)
            if (slots[i].id.load() == id) return i;
        return -1;
    }

    int claim (uint64_t id)
    {
        if (id == 0) return -1;
        const int existing = find (id);
        if (existing >= 0) return existing;
        for (int i = 0; i < numSlots; ++i)
        {
            uint64_t expected = 0;
            if (slots[i].id.compare_exchange_strong (expected, id)) return i;
        }
        return -1;
    }

    void release (int idx, uint64_t id)
    {
        if (idx < 0 || idx >= numSlots) return;
        uint64_t expected = id;
        slots[idx].id.compare_exchange_strong (expected, 0);
    }

    TapSlot slots[numSlots];
};

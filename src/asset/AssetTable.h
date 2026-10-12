#pragma once

#include "core/Types.h"
#include "core/Assert.h"
#include "asset/AssetTypes.h"
#include <vector>
#include <bit>

namespace EngineN {

/**
 * Fixed-capacity generational Structure-of-Arrays (SoA) table.
 * Manages cache-friendly storage for hot presentation state and cold metadata.
 * Slot 0 is permanently reserved for fallback defaults (generation 0).
 */
template <typename T>
class AssetTable {
public:
    struct LookupEntry {
        AssetId id{0};
        u32     slot{0};
    };

    AssetTable() = default;

    void Init(u32 capacity) {
        ENGINE_ASSERT(capacity > 1, "AssetTable capacity must be at least 2 to accommodate fallback slot");
        ENGINE_ASSERT(capacity < (1u << AssetHandle<T>::kIndexBits), "AssetTable capacity must fit within handle index bit width");
        m_capacity = capacity;

        m_gen.assign(capacity, 1);
        m_gen[0] = 0; // Fallback slot permanently bound to generation 0

        m_hot.resize(capacity);
        m_state.assign(capacity, AssetState::Unloaded);
        m_state[0] = AssetState::Ready;

        m_cold.resize(capacity);

        // Pre-populate free stack: slots 1 to capacity-1
        m_freeList.resize(capacity - 1);
        for (u32 i = 0; i < capacity - 1; ++i) {
            // Push in reverse order so slot 1 is popped first
            m_freeList[i] = capacity - 1 - i;
        }

        // Open addressing lookup table: power-of-two size with load factor <= 0.5
        const u32 minLookupSize = std::max(16u, capacity * 2u);
        m_lookupMask = std::bit_ceil(minLookupSize) - 1u;
        m_lookup.assign(m_lookupMask + 1u, LookupEntry{});
        m_droppedAllocations = 0;
        m_retiredSlots = 0;
    }

    [[nodiscard]] AssetHandle<T> Find(AssetId id) const noexcept {
        if (!id.Valid() || m_lookup.empty()) {
            return AssetHandle<T>::Null();
        }

        const u32 slot = ProbeLookup(id);
        if (slot == 0) {
            return AssetHandle<T>::Null();
        }

        return AssetHandle<T>::FromParts(slot, m_gen[slot]);
    }

    [[nodiscard]] AssetHandle<T> Allocate(AssetId id) noexcept {
        if (!id.Valid()) {
            return AssetHandle<T>::Null();
        }

        // Deduplication: return active handle if already registered
        const u32 existingSlot = ProbeLookup(id);
        if (existingSlot != 0) {
            return AssetHandle<T>::FromParts(existingSlot, m_gen[existingSlot]);
        }

        // Capacity check: drop gracefully on overflow without allocating
        if (m_freeList.empty()) {
            m_droppedAllocations++;
            return AssetHandle<T>::Null();
        }

        const u32 slot = m_freeList.back();
        m_freeList.pop_back();

        InsertLookup(id, slot);

        m_state[slot] = AssetState::Loading;
        m_hot[slot] = m_hot[0]; // Mirror fallback during asynchronous or pending decode
        m_cold[slot].id = id;

        return AssetHandle<T>::FromParts(slot, m_gen[slot]);
    }

    void SetReady(u32 slot, const typename T::Hot& hotData, typename T::Cold coldData) noexcept {
        if (slot == 0 || slot >= m_capacity) return;
        coldData.id = m_cold[slot].id; // Preserve allocated identity to ensure RemoveLookup stays consistent
        m_hot[slot] = hotData;
        m_cold[slot] = std::move(coldData);
        m_state[slot] = AssetState::Ready;
    }

    void SetFailed(u32 slot) noexcept {
        if (slot == 0 || slot >= m_capacity) return;
        m_hot[slot] = m_hot[0]; // Ensure fallback is displayed on decode failure
        m_state[slot] = AssetState::Failed;
    }

    void Free(AssetHandle<T> handle) noexcept {
        const u32 slot = handle.Index();
        if (slot == 0 || slot >= m_capacity) return;
        if (m_gen[slot] != handle.Generation()) return;

        RemoveLookup(m_cold[slot].id);

        m_hot[slot] = m_hot[0];
        m_cold[slot] = typename T::Cold{};
        m_state[slot] = AssetState::Unloaded;

        // Advance generation: 12-bit ceiling; retire slot once exhausted
        constexpr u16 kMaxGen = AssetHandle<T>::kGenMask;
        if (m_gen[slot] < kMaxGen) {
            m_gen[slot]++;
            m_freeList.push_back(slot);
        } else {
            // Retired slot: do not re-add to free stack to avoid generation collision
            m_gen[slot] = 0;
            ++m_retiredSlots;
        }
    }

    void SetFallback(const typename T::Hot& fallbackHot, const typename T::Cold& fallbackCold) noexcept {
        m_hot[0] = fallbackHot;
        m_cold[0] = fallbackCold;
        m_state[0] = AssetState::Ready;
        m_gen[0] = 0;
    }

    [[nodiscard]] const typename T::Hot& GetHot(u32 slot) const noexcept {
        return m_hot[slot < m_capacity ? slot : 0];
    }

    [[nodiscard]] const typename T::Cold& GetCold(u32 slot) const noexcept {
        return m_cold[slot < m_capacity ? slot : 0];
    }

    [[nodiscard]] AssetState GetState(u32 slot) const noexcept {
        return m_state[slot < m_capacity ? slot : 0];
    }

    [[nodiscard]] u16 GetGen(u32 slot) const noexcept {
        return m_gen[slot < m_capacity ? slot : 0];
    }

    [[nodiscard]] const u16* GenData() const noexcept { return m_gen.data(); }
    [[nodiscard]] const typename T::Hot* HotData() const noexcept { return m_hot.data(); }
    [[nodiscard]] u32 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] u32 FreeCount() const noexcept { return static_cast<u32>(m_freeList.size()); }
    [[nodiscard]] u32 RetiredSlots() const noexcept { return m_retiredSlots; }
    [[nodiscard]] u32 ActiveCount() const noexcept {
        const u32 inactive = FreeCount() + m_retiredSlots;
        return (m_capacity > 1 && m_capacity > 1 + inactive) ? (m_capacity - 1 - inactive) : 0;
    }
    [[nodiscard]] u32 DroppedAllocations() const noexcept { return m_droppedAllocations; }

private:
    [[nodiscard]] usize HashSlot(AssetId id) const noexcept {
        // Multiplicative Fibonacci hash for 64-bit integer keys
        constexpr u64 kFibonacci = 11400714819323198485ULL;
        return static_cast<usize>((id.value * kFibonacci) >> 32) & m_lookupMask;
    }

    [[nodiscard]] u32 ProbeLookup(AssetId id) const noexcept {
        usize idx = HashSlot(id);
        const usize mask = m_lookupMask;
        for (usize i = 0; i <= mask; ++i) {
            const auto& entry = m_lookup[idx];
            if (entry.id == id) {
                return entry.slot;
            }
            if (!entry.id.Valid()) {
                return 0; // Empty slot terminates search
            }
            idx = (idx + 1) & mask;
        }
        return 0;
    }

    void InsertLookup(AssetId id, u32 slot) noexcept {
        usize idx = HashSlot(id);
        const usize mask = m_lookupMask;
        for (usize i = 0; i <= mask; ++i) {
            auto& entry = m_lookup[idx];
            if (!entry.id.Valid() || entry.id == id) {
                entry.id = id;
                entry.slot = slot;
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    void RemoveLookup(AssetId id) noexcept {
        if (!id.Valid()) return;
        usize idx = HashSlot(id);
        const usize mask = m_lookupMask;
        usize found = mask + 1;

        for (usize i = 0; i <= mask; ++i) {
            if (m_lookup[idx].id == id) {
                found = idx;
                break;
            }
            if (!m_lookup[idx].id.Valid()) {
                return;
            }
            idx = (idx + 1) & mask;
        }

        if (found > mask) return;

        // Backward-shift deletion avoids tombstone bloat in open addressing
        usize i = found;
        m_lookup[i].id = AssetId{0};
        m_lookup[i].slot = 0;

        usize j = i;
        while (true) {
            j = (j + 1) & mask;
            if (!m_lookup[j].id.Valid()) break;

            const usize k = HashSlot(m_lookup[j].id);
            // Check if element at j wants to move to empty slot i
            const usize distFromIdeal = (j + mask + 1 - k) & mask;
            const usize distFromEmpty = (j + mask + 1 - i) & mask;

            if (distFromIdeal >= distFromEmpty) {
                m_lookup[i] = m_lookup[j];
                m_lookup[j].id = AssetId{0};
                m_lookup[j].slot = 0;
                i = j;
            }
        }
    }

    u32                           m_capacity{0};
    u32                           m_lookupMask{0};
    u32                           m_droppedAllocations{0};
    u32                           m_retiredSlots{0};
    std::vector<u16>              m_gen;
    std::vector<typename T::Hot>  m_hot;
    std::vector<AssetState>       m_state;
    std::vector<typename T::Cold> m_cold;
    std::vector<u32>              m_freeList;
    std::vector<LookupEntry>      m_lookup;
};

} // namespace EngineN

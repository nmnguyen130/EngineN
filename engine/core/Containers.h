#pragma once

#include "core/Memory.h"

#include <algorithm>

namespace en {

// Open-addressed power-of-two Robin Hood hash map with backward-shift deletion.
// Non-owning view over an AlignedArena slab; uses no tombstones.
// Invariant: m_capacity is a power of two, key == 0 is reserved as empty, and dist == 0 marks an unused slot.
template <typename ValueT>
class alignas(8) RobinHoodMap {
public:
    struct alignas(8) Slot {
        u64 key{0};
        ValueT value{};
        u32 dist{0};
    };

    [[nodiscard]] bool Init(AlignedArena& arena, u32 minCapacity) noexcept {
        *this = {};
        if (minCapacity == 0U || minCapacity > 0x40000000U) {
            return false;
        }
        const u32 cap = std::max(16U, NextPow2(minCapacity));
        auto slots = arena.Carve<Slot>(cap);
        if (slots.empty()) {
            return false;
        }
        m_slots = slots.data();
        m_capacity = cap;
        return true;
    }

    [[nodiscard]] bool InitForExpectedItems(AlignedArena& arena, u32 expectedItems) noexcept {
        return (expectedItems != 0U && expectedItems <= 0x20000000U) && Init(arena, expectedItems * 2U);
    }

    constexpr void Clear() noexcept {
        std::fill_n(m_slots, m_capacity, Slot{});
        m_count = 0U;
    }

    [[nodiscard]] constexpr bool InsertOrAssign(u64 key, const ValueT& val) noexcept {
        if (key == 0ULL || m_slots == nullptr) {
            return false;
        }
        const u32 mask = m_capacity - 1U;
        const u32 maxLoad = MaxLoad();
        u32 idx = static_cast<u32>(Mix64(key)) & mask;
        u64 curKey = key;
        ValueT curVal = val;
        u32 curDist = 1U;
        bool displacing = false;

        for (u32 step = 0; step < m_capacity; ++step) {
            Slot& slot = m_slots[idx];
            if (slot.dist == 0U) {
                if (!displacing && m_count >= maxLoad) {
                    return false;
                }
                slot.key = curKey;
                slot.value = std::move(curVal);
                slot.dist = curDist;
                ++m_count;
                return true;
            }
            if (!displacing && slot.key == curKey) {
                slot.value = std::move(curVal);
                return true;
            }
            if (slot.dist < curDist) {
                if (!displacing && m_count >= maxLoad) {
                    return false;
                }
                displacing = true;
                std::swap(slot.key, curKey);
                std::swap(slot.value, curVal);
                std::swap(slot.dist, curDist);
            }
            idx = (idx + 1U) & mask;
            ++curDist;
        }
        return false;
    }

    [[nodiscard]] constexpr ValueT* Find(u64 key) noexcept {
        const u32 idx = FindSlotIndex(key);
        return (idx != kInvalidIndex) ? &m_slots[idx].value : nullptr;
    }

    [[nodiscard]] constexpr const ValueT* Find(u64 key) const noexcept {
        const u32 idx = FindSlotIndex(key);
        return (idx != kInvalidIndex) ? &m_slots[idx].value : nullptr;
    }

    [[nodiscard]] constexpr bool Contains(u64 key) const noexcept {
        return FindSlotIndex(key) != kInvalidIndex;
    }

    constexpr bool Erase(u64 key) noexcept {
        u32 idx = FindSlotIndex(key);
        if (idx == kInvalidIndex) {
            return false;
        }
        const u32 mask = m_capacity - 1U;
        for (u32 shift = 1U; shift < m_capacity; ++shift) {
            const u32 nextIdx = (idx + 1U) & mask;
            if (m_slots[nextIdx].dist <= 1U) {
                break;
            }
            m_slots[idx] = std::move(m_slots[nextIdx]);
            --m_slots[idx].dist;
            idx = nextIdx;
        }
        m_slots[idx] = Slot{};
        --m_count;
        return true;
    }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_count; }
    [[nodiscard]] constexpr u32 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u32 MaxLoad() const noexcept { return m_capacity - (m_capacity >> 3U); }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_count == 0U; }
    [[nodiscard]] constexpr std::span<const Slot> Slots() const noexcept { return {m_slots, m_capacity}; }

private:
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFU;

    // Stops probing as soon as a slot has a smaller probe distance than the search key (including 0 for empty).
    [[nodiscard]] constexpr u32 FindSlotIndex(u64 key) const noexcept {
        if (key == 0ULL || m_slots == nullptr) {
            return kInvalidIndex;
        }
        const u32 mask = m_capacity - 1U;
        u32 idx = static_cast<u32>(Mix64(key)) & mask;
        for (u32 dist = 1U; dist <= m_capacity; ++dist) {
            const Slot& slot = m_slots[idx];
            if (slot.dist < dist) {
                return kInvalidIndex;
            }
            if (slot.key == key) {
                return idx;
            }
            idx = (idx + 1U) & mask;
        }
        return kInvalidIndex;
    }

    Slot* m_slots{nullptr};
    u32 m_capacity{0};
    u32 m_count{0};
};
static_assert(sizeof(RobinHoodMap<u32>) == 16);
static_assert(sizeof(RobinHoodMap<u32>::Slot) == 16);

// Fixed-capacity power-of-two FIFO queue with wrapping 32-bit indices.
// Value type owning inline storage.
// Invariant: (m_tail - m_head) <= CapacityPow2.
template <typename T, usize CapacityPow2>
    requires (std::has_single_bit(CapacityPow2) && CapacityPow2 <= 0x80000000U)
class RingBuffer {
public:
    constexpr void Clear() noexcept {
        m_head = 0U;
        m_tail = 0U;
    }

    [[nodiscard]] constexpr bool Push(const T& item) noexcept {
        if (Full()) {
            return false;
        }
        m_storage[m_tail++ & kMask] = item;
        return true;
    }

    [[nodiscard]] constexpr bool Pop(T& outItem) noexcept {
        if (Empty()) {
            return false;
        }
        outItem = std::move(m_storage[m_head++ & kMask]);
        return true;
    }

    [[nodiscard]] constexpr T* PeekFront() noexcept {
        return Empty() ? nullptr : &m_storage[m_head & kMask];
    }

    [[nodiscard]] constexpr const T* PeekFront() const noexcept {
        return Empty() ? nullptr : &m_storage[m_head & kMask];
    }

    [[nodiscard]] constexpr T* PeekBack() noexcept {
        return Empty() ? nullptr : &m_storage[(m_tail - 1U) & kMask];
    }

    [[nodiscard]] constexpr const T* PeekBack() const noexcept {
        return Empty() ? nullptr : &m_storage[(m_tail - 1U) & kMask];
    }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_tail - m_head; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_head == m_tail; }
    [[nodiscard]] constexpr bool Full() const noexcept { return Size() >= CapacityPow2; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return CapacityPow2; }

private:
    static constexpr u32 kMask = static_cast<u32>(CapacityPow2 - 1U);
    std::array<T, CapacityPow2> m_storage{};
    u32 m_head{0};
    u32 m_tail{0};
};

} // namespace en

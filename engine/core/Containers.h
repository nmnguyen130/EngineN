#pragma once

#include "core/Memory.h"
#include <array>

namespace en {

// Open-addressed power-of-two Robin Hood hash table with backward-shift deletion.
// Non-owning view over AlignedArena slabs; zero heap allocations after Init.
// Invariant: m_capacity is a power of two and probe distance never exceeds 65534.
template <typename ValueT>
class RobinHoodMap {
public:
    struct alignas(16) Slot {
        u64 key{0};
        ValueT value{};
        u16 dist{0};
    };

    [[nodiscard]] bool Init(AlignedArena& arena, u32 minCapacity) noexcept {
        const u32 cap = std::max(16U, NextPow2(minCapacity));
        m_slots = arena.Carve<Slot>(cap);
        if (m_slots.empty()) return false;
        m_capacity = cap;
        m_mask = cap - 1U;
        m_count = 0;
        return true;
    }

    constexpr void Clear() noexcept {
        for (auto& s : m_slots) s = Slot{};
        m_count = 0;
    }

    [[nodiscard]] constexpr bool InsertOrAssign(u64 key, const ValueT& val) noexcept {
        if (key == 0 || m_slots.empty() || m_count * 10U >= m_capacity * 9U) return false;
        u32 idx = static_cast<u32>(Mix64(key)) & m_mask;
        u64 curKey = key;
        ValueT curVal = val;
        u16 curDist = 1;

        for (u32 step = 0; step < m_capacity; ++step) {
            Slot& slot = m_slots[idx];
            if (slot.dist == 0) {
                slot.key = curKey;
                slot.value = curVal;
                slot.dist = curDist;
                ++m_count;
                return true;
            }
            if (slot.key == curKey) {
                slot.value = curVal;
                return true;
            }
            if (slot.dist < curDist) {
                std::swap(slot.key, curKey);
                std::swap(slot.value, curVal);
                std::swap(slot.dist, curDist);
            }
            idx = (idx + 1U) & m_mask;
            ++curDist;
        }
        return false;
    }

    [[nodiscard]] constexpr ValueT* Find(u64 key) noexcept {
        if (key == 0 || m_slots.empty()) return nullptr;
        u32 idx = static_cast<u32>(Mix64(key)) & m_mask;
        u16 dist = 1;
        for (u32 step = 0; step < m_capacity; ++step) {
            Slot& slot = m_slots[idx];
            if (slot.dist == 0 || slot.dist < dist) return nullptr;
            if (slot.key == key) return &slot.value;
            idx = (idx + 1U) & m_mask;
            ++dist;
        }
        return nullptr;
    }

    [[nodiscard]] constexpr const ValueT* Find(u64 key) const noexcept {
        if (key == 0 || m_slots.empty()) return nullptr;
        u32 idx = static_cast<u32>(Mix64(key)) & m_mask;
        u16 dist = 1;
        for (u32 step = 0; step < m_capacity; ++step) {
            const Slot& slot = m_slots[idx];
            if (slot.dist == 0 || slot.dist < dist) return nullptr;
            if (slot.key == key) return &slot.value;
            idx = (idx + 1U) & m_mask;
            ++dist;
        }
        return nullptr;
    }

    constexpr bool Erase(u64 key) noexcept {
        if (key == 0 || m_slots.empty()) return false;
        u32 idx = static_cast<u32>(Mix64(key)) & m_mask;
        u16 dist = 1;
        for (u32 step = 0; step < m_capacity; ++step) {
            Slot& slot = m_slots[idx];
            if (slot.dist == 0 || slot.dist < dist) return false;
            if (slot.key == key) {
                u32 prevIdx = idx;
                for (u32 shift = 1; shift < m_capacity; ++shift) {
                    const u32 nextIdx = (prevIdx + 1U) & m_mask;
                    Slot& nextSlot = m_slots[nextIdx];
                    if (nextSlot.dist <= 1) {
                        m_slots[prevIdx] = Slot{};
                        break;
                    }
                    m_slots[prevIdx] = nextSlot;
                    --m_slots[prevIdx].dist;
                    prevIdx = nextIdx;
                }
                --m_count;
                return true;
            }
            idx = (idx + 1U) & m_mask;
            ++dist;
        }
        return false;
    }

    [[nodiscard]] constexpr u32 Size() const noexcept { return m_count; }
    [[nodiscard]] constexpr u32 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr std::span<const Slot> Slots() const noexcept { return m_slots; }

private:
    std::span<Slot> m_slots{};
    u32 m_capacity{0};
    u32 m_mask{0};
    u32 m_count{0};
};

// Fixed 256-bit mask with O(1) word-level bit scanning.
struct alignas(32) Bitset256 {
    std::array<u64, 4> words{};

    constexpr void Set(u32 bit) noexcept {
        if (bit < 256U) words[bit >> 6U] |= (1ULL << (bit & 63U));
    }
    constexpr void Clear(u32 bit) noexcept {
        if (bit < 256U) words[bit >> 6U] &= ~(1ULL << (bit & 63U));
    }
    [[nodiscard]] constexpr bool Test(u32 bit) const noexcept {
        return (bit < 256U) && ((words[bit >> 6U] & (1ULL << (bit & 63U))) != 0ULL);
    }
    constexpr void Reset() noexcept { words.fill(0ULL); }
    [[nodiscard]] constexpr u32 PopCount() const noexcept {
        return static_cast<u32>(std::popcount(words[0]) + std::popcount(words[1]) +
                                std::popcount(words[2]) + std::popcount(words[3]));
    }
};

// Stack-allocated small vector avoiding heap allocation for bounded collections.
template <typename T, usize MaxCapacity>
class SmallVector {
public:
    constexpr void Clear() noexcept { m_size = 0; }

    constexpr bool PushBack(const T& val) noexcept {
        if (m_size >= MaxCapacity) return false;
        m_data[m_size++] = val;
        return true;
    }

    constexpr void PopBack() noexcept {
        if (m_size > 0) --m_size;
    }

    [[nodiscard]] constexpr T& operator[](usize idx) noexcept { return m_data[idx]; }
    [[nodiscard]] constexpr const T& operator[](usize idx) const noexcept { return m_data[idx]; }
    [[nodiscard]] constexpr std::span<T> Span() noexcept { return std::span<T>(m_data.data(), m_size); }
    [[nodiscard]] constexpr std::span<const T> Span() const noexcept { return std::span<const T>(m_data.data(), m_size); }

    [[nodiscard]] constexpr usize Size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_size == 0; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return MaxCapacity; }

private:
    std::array<T, MaxCapacity> m_data{};
    usize m_size{0};
};

// Zero-allocation typed publish-subscribe event bus for decoupled subsystem messaging.
class EventBus {
public:
    struct alignas(32) Message {
        StringId topic{};
        u64 senderId{0};
        i64 paramA{0};
        f64 paramB{0.0};
    };
    using ListenerFn = void (*)(const Message& msg, void* userCtx) noexcept;

    struct Subscriber {
        StringId topic{};
        ListenerFn fn{nullptr};
        void* userCtx{nullptr};
    };

    constexpr void Clear() noexcept {
        m_subCount = 0;
        m_queueCount = 0;
    }

    constexpr bool Subscribe(StringId topic, ListenerFn fn, void* userCtx = nullptr) noexcept {
        if (!topic.IsValid() || !fn || m_subCount >= m_subscribers.size()) return false;
        m_subscribers[m_subCount++] = Subscriber{topic, fn, userCtx};
        return true;
    }

    constexpr bool PublishDeferred(const Message& msg) noexcept {
        if (m_queueCount >= m_queue.size()) return false;
        m_queue[m_queueCount++] = msg;
        return true;
    }

    void DispatchQueued() noexcept {
        const usize count = m_queueCount;
        m_queueCount = 0;
        for (usize i = 0; i < count; ++i) {
            const Message& m = m_queue[i];
            for (usize s = 0; s < m_subCount; ++s) {
                if (m_subscribers[s].topic == m.topic && m_subscribers[s].fn) {
                    m_subscribers[s].fn(m, m_subscribers[s].userCtx);
                }
            }
        }
    }

private:
    std::array<Subscriber, 64> m_subscribers{};
    std::array<Message, 128> m_queue{};
    usize m_subCount{0};
    usize m_queueCount{0};
};

} // namespace en

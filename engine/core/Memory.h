#pragma once

#include "core/Types.h"
#include <memory>
#include <new>

namespace en {

// Single-slab 64-byte cache-line-aligned linear arena allocator.
// Move-only RAII owner using C++23 over-aligned operator new/delete (std::align_val_t{64}) safe on Windows MSVC/MinGW.
// Invariant: m_capacity <= kMaxArenaBytes and every Carve<T>() returns a 64-byte aligned span.
class alignas(64) AlignedArena {
public:
    static constexpr usize kMaxArenaBytes = 512ULL * 1024ULL * 1024ULL;

    AlignedArena() noexcept = default;
    ~AlignedArena() noexcept { Shutdown(); }

    AlignedArena(const AlignedArena&) = delete;
    AlignedArena& operator=(const AlignedArena&) = delete;

    AlignedArena(AlignedArena&& other) noexcept
        : m_base(std::exchange(other.m_base, nullptr)),
          m_capacity(std::exchange(other.m_capacity, 0)),
          m_offset(std::exchange(other.m_offset, 0)),
          m_peakOffset(std::exchange(other.m_peakOffset, 0)),
          m_ownsMemory(std::exchange(other.m_ownsMemory, false)) {}

    AlignedArena& operator=(AlignedArena&& other) noexcept {
        if (this != &other) {
            Shutdown();
            m_base = std::exchange(other.m_base, nullptr);
            m_capacity = std::exchange(other.m_capacity, 0);
            m_offset = std::exchange(other.m_offset, 0);
            m_peakOffset = std::exchange(other.m_peakOffset, 0);
            m_ownsMemory = std::exchange(other.m_ownsMemory, false);
        }
        return *this;
    }

    [[nodiscard]] bool Init(usize capacityBytes) noexcept;
    void BindExternal(std::span<u8> externalSlice) noexcept;
    void Shutdown() noexcept;
    void Reset() noexcept;
    [[nodiscard]] u8* CarveRawBytes(usize rawBytes) noexcept;

    [[nodiscard]] constexpr usize SaveMarker() const noexcept { return m_offset; }

    constexpr void RollbackToMarker(usize marker) noexcept {
        if (marker <= m_offset) {
            m_offset = AlignUp(marker, 64);
        }
    }

    template <typename T>
    [[nodiscard]] std::span<T> Carve(usize count) noexcept {
        static_assert(alignof(T) <= 64);
        if (count == 0 || count > (kMaxArenaBytes / sizeof(T))) return {};
        u8* slicePtr = CarveRawBytes(sizeof(T) * count);
        if (!slicePtr) return {};
        T* typedPtr = reinterpret_cast<T*>(slicePtr);
        for (usize i = 0; i < count; ++i) {
            std::construct_at(typedPtr + i);
        }
        return std::span<T>(typedPtr, count);
    }

    [[nodiscard]] constexpr usize Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr usize UsedBytes() const noexcept { return m_offset; }
    [[nodiscard]] constexpr usize PeakBytes() const noexcept { return m_peakOffset; }
    [[nodiscard]] constexpr usize RemainingBytes() const noexcept { return m_capacity - m_offset; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_base != nullptr; }

private:
    u8* m_base{nullptr};
    usize m_capacity{0};
    usize m_offset{0};
    usize m_peakOffset{0};
    bool m_ownsMemory{false};
};

// RAII scope guard that rewinds an AlignedArena to its entry marker on destruction.
class ArenaTempScope {
public:
    explicit ArenaTempScope(AlignedArena& arena) noexcept
        : m_arena(arena), m_marker(arena.SaveMarker()) {}
    ~ArenaTempScope() noexcept { m_arena.RollbackToMarker(m_marker); }
    ArenaTempScope(const ArenaTempScope&) = delete;
    ArenaTempScope& operator=(const ArenaTempScope&) = delete;

private:
    AlignedArena& m_arena;
    usize m_marker{0};
};

// Ping-pong double-buffered arena for frame-scoped scratch allocations across consecutive frames.
class DoubleBufferedArena {
public:
    [[nodiscard]] bool Init(usize bytesPerBuffer) noexcept {
        m_activeIdx = 0;
        return m_arenas[0].Init(bytesPerBuffer) && m_arenas[1].Init(bytesPerBuffer);
    }

    void Shutdown() noexcept {
        m_arenas[0].Shutdown();
        m_arenas[1].Shutdown();
    }

    void SwapAndResetCurrent() noexcept {
        m_activeIdx ^= 1U;
        m_arenas[m_activeIdx].Reset();
    }

    [[nodiscard]] AlignedArena& Current() noexcept { return m_arenas[m_activeIdx]; }
    [[nodiscard]] const AlignedArena& Previous() const noexcept { return m_arenas[m_activeIdx ^ 1U]; }

private:
    AlignedArena m_arenas[2]{};
    u32 m_activeIdx{0};
};

// O(1) generational slot pool carved over an AlignedArena with intrusive freelist.
template <typename T, typename Tag = T>
class PoolAllocator {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u16 capacity) noexcept {
        if (capacity == 0) return false;
        m_items = arena.Carve<T>(capacity);
        m_generations = arena.Carve<u16>(capacity);
        m_freeNext = arena.Carve<u16>(capacity);
        if (m_items.empty() || m_generations.empty() || m_freeNext.empty()) return false;
        m_capacity = capacity;
        Clear();
        return true;
    }

    void Clear() noexcept {
        m_activeCount = 0;
        m_freeHead = 0;
        for (u16 i = 0; i < m_capacity; ++i) {
            m_generations[i] = 0;
            m_freeNext[i] = static_cast<u16>(i + 1U);
        }
        if (m_capacity > 0) m_freeNext[m_capacity - 1] = 0xFFFFU;
    }

    [[nodiscard]] Handle<Tag> Allocate(const T& initialValue = {}) noexcept {
        if (m_freeHead == 0xFFFFU) return {};
        const u16 idx = m_freeHead;
        m_freeHead = m_freeNext[idx];
        m_freeNext[idx] = kInUseSentinel;
        u16 nextGen = static_cast<u16>(m_generations[idx] + 1U);
        if (nextGen == 0) nextGen = 1U;
        m_generations[idx] = nextGen;
        m_items[idx] = initialValue;
        ++m_activeCount;
        return Handle<Tag>{idx, nextGen};
    }

    bool Free(Handle<Tag> h) noexcept {
        if (!IsValid(h)) return false;
        m_generations[h.index] = static_cast<u16>(m_generations[h.index] + 1U);
        m_freeNext[h.index] = m_freeHead;
        m_freeHead = h.index;
        --m_activeCount;
        m_items[h.index] = T{};
        return true;
    }

    [[nodiscard]] constexpr bool IsValid(Handle<Tag> h) const noexcept {
        return h.IsValid() && h.index < m_capacity && m_generations[h.index] == h.generation &&
               m_freeNext[h.index] == kInUseSentinel;
    }

    [[nodiscard]] constexpr T* TryGet(Handle<Tag> h) noexcept {
        return IsValid(h) ? &m_items[h.index] : nullptr;
    }
    [[nodiscard]] constexpr const T* TryGet(Handle<Tag> h) const noexcept {
        return IsValid(h) ? &m_items[h.index] : nullptr;
    }

    [[nodiscard]] constexpr u16 ActiveCount() const noexcept { return m_activeCount; }
    [[nodiscard]] constexpr u16 Capacity() const noexcept { return m_capacity; }

private:
    static constexpr u16 kInUseSentinel = 0xFFFEU;

    std::span<T> m_items{};
    std::span<u16> m_generations{};
    std::span<u16> m_freeNext{};
    u16 m_capacity{0};
    u16 m_activeCount{0};
    u16 m_freeHead{0xFFFFU};
};

// Fixed-capacity power-of-two ring buffer with zero dynamic memory allocation.
template <typename T, usize CapacityPow2>
class RingBuffer {
    static_assert(std::has_single_bit(CapacityPow2), "CapacityPow2 must be a power of two");

public:
    constexpr void Clear() noexcept {
        m_head = 0;
        m_tail = 0;
    }

    [[nodiscard]] constexpr bool Push(const T& item) noexcept {
        if (Size() >= CapacityPow2) return false;
        m_storage[m_tail & kMask] = item;
        ++m_tail;
        return true;
    }

    [[nodiscard]] constexpr bool Pop(T& outItem) noexcept {
        if (Empty()) return false;
        outItem = m_storage[m_head & kMask];
        ++m_head;
        return true;
    }

    [[nodiscard]] constexpr usize Size() const noexcept { return m_tail - m_head; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_head == m_tail; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return CapacityPow2; }

private:
    static constexpr usize kMask = CapacityPow2 - 1U;
    T m_storage[CapacityPow2]{};
    usize m_head{0};
    usize m_tail{0};
};

} // namespace en

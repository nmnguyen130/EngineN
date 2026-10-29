#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace en {

// Linear bump allocator backed by a single 64-byte-aligned memory slab.
// Move-only owner of the underlying slab; Rollback and Reset zero rewound regions.
// Invariant: m_offset <= m_capacity <= kMaxBytes, and [m_offset, m_capacity) is zeroed.
class Arena {
public:
    static constexpr usize kSlabAlignment = 64U;
    static constexpr u32   kMaxBytes      = 512U * 1024U * 1024U;

    constexpr Arena() noexcept = default;
    ~Arena() noexcept { Shutdown(); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    constexpr Arena(Arena&& other) noexcept
        : m_base(std::exchange(other.m_base, nullptr)),
          m_capacity(std::exchange(other.m_capacity, 0U)),
          m_offset(std::exchange(other.m_offset, 0U)) {}

    constexpr Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            Shutdown();
            m_base     = std::exchange(other.m_base, nullptr);
            m_capacity = std::exchange(other.m_capacity, 0U);
            m_offset   = std::exchange(other.m_offset, 0U);
        }
        return *this;
    }

    [[nodiscard]] bool Init(usize capacityBytes) noexcept {
        Shutdown();
        if (capacityBytes == 0U || capacityBytes > kMaxBytes) return false;
        const u32 alignedCap = static_cast<u32>(AlignUp(capacityBytes, kSlabAlignment));
        auto* raw = static_cast<u8*>(
            ::operator new(alignedCap, std::align_val_t{kSlabAlignment}, std::nothrow));
        if (!raw) return false;
        std::memset(raw, 0, alignedCap);
        m_base     = raw;
        m_capacity = alignedCap;
        m_offset   = 0U;
        return true;
    }

    void Shutdown() noexcept {
        if (m_base) {
            ::operator delete(m_base, std::align_val_t{kSlabAlignment});
            m_base = nullptr;
        }
        m_capacity = 0U;
        m_offset   = 0U;
    }

    void Reset() noexcept { Rollback(0U); }

    [[nodiscard]] u8* AllocBytes(usize bytes, usize alignment = 8U) noexcept {
        if (bytes == 0U || !m_base || bytes > kMaxBytes ||
            !std::has_single_bit(alignment) || alignment > kSlabAlignment) {
            return nullptr;
        }
        const usize alignedOff = AlignUp(m_offset, alignment);
        if (alignedOff > m_capacity || bytes > (m_capacity - alignedOff)) return nullptr;
        m_offset = static_cast<u32>(alignedOff + bytes);
        return m_base + alignedOff;
    }

    template <typename T>
    [[nodiscard]] std::span<T> Alloc(usize count = 1U) noexcept {
        static_assert(alignof(T) <= kSlabAlignment);
        static_assert(std::is_trivially_destructible_v<T>, "Arena::Alloc requires trivially destructible T");
        if (count == 0U || count > (kMaxBytes / sizeof(T))) return {};
        u8* ptr = AllocBytes(sizeof(T) * count, std::max<usize>(alignof(T), 8U));
        if (!ptr) return {};
        T* typed = reinterpret_cast<T*>(ptr);
        for (usize i = 0U; i < count; ++i) std::construct_at(typed + i);
        return {typed, count};
    }

    [[nodiscard]] constexpr u32 Mark() const noexcept { return m_offset; }

    void Rollback(u32 mark) noexcept {
        if (mark < m_offset && m_base) {
            std::memset(m_base + mark, 0, m_offset - mark);
            m_offset = mark;
        }
    }

    [[nodiscard]] constexpr u8*  Base() const noexcept { return m_base; }
    [[nodiscard]] constexpr u32  Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u32  Used() const noexcept { return m_offset; }
    [[nodiscard]] constexpr u32  Remaining() const noexcept { return m_capacity - m_offset; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_base != nullptr; }

private:
    u8* m_base{nullptr};
    u32 m_capacity{0};
    u32 m_offset{0};
};
static_assert(sizeof(Arena) == 16);

// RAII scope guard that rewinds an Arena to its saved marker on destruction.
// Non-owning stack guard; must not outlive the referenced Arena.
class ArenaScope {
public:
    explicit ArenaScope(Arena& arena) noexcept : m_arena(arena), m_mark(arena.Mark()) {}
    ~ArenaScope() noexcept { m_arena.Rollback(m_mark); }

    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;

private:
    Arena& m_arena;
    u32    m_mark{0};
};

} // namespace en

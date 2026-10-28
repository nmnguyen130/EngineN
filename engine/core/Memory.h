#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace en {

namespace detail {

// Clears 64-byte aligned slabs in 8-byte words to avoid GCC VRP false-positive stringop warnings.
inline void ZeroAlignedBlocks(u8* base, usize alignedByteCount) noexcept {
    std::fill_n(reinterpret_cast<u64*>(base), (alignedByteCount & 0x3FFFFFFFULL) >> 3U, 0ULL);
}

template <typename T>
[[nodiscard]] constexpr T NormalizeLittleEndian(T value) noexcept {
    if constexpr (std::endian::native == std::endian::big && sizeof(T) > 1U && std::is_arithmetic_v<T>) {
        if constexpr (std::is_floating_point_v<T> && sizeof(T) == 4U) {
            return std::bit_cast<T>(std::byteswap(std::bit_cast<u32>(value)));
        } else if constexpr (std::is_floating_point_v<T> && sizeof(T) == 8U) {
            return std::bit_cast<T>(std::byteswap(std::bit_cast<u64>(value)));
        } else if constexpr (std::is_integral_v<T>) {
            return std::byteswap(value);
        }
    }
    return value;
}

} // namespace detail

// Linear arena allocator backed by a single 64-byte-aligned memory slab.
// Move-only owner of the underlying slab; Reset and RollbackToMarker zero rewound regions.
// Invariant: m_offset <= m_capacity <= kMaxArenaBytes, both are multiples of 64, and [m_offset, m_capacity) is zeroed.
class alignas(16) AlignedArena {
public:
    static constexpr u32 kCacheLineBytes = 64U;
    static constexpr u32 kMaxArenaBytes = 512U * 1024U * 1024U;

    constexpr AlignedArena() noexcept = default;
    ~AlignedArena() noexcept { Shutdown(); }

    AlignedArena(const AlignedArena&) = delete;
    AlignedArena& operator=(const AlignedArena&) = delete;

    constexpr AlignedArena(AlignedArena&& other) noexcept { *this = std::move(other); }

    constexpr AlignedArena& operator=(AlignedArena&& other) noexcept {
        if (this != &other) {
            Shutdown();
            m_base = std::exchange(other.m_base, nullptr);
            m_capacity = std::exchange(other.m_capacity, 0U);
            m_offset = std::exchange(other.m_offset, 0U);
        }
        return *this;
    }

    [[nodiscard]] bool Init(usize capacityBytes) noexcept {
        Shutdown();
        if (capacityBytes == 0U || capacityBytes > kMaxArenaBytes) {
            return false;
        }
        const u32 alignedCap = static_cast<u32>(AlignUp(capacityBytes, kCacheLineBytes));
        auto* raw = static_cast<u8*>(
            ::operator new(alignedCap, std::align_val_t{kCacheLineBytes}, std::nothrow));
        if (!raw) {
            return false;
        }
        detail::ZeroAlignedBlocks(raw, alignedCap);
        m_base = raw;
        m_capacity = alignedCap;
        m_offset = 0U;
        return true;
    }

    constexpr void Shutdown() noexcept {
        if (m_base != nullptr) {
            ::operator delete(m_base, std::align_val_t{kCacheLineBytes});
            m_base = nullptr;
        }
        m_capacity = 0U;
        m_offset = 0U;
    }

    void Reset() noexcept { RollbackToMarker(0U); }

    [[nodiscard]] u8* CarveRawBytes(usize rawBytes) noexcept {
        if (rawBytes == 0U || m_base == nullptr || rawBytes > kMaxArenaBytes) {
            return nullptr;
        }
        const u32 paddedBytes = static_cast<u32>(AlignUp(rawBytes, kCacheLineBytes));
        if (paddedBytes > (m_capacity - m_offset)) {
            return nullptr;
        }
        u8* slicePtr = m_base + m_offset;
        m_offset += paddedBytes;
        return slicePtr;
    }

    template <typename T>
    [[nodiscard]] std::span<T> Carve(usize count) noexcept {
        static_assert(alignof(T) <= kCacheLineBytes);
        static_assert(std::is_trivially_destructible_v<T>, "AlignedArena::Carve requires trivially destructible types");
        if (count == 0U || count > (kMaxArenaBytes / sizeof(T))) {
            return {};
        }
        u8* slicePtr = CarveRawBytes(sizeof(T) * count);
        if (!slicePtr) {
            return {};
        }
        T* typedPtr = reinterpret_cast<T*>(slicePtr);
        for (usize i = 0; i < count; ++i) {
            std::construct_at(typedPtr + i);
        }
        return {typedPtr, count};
    }

    [[nodiscard]] constexpr u32 SaveMarker() const noexcept { return m_offset; }

    void RollbackToMarker(u32 marker) noexcept {
        if (marker < m_offset && m_base != nullptr) {
            const u32 alignedMarker = static_cast<u32>(AlignUp(marker, kCacheLineBytes));
            if (alignedMarker < m_offset) {
                detail::ZeroAlignedBlocks(m_base + alignedMarker, m_offset - alignedMarker);
                m_offset = alignedMarker;
            }
        }
    }

    [[nodiscard]] constexpr u32 Capacity() const noexcept { return m_capacity; }
    [[nodiscard]] constexpr u32 UsedBytes() const noexcept { return m_offset; }
    [[nodiscard]] constexpr u32 RemainingBytes() const noexcept { return m_capacity - m_offset; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_base != nullptr; }

private:
    u8* m_base{nullptr};
    u32 m_capacity{0};
    u32 m_offset{0};
};
static_assert(sizeof(AlignedArena) == 16);

// Scope guard that rewinds an AlignedArena to its saved marker on destruction.
// Non-owning stack guard; must not outlive the referenced AlignedArena.
// Invariant: m_arena is rewound to AlignUp(m_marker, 64) when the scope exits.
class alignas(8) ArenaTempScope {
public:
    explicit ArenaTempScope(AlignedArena& arena) noexcept
        : m_arena(arena),
          m_marker(arena.SaveMarker()) {}

    ~ArenaTempScope() noexcept { m_arena.RollbackToMarker(m_marker); }

    ArenaTempScope(const ArenaTempScope&) = delete;
    ArenaTempScope& operator=(const ArenaTempScope&) = delete;

private:
    AlignedArena& m_arena;
    u32 m_marker{0};
};
static_assert(sizeof(ArenaTempScope) == 16);

// Bounds-checked little-endian binary writer over a caller-supplied byte span.
// Non-owning view; never reallocates or writes past m_capacity.
// Invariant: once m_ok becomes false, subsequent writes fail without modifying the buffer.
class alignas(8) BinaryWriter {
public:
    constexpr explicit BinaryWriter(std::span<u8> buffer) noexcept
        : m_data(buffer.data()),
          m_capacity(static_cast<u32>(std::min<usize>(buffer.size(), 0x7FFFFFFFU))) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    bool Write(const T& value) noexcept {
        const T leVal = detail::NormalizeLittleEndian(value);
        return WriteBytes({reinterpret_cast<const u8*>(&leVal), sizeof(T)});
    }

    bool WriteBytes(std::span<const u8> bytes) noexcept {
        if (!m_ok || bytes.size() > Remaining()) {
            m_ok = false;
            return false;
        }
        if (!bytes.empty()) {
            std::memcpy(m_data + m_offset, bytes.data(), bytes.size());
            m_offset += static_cast<u32>(bytes.size());
        }
        return true;
    }

    bool WriteString(std::string_view sv) noexcept {
        if (!m_ok || sv.size() > 0xFFFFU || (2U + sv.size()) > Remaining()) {
            m_ok = false;
            return false;
        }
        return Write(static_cast<u16>(sv.size())) &&
               WriteBytes({reinterpret_cast<const u8*>(sv.data()), sv.size()});
    }

    [[nodiscard]] constexpr std::span<const u8> WrittenSpan() const noexcept { return {m_data, m_offset}; }
    [[nodiscard]] constexpr u32 Offset() const noexcept { return m_offset; }
    [[nodiscard]] constexpr u32 Remaining() const noexcept { return m_capacity - m_offset; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return m_ok; }

private:
    u8* m_data{nullptr};
    u32 m_capacity{0};
    u32 m_offset{0};
    bool m_ok{true};
};
static_assert(sizeof(BinaryWriter) == 24);

// Bounds-checked little-endian binary reader over a read-only byte span.
// Non-owning view; never reads past m_size.
// Invariant: once m_ok becomes false, subsequent reads fail and leave output arguments unchanged.
class alignas(8) BinaryReader {
public:
    constexpr explicit BinaryReader(std::span<const u8> buffer) noexcept
        : m_data(buffer.data()),
          m_size(static_cast<u32>(std::min<usize>(buffer.size(), 0x7FFFFFFFU))) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] bool Read(T& outValue) noexcept {
        if (!m_ok || sizeof(T) > Remaining()) {
            m_ok = false;
            return false;
        }
        T tmp{};
        std::memcpy(&tmp, m_data + m_offset, sizeof(T));
        m_offset += static_cast<u32>(sizeof(T));
        outValue = detail::NormalizeLittleEndian(tmp);
        return true;
    }

    [[nodiscard]] bool ReadBytes(std::span<u8> outBytes) noexcept {
        if (!m_ok || outBytes.size() > Remaining()) {
            m_ok = false;
            return false;
        }
        if (!outBytes.empty()) {
            std::memcpy(outBytes.data(), m_data + m_offset, outBytes.size());
            m_offset += static_cast<u32>(outBytes.size());
        }
        return true;
    }

    [[nodiscard]] constexpr bool Skip(u32 byteCount) noexcept {
        if (!m_ok || byteCount > Remaining()) {
            m_ok = false;
            return false;
        }
        m_offset += byteCount;
        return true;
    }

    [[nodiscard]] bool ReadStringView(std::string_view& outSv) noexcept {
        const u32 savedOffset = m_offset;
        u16 len = 0;
        if (!Read(len) || len > Remaining()) {
            m_offset = savedOffset;
            m_ok = false;
            return false;
        }
        outSv = {reinterpret_cast<const char*>(m_data + m_offset), len};
        m_offset += len;
        return true;
    }

    [[nodiscard]] constexpr u32 Offset() const noexcept { return m_offset; }
    [[nodiscard]] constexpr u32 Remaining() const noexcept { return m_size - m_offset; }
    [[nodiscard]] constexpr bool Ok() const noexcept { return m_ok; }

private:
    const u8* m_data{nullptr};
    u32 m_size{0};
    u32 m_offset{0};
    bool m_ok{true};
};
static_assert(sizeof(BinaryReader) == 24);

} // namespace en

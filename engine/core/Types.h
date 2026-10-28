#pragma once

#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace en {

using u8    = std::uint8_t;
using u16   = std::uint16_t;
using u32   = std::uint32_t;
using u64   = std::uint64_t;
using i8    = std::int8_t;
using i16   = std::int16_t;
using i32   = std::int32_t;
using i64   = std::int64_t;
using usize = std::size_t;
using f32   = float;
using f64   = double;

template <typename T, typename E>
using Result = std::expected<T, E>;

template <typename E>
[[nodiscard]] constexpr std::unexpected<E> Err(E error) noexcept {
    return std::unexpected<E>(error);
}

template <typename E>
concept BitmaskEnum = std::is_enum_v<E> && requires {
    { enable_bitmask_operators(E{}) } -> std::same_as<bool>;
};

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator|(E lhs, E rhs) noexcept {
    return static_cast<E>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator&(E lhs, E rhs) noexcept {
    return static_cast<E>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator^(E lhs, E rhs) noexcept {
    return static_cast<E>(std::to_underlying(lhs) ^ std::to_underlying(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator~(E val) noexcept {
    return static_cast<E>(~std::to_underlying(val));
}

template <BitmaskEnum E>
constexpr E& operator|=(E& lhs, E rhs) noexcept { return lhs = lhs | rhs; }

template <BitmaskEnum E>
constexpr E& operator&=(E& lhs, E rhs) noexcept { return lhs = lhs & rhs; }

template <BitmaskEnum E>
constexpr E& operator^=(E& lhs, E rhs) noexcept { return lhs = lhs ^ rhs; }

template <BitmaskEnum E>
[[nodiscard]] constexpr bool HasFlag(E value, E flag) noexcept {
    return (std::to_underlying(value) & std::to_underlying(flag)) != 0;
}

// Next power of two. Returns 0 when the result does not fit in u32.
[[nodiscard]] constexpr u32 NextPow2(u32 value) noexcept {
    if (value <= 1U) return 1U;
    if (value > 0x80000000U) return 0U;
    return std::bit_ceil(value);
}

// `alignment` must be a non-zero power of two. Returns 0 on invalid alignment or overflow.
[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) noexcept {
    if (alignment == 0U || !std::has_single_bit(alignment)) return 0U;
    const usize mask = alignment - 1U;
    if (value > ~mask) return 0U;
    return (value + mask) & ~mask;
}

[[nodiscard]] constexpr u64 Mix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

[[nodiscard]] constexpr u64 HashCombine64(u64 seed, u64 val) noexcept {
    return Mix64(seed ^ Mix64(val));
}

namespace detail {

consteval std::array<u32, 256> BuildCrc32Table() noexcept {
    std::array<u32, 256> table{};
    for (u32 i = 0U; i < 256U; ++i) {
        u32 crc = i;
        for (u32 bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr auto kCrc32Table = BuildCrc32Table();

} // namespace detail

// IEEE 802.3 CRC32. Supports incremental chaining via previousCrc.
[[nodiscard]] constexpr u32 ComputeCrc32(std::span<const u8> bytes, u32 previousCrc = 0U) noexcept {
    u32 crc = ~previousCrc;
    for (const u8 b : bytes) {
        crc = (crc >> 8U) ^ detail::kCrc32Table[(crc ^ b) & 0xFFU];
    }
    return ~crc;
}


// Case-insensitive, slash-normalized 64-bit FNV-1a for symbols and asset paths.
// Empty → 0. Backslash → '/', ASCII upper → lower.
struct alignas(8) StringId {
    u64 value{0};

    constexpr StringId() noexcept = default;
    constexpr explicit StringId(u64 raw) noexcept : value(raw) {}

    [[nodiscard]] static constexpr StringId Hash(std::string_view str) noexcept {
        if (str.empty()) return StringId{0};
        u64 h = 0xCBF29CE484222325ULL;
        for (const char ch : str) {
            char norm = (ch == '\\') ? '/' : ch;
            if (norm >= 'A' && norm <= 'Z') {
                norm = static_cast<char>(norm + ('a' - 'A'));
            }
            h ^= static_cast<u8>(norm);
            h *= 0x100000001B3ULL;
        }
        return StringId{h};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const StringId&) const noexcept = default;
};
static_assert(std::endian::native == std::endian::little, "Engine core requires little-endian");
static_assert(sizeof(StringId) == 8);

inline namespace literals {
[[nodiscard]] consteval StringId operator""_sid(const char* str, usize len) noexcept {
    return StringId::Hash(std::string_view{str, len});
}
} // namespace literals

// Type-tagged 32-bit generational handle. generation == 0 is invalid.
template <typename Tag>
struct alignas(4) Handle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }

    [[nodiscard]] constexpr u32 Packed() const noexcept {
        return (static_cast<u32>(generation) << 16U) | static_cast<u32>(index);
    }

    [[nodiscard]] static constexpr Handle FromPacked(u32 packed) noexcept {
        return Handle{
            static_cast<u16>(packed & 0xFFFFU),
            static_cast<u16>(packed >> 16U),
        };
    }

    [[nodiscard]] constexpr auto operator<=>(const Handle&) const noexcept = default;
};
static_assert(sizeof(Handle<void>) == 4);

// Fixed-capacity stack string. sizeof(FixedString<N>) == N.
// Failed ops leave the object unchanged.
template <usize CapacityBytes>
    requires (CapacityBytes >= 8 && CapacityBytes <= 65536 && (CapacityBytes % 2U) == 0U)
class alignas(2) FixedString {
public:
    static constexpr usize kMaxLength = CapacityBytes - 3U;

    constexpr FixedString() noexcept = default;

    constexpr explicit FixedString(std::string_view sv) noexcept {
        (void)Assign(sv);
    }

    constexpr bool Assign(std::string_view sv) noexcept {
        if (sv.size() > kMaxLength || sv.contains('\0')) return false;
        if (!sv.empty()) {
            std::char_traits<char>::move(m_buf.data(), sv.data(), sv.size());
        }
        m_len = static_cast<u16>(sv.size());
        m_buf[m_len] = '\0';
        return true;
    }

    [[nodiscard]] constexpr bool Append(std::string_view sv) noexcept {
        if (m_len > kMaxLength || sv.size() > (kMaxLength - m_len) || sv.contains('\0')) {
            return false;
        }
        if (!sv.empty()) {
            std::char_traits<char>::move(m_buf.data() + m_len, sv.data(), sv.size());
            m_len = static_cast<u16>(m_len + sv.size());
        }
        m_buf[m_len] = '\0';
        return true;
    }

    // Normalize separators and ensure trailing '/'. Rejects control chars.
    [[nodiscard]] constexpr bool AssignNormalizedDir(std::string_view sv) noexcept {
        if (sv.empty() || sv.size() > kMaxLength) return false;
        FixedString candidate{};
        for (const char c : sv) {
            if (static_cast<u8>(c) < 0x20U || static_cast<u8>(c) == 0x7FU) return false;
            candidate.m_buf[candidate.m_len++] = (c == '\\') ? '/' : c;
        }
        if (candidate.m_buf[candidate.m_len - 1U] != '/') {
            if (candidate.m_len >= kMaxLength) return false;
            candidate.m_buf[candidate.m_len++] = '/';
        }
        candidate.m_buf[candidate.m_len] = '\0';
        *this = candidate;
        return true;
    }

    // Join relative path. Rejects "..", ".", empty segments, drive colons, absolute paths.
    [[nodiscard]] constexpr bool Resolve(std::string_view rel, FixedString& out) const noexcept {
        if (rel.empty() || rel.front() == '/' || rel.front() == '\\' ||
            rel.back() == '/' || rel.back() == '\\' || m_len >= kMaxLength) {
            return false;
        }
        const usize sepLen = (m_len > 0 && m_buf[m_len - 1U] != '/') ? 1U : 0U;
        if (m_len + sepLen >= kMaxLength || rel.size() > (kMaxLength - (m_len + sepLen))) {
            return false;
        }

        FixedString candidate = *this;
        if (sepLen != 0U) {
            candidate.m_buf[candidate.m_len++] = '/';
        }

        usize segLen = 0;
        usize dotCount = 0;
        for (const char c : rel) {
            const u8 byte = static_cast<u8>(c);
            if (byte < 0x20U || byte == 0x7FU || c == ':') return false;
            const char norm = (c == '\\') ? '/' : c;
            if (norm == '/') {
                if (segLen == 0 || (dotCount == segLen && segLen <= 2U)) return false;
                segLen = 0;
                dotCount = 0;
            } else {
                ++segLen;
                if (norm == '.') ++dotCount;
            }
            candidate.m_buf[candidate.m_len++] = norm;
        }
        if (segLen == 0 || (dotCount == segLen && segLen <= 2U)) return false;
        candidate.m_buf[candidate.m_len] = '\0';
        out = candidate;
        return true;
    }

    constexpr void Clear() noexcept {
        m_len = 0;
        m_buf[0] = '\0';
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf.data(), m_len}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return m_buf.data(); }
    [[nodiscard]] constexpr u16 Size() const noexcept { return m_len; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_len == 0; }
    [[nodiscard]] static constexpr usize Capacity() noexcept { return kMaxLength; }

    [[nodiscard]] constexpr bool operator==(const FixedString& other) const noexcept {
        return View() == other.View();
    }
    [[nodiscard]] constexpr bool operator==(std::string_view sv) const noexcept {
        return View() == sv;
    }

private:
    u16 m_len{0};
    std::array<char, CapacityBytes - 2U> m_buf{};
};
static_assert(sizeof(FixedString<256>) == 256);

// Deterministic 64-bit SplitMix + Lemire unbiased bounded sampling.
class alignas(8) Rng64 {
public:
    static constexpr u64 kDefaultSeed = 0x853C49E6748FEA9BULL;

    constexpr explicit Rng64(u64 seed = kDefaultSeed) noexcept : m_state(seed) {}

    [[nodiscard]] constexpr u64 NextU64() noexcept {
        m_state += 0x9E3779B97F4A7C15ULL;
        return Mix64(m_state);
    }

    [[nodiscard]] constexpr u32 NextU32() noexcept {
        return static_cast<u32>(NextU64() >> 32U);
    }

    [[nodiscard]] constexpr f32 NextF32() noexcept {
        return static_cast<f32>(NextU32() >> 8U) * (1.0f / 16777216.0f);
    }

    [[nodiscard]] constexpr u32 NextBoundedU32(u32 bound) noexcept {
        if (bound <= 1U) return 0U;
        const u32 threshold = (0U - bound) % bound;
        for (;;) {
            const u64 product = static_cast<u64>(NextU32()) * static_cast<u64>(bound);
            if (static_cast<u32>(product) >= threshold) {
                return static_cast<u32>(product >> 32U);
            }
        }
    }

    [[nodiscard]] constexpr f32 RangeF32(f32 lo, f32 hi) noexcept {
        return (hi <= lo) ? lo : (lo + (hi - lo) * NextF32());
    }

    [[nodiscard]] constexpr i32 RangeI32(i32 lo, i32 hi) noexcept {
        if (hi <= lo) return lo;
        const u64 span = static_cast<u64>(static_cast<i64>(hi) - static_cast<i64>(lo));
        if (span >= 0xFFFFFFFFULL) {
            return std::bit_cast<i32>(NextU32());
        }
        return static_cast<i32>(static_cast<i64>(lo) + static_cast<i64>(NextBoundedU32(static_cast<u32>(span + 1ULL))));
    }

    [[nodiscard]] constexpr u64 State() const noexcept { return m_state; }

private:
    u64 m_state{kDefaultSeed};
};
static_assert(sizeof(Rng64) == 8);

} // namespace en

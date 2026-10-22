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

namespace en {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;
using f32 = float;
using f64 = double;

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
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) | static_cast<U>(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator&(E lhs, E rhs) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) & static_cast<U>(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator^(E lhs, E rhs) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) ^ static_cast<U>(rhs));
}

template <BitmaskEnum E>
[[nodiscard]] constexpr E operator~(E val) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(~static_cast<U>(val));
}

template <BitmaskEnum E>
constexpr E& operator|=(E& lhs, E rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

template <BitmaskEnum E>
constexpr E& operator&=(E& lhs, E rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

template <BitmaskEnum E>
constexpr E& operator^=(E& lhs, E rhs) noexcept {
    lhs = lhs ^ rhs;
    return lhs;
}

template <BitmaskEnum E>
[[nodiscard]] constexpr bool HasFlag(E value, E flag) noexcept {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
}

// Clamps inputs above 2^31 to 0x80000000U because std::bit_ceil is undefined when the result overflows u32.
[[nodiscard]] constexpr u32 NextPow2(u32 value) noexcept {
    if (value <= 1U) return 1U;
    if (value > 0x80000000U) return 0x80000000U;
    return std::bit_ceil(value);
}

[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) noexcept {
    if (!std::has_single_bit(alignment)) return value;
    const usize mask = alignment - 1U;
    if (value > (~usize{0} - mask)) return value & ~mask;
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
    for (u32 i = 0; i < 256U; ++i) {
        u32 crc = i;
        for (u32 bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
        table[i] = crc;
    }
    return table;
}

inline constexpr std::array<u32, 256> kCrc32Table = BuildCrc32Table();

} // namespace detail

// Table-driven IEEE 802.3 CRC32 checksum supporting incremental chaining via previousCrc.
[[nodiscard]] constexpr u32 ComputeCrc32(std::span<const u8> bytes, u32 previousCrc = 0U) noexcept {
    u32 crc = ~previousCrc;
    for (const u8 b : bytes) {
        crc = (crc >> 8U) ^ detail::kCrc32Table[static_cast<u8>((crc ^ b) & 0xFFU)];
    }
    return ~crc;
}

// Case-insensitive, slash-normalized 64-bit FNV-1a identifier for engine symbols and asset paths.
// Value type; zero dynamic memory ownership.
// Invariant: empty strings map to 0, backslashes fold to '/', and ASCII uppercase folds to lowercase.
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
static_assert(sizeof(StringId) == 8);

inline namespace literals {
[[nodiscard]] consteval StringId operator""_sid(const char* str, usize len) noexcept {
    return StringId::Hash(std::string_view{str, len});
}
} // namespace literals

// Type-tagged 32-bit generational handle preventing ABA use-after-free bugs in slot pools.
// Value type; default-constructed handle has generation == 0 (invalid sentinel).
// Invariant: IsValid() holds iff generation != 0.
template <typename Tag>
struct alignas(4) Handle {
    u16 index{0};
    u16 generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }

    [[nodiscard]] constexpr u32 Packed() const noexcept {
        return (static_cast<u32>(generation) << 16U) | static_cast<u32>(index);
    }

    [[nodiscard]] static constexpr Handle FromPacked(u32 packed) noexcept {
        return Handle{static_cast<u16>(packed & 0xFFFFU), static_cast<u16>(packed >> 16U)};
    }

    [[nodiscard]] constexpr auto operator<=>(const Handle&) const noexcept = default;
};
static_assert(sizeof(Handle<void>) == 4);

// Fixed-capacity stack string with transactional single-pass virtual path resolution.
// Value type with 2-byte natural alignment so embedding inside structs never introduces alignment holes.
// Invariant: sizeof(FixedString<N>) == N, m_buf[m_len] == '\0', and failed operations never mutate state.
template <usize CapacityBytes>
    requires (CapacityBytes >= 8 && CapacityBytes <= 65536 && (CapacityBytes % 2U) == 0U)
class alignas(2) FixedString {
public:
    static constexpr usize kMaxLength = CapacityBytes - 3U;

    constexpr FixedString() noexcept = default;
    constexpr explicit FixedString(std::string_view sv) noexcept { (void)Assign(sv); }

    constexpr bool Assign(std::string_view sv) noexcept {
        if (sv.size() > kMaxLength) return false;
        for (const char c : sv) {
            if (c == '\0') return false;
        }
        m_len = static_cast<u16>(sv.size());
        for (usize i = 0; i < sv.size(); ++i) {
            m_buf[i] = sv[i];
        }
        m_buf[m_len] = '\0';
        return true;
    }

    constexpr bool Append(std::string_view sv) noexcept {
        if (sv.size() > kMaxLength - m_len) return false;
        for (const char c : sv) {
            if (c == '\0') return false;
        }
        for (usize i = 0; i < sv.size(); ++i) {
            m_buf[m_len + i] = sv[i];
        }
        m_len = static_cast<u16>(m_len + sv.size());
        m_buf[m_len] = '\0';
        return true;
    }

    constexpr bool AssignNormalizedDir(std::string_view sv) noexcept {
        if (sv.empty() || sv.size() > kMaxLength) return false;
        for (const char c : sv) {
            if (static_cast<u8>(c) < 0x20U || static_cast<u8>(c) == 0x7FU) return false;
        }
        const char lastNorm = (sv.back() == '\\') ? '/' : sv.back();
        if (sv.size() + (lastNorm == '/' ? 0U : 1U) > kMaxLength) return false;

        u16 out = 0;
        for (const char c : sv) {
            m_buf[out++] = (c == '\\') ? '/' : c;
        }
        if (m_buf[out - 1U] != '/') {
            m_buf[out++] = '/';
        }
        m_buf[out] = '\0';
        m_len = out;
        return true;
    }

    // Resolves a relative path against this base directory while rejecting traversal ("..", "."),
    // empty segments ("//"), absolute prefixes, drive colons, and control characters.
    [[nodiscard]] constexpr bool Resolve(std::string_view rel, FixedString& out) const noexcept {
        if (rel.empty() || rel.front() == '/' || rel.front() == '\\' ||
            rel.back() == '/' || rel.back() == '\\') {
            return false;
        }
        const bool needsSep = (m_len > 0 && m_buf[m_len - 1U] != '/');
        if (rel.size() > kMaxLength - m_len - (needsSep ? 1U : 0U)) {
            return false;
        }

        FixedString candidate = *this;
        if (needsSep) {
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

// Deterministic 64-bit SplitMix pseudo-random generator with Lemire unbiased bounded sampling.
// Value type; 8-byte footprint with zero dynamic allocation.
// Invariant: m_state != 0.
class alignas(8) Rng64 {
public:
    static constexpr u64 kDefaultSeed = 0x853C49E6748FEA9BULL;

    constexpr explicit Rng64(u64 seed = kDefaultSeed) noexcept
        : m_state(seed != 0ULL ? seed : kDefaultSeed) {}

    [[nodiscard]] constexpr u64 NextU64() noexcept {
        m_state += 0x9E3779B97F4A7C15ULL;
        if (m_state == 0ULL) m_state = kDefaultSeed;
        u64 z = m_state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] constexpr u32 NextU32() noexcept {
        return static_cast<u32>(NextU64() >> 32U);
    }

    // Generates a float in [0.0f, 1.0f) using the upper 24 mantissa bits.
    [[nodiscard]] constexpr f32 NextF32() noexcept {
        return static_cast<f32>(NextU32() >> 8U) * (1.0f / 16777216.0f);
    }

    // Lemire's divisionless rejection method eliminating modulo bias in [0, bound).
    [[nodiscard]] constexpr u32 NextBoundedU32(u32 bound) noexcept {
        if (bound <= 1U) return 0U;
        u64 product = static_cast<u64>(NextU32()) * static_cast<u64>(bound);
        u32 low = static_cast<u32>(product);
        if (low < bound) {
            const u32 threshold = (0U - bound) % bound;
            while (low < threshold) {
                product = static_cast<u64>(NextU32()) * static_cast<u64>(bound);
                low = static_cast<u32>(product);
            }
        }
        return static_cast<u32>(product >> 32U);
    }

    [[nodiscard]] constexpr f32 RangeF32(f32 lo, f32 hi) noexcept {
        return (hi <= lo) ? lo : (lo + (hi - lo) * NextF32());
    }

    // Overflow-safe across the full [INT32_MIN, INT32_MAX] domain.
    [[nodiscard]] constexpr i32 RangeI32(i32 lo, i32 hi) noexcept {
        if (hi <= lo) return lo;
        const u64 spanMinusOne = static_cast<u64>(static_cast<i64>(hi) - static_cast<i64>(lo));
        if (spanMinusOne >= 0xFFFFFFFFULL) {
            return static_cast<i32>(NextU32());
        }
        const u32 offset = NextBoundedU32(static_cast<u32>(spanMinusOne + 1ULL));
        return static_cast<i32>(static_cast<i64>(lo) + static_cast<i64>(offset));
    }

    [[nodiscard]] constexpr u64 State() const noexcept { return m_state; }

private:
    u64 m_state{kDefaultSeed};
};
static_assert(sizeof(Rng64) == 8);

} // namespace en

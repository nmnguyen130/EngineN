#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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
[[nodiscard]] constexpr bool HasFlag(E value, E mask) noexcept {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(value) & static_cast<U>(mask)) != 0;
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

[[nodiscard]] constexpr u32 ComputeCrc32(std::span<const u8> bytes, u32 crc = 0xFFFFFFFFU) noexcept {
    for (const u8 b : bytes) {
        crc ^= static_cast<u32>(b);
        for (u32 bit = 0; bit < 8U; ++bit) {
            const u32 mask = static_cast<u32>(-static_cast<i32>(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] constexpr u32 NextPow2(u32 v) noexcept {
    return v <= 1U ? 1U : std::bit_ceil(v);
}

[[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) noexcept {
    return (value + (alignment - 1U)) & ~(alignment - 1U);
}

// Case-insensitive path-normalized 64-bit FNV-1a identifier for assets and symbols.
// Value type; zero heap ownership.
// Invariant: backslashes normalize to forward slashes and ASCII uppercase folds to lowercase.
struct alignas(8) StringId {
    u64 value{0};

    constexpr StringId() noexcept = default;
    constexpr explicit StringId(u64 raw) noexcept : value(raw) {}

    [[nodiscard]] static constexpr StringId Hash(std::string_view str) noexcept {
        if (str.empty()) return StringId{0};
        u64 h = 0xCBF29CE484222325ULL;
        for (char c : str) {
            char norm = (c == '\\') ? '/' : c;
            if (norm >= 'A' && norm <= 'Z') {
                norm = static_cast<char>(norm + ('a' - 'A'));
            }
            h ^= static_cast<u8>(norm);
            h *= 0x100000001B3ULL;
        }
        return StringId{h};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const StringId&) const noexcept = default;
};
static_assert(sizeof(StringId) == 8);

inline namespace literals {
[[nodiscard]] consteval StringId operator""_sid(const char* str, usize len) noexcept {
    return StringId::Hash(std::string_view{str, len});
}
} // namespace literals

// Generational 32-bit resource handle preventing ABA use-after-free bugs.
// Value type; index 0 with generation 0 denotes a null handle.
// Invariant: IsValid() is true iff generation != 0.
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
    [[nodiscard]] constexpr bool operator==(const Handle&) const noexcept = default;
};

// Fixed-capacity stack string with transactional single-pass sandbox path validation.
// Value type; owns inline char storage.
// Invariant: m_buf is always null-terminated and failed Resolve() never mutates out.
template <usize N>
class alignas(N >= 64 ? 64 : 8) FixedString {
public:
    static constexpr usize kMaxLength = N - 3;

    constexpr FixedString() noexcept = default;
    constexpr explicit FixedString(std::string_view sv) noexcept { (void)Assign(sv); }

    constexpr bool Assign(std::string_view sv) noexcept {
        if (sv.size() > kMaxLength) return false;
        m_len = static_cast<u16>(sv.size());
        for (usize i = 0; i < sv.size(); ++i) m_buf[i] = sv[i];
        m_buf[m_len] = '\0';
        return true;
    }

    constexpr bool AssignNormalizedDir(std::string_view sv) noexcept {
        if (sv.empty() || sv.size() + 1U > kMaxLength) return false;
        u16 out = 0;
        for (char c : sv) {
            m_buf[out++] = (c == '\\') ? '/' : c;
        }
        if (m_buf[out - 1] != '/') m_buf[out++] = '/';
        m_buf[out] = '\0';
        m_len = out;
        return true;
    }

    [[nodiscard]] constexpr bool Resolve(std::string_view rel, FixedString& out) const noexcept {
        if (rel.empty() || rel.front() == '/' || rel.front() == '\\' ||
            rel.back() == '/' || rel.back() == '\\') {
            return false;
        }
        if (static_cast<usize>(m_len) + rel.size() > kMaxLength) return false;
        FixedString candidate = *this;
        usize segLen = 0;
        usize dotCount = 0;
        for (char c : rel) {
            if (static_cast<u8>(c) < 0x20U || c == ':') return false;
            const char norm = (c == '\\') ? '/' : c;
            if (norm == '/') {
                if (segLen == 0 || (dotCount == segLen && segLen <= 2)) return false;
                segLen = 0;
                dotCount = 0;
            } else {
                ++segLen;
                if (norm == '.') ++dotCount;
            }
            candidate.m_buf[candidate.m_len++] = norm;
        }
        if (segLen == 0 || (dotCount == segLen && segLen <= 2)) return false;
        candidate.m_buf[candidate.m_len] = '\0';
        out = candidate;
        return true;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf, m_len}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return m_buf; }
    [[nodiscard]] constexpr u16 Size() const noexcept { return m_len; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_len == 0; }

private:
    u16 m_len{0};
    char m_buf[N - 2]{};
};

using FilePath = FixedString<256>;
static_assert(sizeof(FilePath) == 256);

// Deterministic 64-bit SplitMix pseudo-random number generator.
class Rng64 {
public:
    constexpr explicit Rng64(u64 seed = 0x853C49E6748FEA9BULL) noexcept
        : m_state(seed != 0 ? seed : 0x853C49E6748FEA9BULL) {}

    [[nodiscard]] constexpr u64 NextU64() noexcept {
        m_state = Mix64(m_state);
        return m_state;
    }

    [[nodiscard]] constexpr u32 NextU32() noexcept {
        return static_cast<u32>(NextU64() >> 32U);
    }

    [[nodiscard]] constexpr f32 NextF32() noexcept {
        return static_cast<f32>(NextU32() >> 8U) * (1.0f / 16777216.0f);
    }

    [[nodiscard]] constexpr f32 RangeF32(f32 lo, f32 hi) noexcept {
        return lo + (hi - lo) * NextF32();
    }

    [[nodiscard]] constexpr i32 RangeI32(i32 lo, i32 hi) noexcept {
        if (hi <= lo) return lo;
        const u32 span = static_cast<u32>(hi - lo + 1);
        return lo + static_cast<i32>(NextU32() % span);
    }

    [[nodiscard]] constexpr u64 State() const noexcept { return m_state; }

private:
    u64 m_state;
};

} // namespace en

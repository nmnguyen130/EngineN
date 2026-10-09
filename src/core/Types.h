#pragma once

#include <cstddef>
#include <cstdint>

namespace EngineN {

// Fixed-width integer types
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using usize = std::size_t;
using isize = std::ptrdiff_t;

// Floating-point types
using f32 = float;
using f64 = double;

// Memory size user-defined literals
constexpr usize operator""_KB(unsigned long long val) noexcept { return static_cast<usize>(val * 1024ULL); }
constexpr usize operator""_MB(unsigned long long val) noexcept { return static_cast<usize>(val * 1024ULL * 1024ULL); }
constexpr usize operator""_GB(unsigned long long val) noexcept { return static_cast<usize>(val * 1024ULL * 1024ULL * 1024ULL); }

} // namespace EngineN

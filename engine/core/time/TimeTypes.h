#pragma once

#include "core/Types.h"

#include <array>
#include <concepts>

namespace en::time {

inline constexpr u32 kSimHz           = 120;
inline constexpr u64 kNsPerSecond     = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs       = kNsPerSecond / kSimHz;
inline constexpr f32 kFixedDtSec      = 1.0f / static_cast<f32>(kSimHz);
inline constexpr u8  kMaxTimeChannels = 8;

static_assert(kSimHz % 60 == 0, "Simulation frequency must be an integer multiple of 60Hz.");

consteval u32 operator""_f60(unsigned long long frames) noexcept {
    return static_cast<u32>(frames * (kSimHz / 60));
}

/**
 * TimeChannel: Discrete simulation and presentation timelines.
 * Enables independent time dilation (e.g. hitstop on Player while World continues).
 */
enum class TimeChannel : u8 {
    World = 0,
    Player,
    Enemy,
    Projectile,
    Vfx,
    Camera,
    Ui,
    Custom
};

template <typename T>
concept ChannelId = std::same_as<T, TimeChannel> || std::unsigned_integral<T>;

[[nodiscard]] constexpr u8 ToChannelIndex(ChannelId auto ch) noexcept {
    const auto idx = static_cast<u8>(ch);
    return (idx < kMaxTimeChannels) ? idx : 0;
}

template <typename T>
concept ClockSource = requires(T clock) {
    { clock.NowNs() } noexcept -> std::same_as<u64>;
};

/**
 * BakedTickState: Precomputed delta time and effective scale for all 8 channels.
 * Packed into a single 64-byte L1 cache line to eliminate per-entity float math and cache misses.
 */
struct alignas(64) BakedTickState {
    std::array<f32, kMaxTimeChannels> scaledDt{};
    std::array<f32, kMaxTimeChannels> effectiveScale{};
};
static_assert(sizeof(BakedTickState) == 64);

/**
 * BakedRenderState: Presentation interpolation alpha snapshot across all channels.
 * Aligns with cache boundaries for render extraction and sprite interpolation.
 */
struct alignas(32) BakedRenderState {
    std::array<f32, kMaxTimeChannels> alpha{};
};

} // namespace en::time
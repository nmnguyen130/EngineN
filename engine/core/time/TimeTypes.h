#pragma once

#include "core/Types.h"

#include <array>
#include <concepts>
#include <type_traits>

namespace en::time {

inline constexpr u32 kSimHz                = 120;
inline constexpr u64 kNsPerSecond          = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs            = kNsPerSecond / kSimHz;
inline constexpr f32 kFixedDtSec           = 1.0f / static_cast<f32>(kSimHz);
inline constexpr f64 kInvNsPerSecond       = 1.0 / static_cast<f64>(kNsPerSecond);
inline constexpr u8  kMaxTimeChannels      = 8;
inline constexpr u8  kGameplayChannelsMask = 0x3Fu; // Channels 0..5 (World..Camera); leaves Ui and Custom active on pause.

static_assert(kSimHz > 0 && kSimHz % 60 == 0, "Simulation frequency must be a positive multiple of 60Hz.");
static_assert(kMaxTimeChannels == 8, "Bitmask channel state requires exactly 8 channels.");

inline namespace literals {

consteval u32 operator""_f60(unsigned long long frames) noexcept {
    return static_cast<u32>(frames * (kSimHz / 60));
}

consteval u32 operator""_ticks(unsigned long long ticks) noexcept {
    return static_cast<u32>(ticks);
}

} // namespace literals

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
concept ChannelId = std::unsigned_integral<T> ||
    (std::is_enum_v<T> && std::same_as<std::underlying_type_t<T>, u8>);

[[nodiscard]] constexpr u8 ToChannelIndex(ChannelId auto ch) noexcept {
    const auto idx = static_cast<u8>(ch);
    return (idx < kMaxTimeChannels) ? idx : u8{0};
}

template <ChannelId... Channels>
[[nodiscard]] constexpr u8 MakeChannelMask(Channels... chs) noexcept {
    return static_cast<u8>((static_cast<u8>(1u << ToChannelIndex(chs)) | ... | u8{0}));
}

template <typename T>
concept ClockSource = requires(T clock) {
    { clock.NowNs() } noexcept -> std::same_as<u64>;
};

// Deterministic Layer 1 mock clock for unit testing and headless replay without OS timer dependencies.
struct ManualClock {
    u64 currentNs = 0;

    [[nodiscard]] constexpr u64 NowNs() const noexcept { return currentNs; }

    constexpr void AdvanceNs(u64 deltaNs) noexcept { currentNs += deltaNs; }
    constexpr void AdvanceUs(u64 deltaUs) noexcept { currentNs += deltaUs * 1'000ULL; }
    constexpr void AdvanceMs(u64 deltaMs) noexcept { currentNs += deltaMs * 1'000'000ULL; }
    constexpr void AdvanceTicks(u64 ticks = 1) noexcept {
        currentNs += (ticks * kNsPerSecond) / kSimHz;
    }
};

// Precomputed per-tick delta time and effective scale for all 8 channels packed into a single 64B L1 cache line.
struct alignas(64) BakedTickState {
    std::array<f32, kMaxTimeChannels> scaledDt{
        kFixedDtSec, kFixedDtSec, kFixedDtSec, kFixedDtSec,
        kFixedDtSec, kFixedDtSec, kFixedDtSec, kFixedDtSec
    };
    std::array<f32, kMaxTimeChannels> effectiveScale{
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f
    };
};
static_assert(sizeof(BakedTickState) == 64);

// Presentation interpolation alpha snapshot across all 8 channels.
struct alignas(32) BakedRenderState {
    std::array<f32, kMaxTimeChannels> alpha{
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f
    };
};
static_assert(sizeof(BakedRenderState) == 32);

// Read-only 64B L1 cache-aligned presentation snapshot passed to WorldExtractor, Renderer, AudioSystem, and DebugSystem.
// Invariant: Frozen channels have channelAlpha pinned to 1.0f to prevent sub-frame interpolation jitter.
struct alignas(64) RenderTimeView {
    BakedRenderState channelAlpha{};
    f32              rawAlpha   = 0.0f;
    f32              realDt     = 0.0f;
    u64              tickIndex  = 0;
    u8               frozenMask = 0;

    [[nodiscard]] constexpr f32 Alpha(ChannelId auto ch = TimeChannel::World) const noexcept {
        return channelAlpha.alpha[ToChannelIndex(ch)];
    }

    [[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept {
        return (frozenMask & static_cast<u8>(1u << ToChannelIndex(ch))) != 0;
    }
};
static_assert(sizeof(RenderTimeView) == 64);

} // namespace en::time

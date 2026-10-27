#pragma once

#include "core/Types.h"

#include <algorithm>
#include <array>

namespace en {

inline constexpr u32 kSimHz = 120U;
inline constexpr u64 kNsPerSecond = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs = kNsPerSecond / kSimHz;
inline constexpr f32 kFixedDtSec = 1.0f / static_cast<f32>(kSimHz);

// 1440 Hz (12 * 120 Hz) is the least common multiple of standard refresh rates:
// 30, 40, 48, 60, 72, 80, 90, 120, 144, 160, 180, 240, 360, and 480 Hz.
inline constexpr u64 kHarmonicSubdivisions = 12ULL;
inline constexpr u64 kHarmonicHz = static_cast<u64>(kSimHz) * kHarmonicSubdivisions;
inline constexpr u64 kTickUnits = kNsPerSecond * kHarmonicSubdivisions;

inline constexpr u32 kMaxTimeChannels = 8U;
inline constexpr u8 kGameplayChannelsMask = 0x3FU;
inline constexpr u32 kMaxFreezeTicks = 3600U;

// 8 simulation time channels: World (0) cascades to gameplay channels (1..5); Ui (6) and Custom (7) are independent.
enum class TimeChannel : u8 {
    World      = 0,
    Player     = 1,
    Enemy      = 2,
    Projectile = 3,
    Vfx        = 4,
    Camera     = 5,
    Ui         = 6,
    Custom     = 7,
};

[[nodiscard]] constexpr u8 ChannelIdx(TimeChannel ch) noexcept {
    return static_cast<u8>(ch) & 7U;
}

[[nodiscard]] constexpr u8 ChannelBit(TimeChannel ch) noexcept {
    return static_cast<u8>(1U << ChannelIdx(ch));
}

inline namespace literals {
[[nodiscard]] consteval u32 operator""_f60(unsigned long long frames) noexcept {
    return static_cast<u32>(frames * 2ULL);
}
[[nodiscard]] consteval u32 operator""_ticks(unsigned long long ticks) noexcept {
    return static_cast<u32>(ticks);
}
} // namespace literals

struct alignas(64) BakedTickState {
    std::array<f32, kMaxTimeChannels> scaledDt{};
    std::array<f32, kMaxTimeChannels> effectiveScale{};
};
static_assert(sizeof(BakedTickState) == 64);

// Per-channel time modulation state for freeze ticks and smooth time-scale ramps.
struct alignas(16) ChannelModSlot {
    f32 baseScale{1.0f};
    f32 rampTargetScale{1.0f};
    u16 hitstopTicks{0};
    u16 rampHoldTicks{0};
    u16 rampBlendTicks{0};
    u16 rampElapsedTicks{0};

    constexpr void ClearRamp() noexcept {
        rampTargetScale = 1.0f;
        rampHoldTicks = 0U;
        rampBlendTicks = 0U;
        rampElapsedTicks = 0U;
    }

    constexpr void ClearEffects() noexcept {
        ClearRamp();
        hitstopTicks = 0U;
    }

    [[nodiscard]] constexpr f32 EvaluateLocalScale(bool rampActive) const noexcept {
        if (hitstopTicks > 0U) return 0.0f;
        if (!rampActive) return baseScale;
        if (rampElapsedTicks < rampHoldTicks) return baseScale * rampTargetScale;
        if (rampBlendTicks == 0U) return baseScale;
        const f32 t = std::clamp(
            static_cast<f32>(rampElapsedTicks - rampHoldTicks) / static_cast<f32>(rampBlendTicks),
            0.0f, 1.0f);
        const f32 smooth = t * t * (3.0f - 2.0f * t);
        return baseScale * (rampTargetScale + (1.0f - rampTargetScale) * smooth);
    }
};
static_assert(sizeof(ChannelModSlot) == 16);

struct alignas(16) RenderTimeView {
    std::array<f32, kMaxTimeChannels> channelAlpha{};
    u64 tickIndex{0};
    f32 rawAlpha{0.0f};
    f32 realDt{kFixedDtSec};

    [[nodiscard]] constexpr f32 Alpha(TimeChannel ch = TimeChannel::World) const noexcept {
        return channelAlpha[ChannelIdx(ch)];
    }
};
static_assert(sizeof(RenderTimeView) == 48);

struct alignas(8) TimeTelemetry {
    u64 totalSimTicks{0};
    u64 totalFrames{0};
    u32 droppedTicks{0};
    u16 ticksLastFrame{0};
    u16 maxCatchupTicks{8};
};
static_assert(sizeof(TimeTelemetry) == 24);

} // namespace en

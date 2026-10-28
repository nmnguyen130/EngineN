#pragma once

#include "core/Types.h"

#include <algorithm>
#include <array>

namespace en {

// 120 Hz fixed simulation. Proven for precision platformers / Metroidvania feel.
inline constexpr u32 kSimHz           = 120U;
inline constexpr u64 kNsPerSecond     = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs       = kNsPerSecond / kSimHz;
inline constexpr f32 kFixedDtSec      = 1.0f / static_cast<f32>(kSimHz);

// 1440 Hz harmonic base = LCM of common refresh rates. Enables exact rational snap.
inline constexpr u64 kHarmonicSubdiv  = 12ULL;
inline constexpr u64 kHarmonicHz      = static_cast<u64>(kSimHz) * kHarmonicSubdiv;
inline constexpr u64 kTickUnits       = kNsPerSecond * kHarmonicSubdiv; // 12e9

inline constexpr u32 kMaxTimeChannels = 8U;
inline constexpr u8  kGameplayMask    = 0x3FU; // World..Camera
inline constexpr u32 kMaxFreezeTicks  = 3600U; // 30 s @ 120 Hz

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
[[nodiscard]] consteval u32 operator""_f60(unsigned long long f) noexcept {
    return static_cast<u32>(f * 2ULL); // 60 fps frames → 120 Hz ticks
}
[[nodiscard]] consteval u32 operator""_ticks(unsigned long long t) noexcept {
    return static_cast<u32>(t);
}
} // namespace literals

// 64-byte baked scales for one tick (cache line).
struct alignas(64) BakedTickState {
    std::array<f32, kMaxTimeChannels> scaledDt{};
    std::array<f32, kMaxTimeChannels> effectiveScale{};
};
static_assert(sizeof(BakedTickState) == 64);

// 16-byte per-channel modulation. Hitstop + smooth ramp.
struct alignas(16) ChannelModSlot {
    f32 baseScale{1.0f};
    f32 rampTarget{1.0f};
    u16 hitstop{0};
    u16 rampHold{0};
    u16 rampBlend{0};
    u16 rampElapsed{0};

    constexpr void ClearRamp() noexcept {
        rampTarget = 1.0f;
        rampHold = rampBlend = rampElapsed = 0U;
    }
    constexpr void Clear() noexcept {
        ClearRamp();
        hitstop = 0U;
    }

    [[nodiscard]] constexpr f32 LocalScale(bool rampOn) const noexcept {
        if (hitstop > 0U) return 0.0f;
        if (!rampOn) return baseScale;
        if (rampHold > 0U) return baseScale * rampTarget;
        if (rampBlend == 0U) return baseScale;
        const f32 t = std::clamp(static_cast<f32>(rampElapsed) / static_cast<f32>(rampBlend), 0.0f, 1.0f);
        const f32 s = t * t * (3.0f - 2.0f * t); // smoothstep
        return baseScale * (rampTarget + (1.0f - rampTarget) * s);
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
    u16 maxCatchup{8}; // ~66 ms @ 120 Hz — safe default
};
static_assert(sizeof(TimeTelemetry) == 24);

} // namespace en

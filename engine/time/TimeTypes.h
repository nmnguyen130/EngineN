#pragma once

#include "core/Math.h"

namespace en {

inline constexpr u32 kTickHz           = 120U;
inline constexpr u64 kNsPerSec         = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs        = kNsPerSec / kTickHz;
inline constexpr f32 kFixedDt          = 1.0f / static_cast<f32>(kTickHz);

// 1440 Hz harmonic base (LCM of 30, 36, 40, 48, 60, 72, 80, 90, 120, 144, 160, 180, 240, 360, 480 Hz).
inline constexpr u64 kHarmonicSubdiv   = 12ULL;
inline constexpr u64 kHarmonicHz       = static_cast<u64>(kTickHz) * kHarmonicSubdiv;
inline constexpr u64 kTickUnits        = kNsPerSec * kHarmonicSubdiv;
inline constexpr u64 kMaxFrameNs       = 250'000'000ULL;
inline constexpr u64 kHarmonicSnapNs   = 150'000ULL;
inline constexpr f32 kMaxTimeScale     = 8.0f;

inline constexpr u32 kGameplayChannels = 4U;
inline constexpr u8  kGameplayMask     = 0x0FU;

enum class TimeChannel : u8 {
    World  = 0,
    Player = 1,
    Enemy  = 2,
    Camera = 3,
    Ui     = 4,
};

[[nodiscard]] constexpr u8 ChannelBit(TimeChannel ch) noexcept {
    const u8 idx = static_cast<u8>(ch);
    return idx < kGameplayChannels ? static_cast<u8>(1U << idx) : 0U;
}

namespace literals {
[[nodiscard]] consteval u16 operator""_ticks(unsigned long long v) noexcept {
    return static_cast<u16>(v);
}
[[nodiscard]] consteval u16 operator""_ms(unsigned long long ms) noexcept {
    const unsigned long long ticks = (ms * kTickHz + 999ULL) / 1000ULL;
    return static_cast<u16>(ticks == 0ULL && ms > 0ULL ? 1ULL : ticks);
}
} // namespace literals

// 24-byte immutable snapshot of one 120 Hz simulation tick.
// Value type; Scale(ch) and SimTimeNs() are derived inline without duplicate storage.
struct TickStep {
    std::array<f32, kGameplayChannels> dt{kFixedDt, kFixedDt, kFixedDt, kFixedDt};
    u32 tick{0};
    u16 substep{0};
    u8  frozenMask{0};
    u8  active{0};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return active != 0U; }

    [[nodiscard]] constexpr f32 Dt(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch);
        return idx < kGameplayChannels ? dt[idx] : kFixedDt;
    }

    [[nodiscard]] constexpr f32 Scale(TimeChannel ch = TimeChannel::World) const noexcept {
        const u8 idx = static_cast<u8>(ch);
        return idx < kGameplayChannels ? (dt[idx] * static_cast<f32>(kTickHz)) : 1.0f;
    }

    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (frozenMask & ChannelBit(ch)) != 0U;
    }

    [[nodiscard]] constexpr u64 SimTimeNs() const noexcept {
        return static_cast<u64>(tick) * kFixedDtNs;
    }
};
static_assert(sizeof(TickStep) == 24);

// 20-byte sub-tick interpolation state for render transform blending.
// Frozen gameplay channels resolve to alpha = 1.0f inline; UI always uses raw alpha.
struct RenderStep {
    f32 alpha{0.0f};
    f32 frameDt{kFixedDt};
    f32 smoothDt{1.0f / 60.0f};
    u32 tick{0};
    u8  frozenMask{0};
    u8  paused{0};
    u16 frame{0};

    [[nodiscard]] constexpr bool IsFrozen(TimeChannel ch = TimeChannel::World) const noexcept {
        return (ch != TimeChannel::Ui) && (paused != 0U || (frozenMask & ChannelBit(ch)) != 0U);
    }

    [[nodiscard]] constexpr f32 Alpha(TimeChannel ch = TimeChannel::World) const noexcept {
        return IsFrozen(ch) ? 1.0f : alpha;
    }

    [[nodiscard]] constexpr f32 Fps() const noexcept {
        return smoothDt > 1e-6f ? (1.0f / smoothDt) : 0.0f;
    }
};
static_assert(sizeof(RenderStep) == 20);

// 16-byte per-channel hitstop and smoothstep time-ramp state with zero padding.
struct ChannelMod {
    f32 baseScale{1.0f};
    f32 rampTarget{1.0f};
    u16 hitstop{0};
    u16 rampHold{0};
    u16 rampRecover{0};
    u16 rampElapsed{0};

    constexpr void ClearRamp() noexcept {
        rampTarget  = 1.0f;
        rampHold    = 0U;
        rampRecover = 0U;
        rampElapsed = 0U;
    }

    [[nodiscard]] constexpr f32 LocalScale(bool hasRamp) const noexcept {
        if (hitstop > 0U) return 0.0f;
        if (!hasRamp) return baseScale;
        if (rampHold > 0U) return baseScale * rampTarget;
        const f32 t = static_cast<f32>(rampElapsed) / static_cast<f32>(rampRecover);
        const f32 s = t * t * (3.0f - 2.0f * t);
        return baseScale * Lerp(rampTarget, 1.0f, s);
    }
};
static_assert(sizeof(ChannelMod) == 16);

// 24-byte telemetry counters for frame and tick diagnostics.
struct TimeStats {
    u64 totalTicks{0};
    u64 totalFrames{0};
    u32 droppedTicks{0};
    u16 lastFrameTicks{0};
    u16 clampedFrames{0};
};
static_assert(sizeof(TimeStats) == 24);

} // namespace en

#pragma once

#include "core/Types.h"
#include "core/time/HitstopEngine.h"

#include <array>
#include <type_traits>

namespace EngineN {

// Deterministic simulation baseline (120Hz)
inline constexpr u32 kSimHz = 120;
inline constexpr u64 kNsPerSecond = 1'000'000'000ULL;
inline constexpr u64 kFixedDtNs = kNsPerSecond / kSimHz;
inline constexpr f32 kFixedDtSec = 1.0f / static_cast<f32>(kSimHz);

// Protection limits against OS hitch spikes (250ms) and spiral-of-death backlog
inline constexpr u64 kMaxFrameDeltaNs = 250'000'000ULL;
inline constexpr u32 kDefaultMaxCatchupTicks = 8;

static_assert(kSimHz % 60 == 0, "Simulation Hz must be an integer multiple of 60Hz");

// Authoring literal: converts 60Hz design frames to 120Hz simulation ticks
constexpr u32 operator""_f60(unsigned long long frames) noexcept {
    return static_cast<u32>(frames * (kSimHz / 60));
}

struct TimeConfig {
    u32 maxCatchupTicks = kDefaultMaxCatchupTicks;
    u64 maxFrameDeltaNs = kMaxFrameDeltaNs;
};

/**
 * TimeManager: Central deterministic 120Hz fixed-timestep coordinator.
 * Integer rational accumulator (ns * kSimHz) ensures bitwise zero-drift.
 * Channel-agnostic, supporting generic indexing and user-defined enums.
 */
class TimeManager {
public:
    void Init(const TimeConfig& config = {}) noexcept;

    // Frame lifecycle
    void BeginFrame() noexcept;
    void BeginFrameManual(u64 rawDeltaNs) noexcept;
    [[nodiscard]] bool ConsumeTick() noexcept;
    void EndTick() noexcept;
    void EndFrame() noexcept;

    // State control
    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }
    void ResetClocksOnRefocus() noexcept;

    // Presentation metrics
    [[nodiscard]] f32 GetAlpha() const noexcept { return m_alpha; }
    [[nodiscard]] f64 GetRealDt() const noexcept { return m_realDt; }

    // Simulation metrics & telemetry
    [[nodiscard]] u64 GetSimTickCount() const noexcept { return m_simTickCount; }
    [[nodiscard]] u64 GetDroppedTicks() const noexcept { return m_droppedTicks; }
    [[nodiscard]] u64 GetAccumulatorNs() const noexcept { return m_accUnits / kSimHz; }

    // Channel time scaling
    void SetTimeScale(u8 channel, f32 scale) noexcept;

    template <typename E>
        requires std::is_enum_v<E>
    void SetTimeScale(E channel, f32 scale) noexcept {
        SetTimeScale(static_cast<u8>(channel), scale);
    }

    template <typename C = u8>
    [[nodiscard]] f32 GetTimeScale(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        return (ch < kMaxTimeChannels) ? m_channelScales[ch] : 1.0f;
    }

    // Effective scaled delta time for the current tick (0.0f when hitstop-frozen)
    template <typename C = u8>
    [[nodiscard]] f32 GetScaledDt(C channel = kDefaultTimeChannel) const noexcept {
        const u8 ch = static_cast<u8>(channel);
        if (ch >= kMaxTimeChannels) {
            return 0.0f;
        }
        return kFixedDtSec * m_channelScales[ch] * m_hitstop.GetModifier(ch);
    }

    // Subsystem accessors
    [[nodiscard]] HitstopEngine& Hitstop() noexcept { return m_hitstop; }
    [[nodiscard]] const HitstopEngine& Hitstop() const noexcept { return m_hitstop; }

private:
    TimeConfig    m_config{};
    HitstopEngine m_hitstop{};

    u64  m_prevFrameNs = 0;
    u64  m_accUnits = 0; // Integer accumulator in (ns * kSimHz) units: 10^9 per tick
    f64  m_realDt = 0.0;
    f32  m_alpha = 0.0f;
    u64  m_simTickCount = 0;
    u64  m_droppedTicks = 0;
    u32  m_ticksThisFrame = 0;
    bool m_paused = false;
    bool m_tickOpen = false;

    std::array<f32, kMaxTimeChannels> m_channelScales{};
};

} // namespace EngineN

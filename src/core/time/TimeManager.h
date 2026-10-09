#pragma once

#include "core/Types.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace EngineN {

// Timestep constants & helpers
inline constexpr std::uint64_t kDefaultSimHz = 120ULL;
inline constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;
inline constexpr std::uint64_t kDefaultFixedDtNs = kNsPerSec / kDefaultSimHz;
inline constexpr double kDefaultFixedDtSec = 1.0 / static_cast<double>(kDefaultSimHz);

static_assert(kDefaultSimHz % 60ULL == 0, "Simulation Hz must be an exact integer multiple of 60Hz");

// Converts 60Hz authoring frames to simulation ticks at compile-time (e.g. 6_f60 == 12 ticks)
constexpr std::uint64_t operator""_f60(unsigned long long frames) noexcept {
    return frames * (kDefaultSimHz / 60ULL);
}

// Q16.16 fixed-point math helpers for domain dilation
inline constexpr std::uint32_t kQ16One = 1u << 16;
constexpr std::uint32_t ToQ16(float scale) noexcept {
    return static_cast<std::uint32_t>(scale * static_cast<float>(kQ16One) + 0.5f);
}
constexpr float FromQ16(std::uint32_t q) noexcept {
    return static_cast<float>(q) / static_cast<float>(kQ16One);
}

/**
 * HighResolutionClock: Monotonic hardware nanosecond clock.
 * Queries SDL_GetTicksNS() and filters out backwards jumps or large resume spikes.
 */
class HighResolutionClock {
public:
    void Init() noexcept { Reset(); }
    void Reset() noexcept;

    // Samples elapsed nanoseconds since previous sample, rejecting negative clock jumps
    [[nodiscard]] std::uint64_t Sample() noexcept;

    [[nodiscard]] std::uint64_t GetPreviousTicksNs() const noexcept { return m_prevTicksNs; }
    void SetPreviousTicksNs(std::uint64_t ns) noexcept { m_prevTicksNs = ns; }

private:
    std::uint64_t m_prevTicksNs = 0;
    bool m_initialized = false;
};

enum class ChannelId : std::uint8_t {
    Real = 0,
    World,
    Player,
    Enemy,
    Count
};

enum class HitstopChannel : std::uint8_t {
    World = 0,
    Player,
    Enemy,
    Count
};

struct HitstopRequest {
    HitstopChannel channel = HitstopChannel::World;
    double durationSec = 0.0;
};

/**
 * TimeChannel: Discrete simulation channel for isolated time scaling and hitstop.
 * Provides scaled dt for physics integration, total accumulated sim time, and tick gating.
 */
class TimeChannel {
public:
    void SetTimeScale(double scale) noexcept;

    [[nodiscard]] double GetTimeScale() const noexcept { return m_timeScale; }
    [[nodiscard]] std::uint32_t GetScaleQ16() const noexcept { return m_scaleQ16; }
    [[nodiscard]] double GetScaledDt() const noexcept { return m_scaledDt; }
    [[nodiscard]] double GetTotalTime() const noexcept { return m_totalTime; }
    [[nodiscard]] std::uint64_t GetSimTicks() const noexcept { return m_simTicks; }

private:
    friend class TimeManager;

    void SetRealDelta(double dtSec) noexcept;
    void PrepareTick(double fixedDtSec, double hitstopModifier) noexcept;
    void CommitTick() noexcept;

    double m_timeScale = 1.0;
    std::uint32_t m_scaleQ16 = kQ16One;
    double m_scaledDt = 0.0;
    double m_totalTime = 0.0;
    std::uint64_t m_simTicks = 0;

    std::uint32_t m_accQ16 = 0;            // Q16 progress toward next domain tick
    std::uint32_t m_effScaleQ16 = kQ16One; // Effective scale accounting for hitstop
};

/**
 * HitstopEngine: Manages impact freeze durations per channel.
 * Implements non-stacking merge-by-max to prevent runaway freezes during combos.
 */
class HitstopEngine {
public:
    void Request(const HitstopRequest& req) noexcept;
    void Update(double realDtSec) noexcept;
    void Reset() noexcept;

    [[nodiscard]] double GetModifier(HitstopChannel ch) const noexcept;
    [[nodiscard]] bool IsFrozen(HitstopChannel ch) const noexcept;
    [[nodiscard]] double GetRemainingSec(HitstopChannel ch) const noexcept;

private:
    std::array<double, static_cast<std::size_t>(HitstopChannel::Count)> m_remainingSec{};
};

/**
 * FramePacer: Absolute-deadline frame rate limiter.
 * Prevents cumulative sleep drift by incrementing an absolute target deadline.
 */
class FramePacer {
public:
    void SetTargetFps(int fps) noexcept;
    [[nodiscard]] int GetTargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] std::uint64_t GetTargetFrameNs() const noexcept { return m_targetFrameNs; }
    [[nodiscard]] std::uint64_t GetTargetDeadlineNs() const noexcept { return m_targetDeadlineNs; }

    void WaitForTarget() noexcept;
    void Reset() noexcept;

private:
    int m_targetFps = 0; // 0 = uncapped or VSync-driven presentation
    std::uint64_t m_targetFrameNs = 0;
    std::uint64_t m_targetDeadlineNs = 0;
};

struct TimeConfig {
    int simulationHz = static_cast<int>(kDefaultSimHz);
    int targetRenderFps = 0;                   // 0 = uncapped / vsync
    int maxCatchupTicks = 8;
    std::uint64_t maxFrameNs = 250'000'000ULL; // 250ms spiral-of-death safety clamp
};

/**
 * TimeManager: Central timeloop coordinator for deterministic 120Hz fixed-timestep.
 * Accumulator is tracked in integer ns x Hz to ensure bitwise zero-drift over long runs.
 */
class TimeManager {
public:
    void Init(const TimeConfig& config = {}) noexcept;

    void BeginFrame() noexcept;
    void BeginFrameManual(std::uint64_t rawDeltaNs) noexcept;
    [[nodiscard]] bool ConsumeTick() noexcept;
    void EndTick() noexcept;
    void EndFrame() noexcept;

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }
    void ResetClocksOnRefocus() noexcept;

    // Sub-frame presentation interpolation alpha [0.0, 1.0)
    [[nodiscard]] double GetAlpha() const noexcept { return m_alpha; }
    [[nodiscard]] double GetDomainAlpha(ChannelId ch) const noexcept;

    // Exact accumulator metrics
    [[nodiscard]] std::uint64_t GetAccumulatorNsHz() const noexcept { return m_accNsHz; }
    [[nodiscard]] std::uint64_t GetAccumulatorNs() const noexcept {
        return (m_simHz > 0) ? (m_accNsHz / m_simHz) : 0ULL;
    }

    [[nodiscard]] std::uint64_t GetFixedDtNs() const noexcept { return m_fixedDtNs; }
    [[nodiscard]] double GetFixedDtSec() const noexcept { return m_fixedDtSec; }
    [[nodiscard]] std::uint64_t GetSimTickCount() const noexcept { return m_simTickCount; }
    [[nodiscard]] int GetTicksThisFrame() const noexcept { return m_ticksThisFrame; }
    [[nodiscard]] double GetLastRawDeltaMs() const noexcept { return m_lastRawDeltaMs; }
    [[nodiscard]] bool WasTimeDropped() const noexcept { return m_droppedTime; }

    // Channel accessors: RealTime is const to ensure gameplay never scales wall clock
    [[nodiscard]] const TimeChannel& RealTime() const noexcept;
    [[nodiscard]] TimeChannel& WorldTime() noexcept;
    [[nodiscard]] TimeChannel& PlayerTime() noexcept;
    [[nodiscard]] TimeChannel& EnemyTime() noexcept;

    // Subsystem accessors
    [[nodiscard]] HitstopEngine& Hitstop() noexcept { return m_hitstop; }
    [[nodiscard]] FramePacer& Pacer() noexcept { return m_pacer; }
    [[nodiscard]] HighResolutionClock& Clock() noexcept { return m_clock; }

private:
    void UpdateDomainDeltas() noexcept;

    TimeConfig m_config{};
    std::uint64_t m_simHz = kDefaultSimHz;
    std::uint64_t m_fixedDtNs = kDefaultFixedDtNs;
    double m_fixedDtSec = kDefaultFixedDtSec;
    std::uint64_t m_maxFrameNs = 250'000'000ULL;

    HighResolutionClock m_clock{};
    std::uint64_t m_accNsHz = 0; // Integer ns x Hz accumulator: exactly 10^9 per tick (Zero Drift)
    double m_alpha = 0.0;
    std::uint64_t m_simTickCount = 0;
    int m_ticksThisFrame = 0;
    double m_lastRawDeltaMs = 0.0;
    bool m_paused = false;
    bool m_droppedTime = false;
    bool m_tickOpen = false; // Guard enforcing balanced ConsumeTick/EndTick lifecycles

    std::array<TimeChannel, static_cast<std::size_t>(ChannelId::Count)> m_channels{};
    HitstopEngine m_hitstop{};
    FramePacer m_pacer{};
};

} // namespace EngineN

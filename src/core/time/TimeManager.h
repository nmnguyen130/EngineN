#pragma once

#include "core/Types.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace EngineN {

// Timestep constants & helpers
inline constexpr uint64_t kDefaultSimHz = 120ULL;
inline constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
inline constexpr uint64_t kDefaultFixedDtNs = kNsPerSec / kDefaultSimHz;
inline constexpr double kDefaultFixedDtSec = 1.0 / static_cast<double>(kDefaultSimHz);

static_assert(kDefaultSimHz % 60ULL == 0, "Simulation Hz must be an integer multiple of 60Hz");

// Converts 60Hz authoring frames to 120Hz ticks (e.g. 6_f60 == 12 ticks)
constexpr uint64_t operator""_f60(unsigned long long frames) noexcept {
    return frames * (kDefaultSimHz / 60ULL);
}

// Monotonic hardware clock (nanoseconds)
struct HighResolutionClock {
    uint64_t previousTicks = 0;

    void Init() noexcept {
        previousTicks = SDL_GetTicksNS();
    }

    // Samples elapsed nanoseconds since last sample; rejects negative clock jumps
    uint64_t Sample() noexcept {
        const uint64_t now = SDL_GetTicksNS();
        const uint64_t deltaNs = (now >= previousTicks) ? (now - previousTicks) : 0ULL;
        previousTicks = now;
        return deltaNs;
    }

    // Reset previous ticks after pause or window defocus to prevent delta spikes
    void ResetDelta() noexcept {
        previousTicks = SDL_GetTicksNS();
    }
};

// Discrete simulation channel
struct TimeChannel {
    double timeScale = 1.0;
    double scaledDt = 0.0;
    double totalTime = 0.0;
    uint64_t simTicks = 0;

    void SetModifier(double rawFixedDtSec, double hitstopModifier) noexcept {
        const double effectiveScale = timeScale * hitstopModifier;
        scaledDt = rawFixedDtSec * effectiveScale;
    }

    void CommitTick() noexcept {
        totalTime += scaledDt;
        simTicks++;
    }

    void Advance(double rawDtSec, double hitstopModifier) noexcept {
        SetModifier(rawDtSec, hitstopModifier);
        totalTime += scaledDt;
        simTicks++;
    }

    void SetTimeScale(double scale) noexcept {
        if (!std::isfinite(scale) || scale < 0.0) {
            return;
        }
        timeScale = std::clamp(scale, 0.0, 8.0);
    }
};

// Combat hitstop manager (non-stacking merge-by-max)
enum class HitstopChannel : uint8_t {
    World = 0,
    Player = 1,
    Enemy = 2,
    Count = 3
};

struct HitstopRequest {
    HitstopChannel channel = HitstopChannel::World;
    double freezeDurationSec = 0.0;
};

class HitstopEngine {
public:
    void Request(const HitstopRequest& req) noexcept;
    void Update(double realDtSec) noexcept;
    void Reset() noexcept;

    [[nodiscard]] double GetModifier(HitstopChannel ch) const noexcept;
    [[nodiscard]] bool IsFrozen(HitstopChannel ch) const noexcept;

private:
    std::array<double, static_cast<size_t>(HitstopChannel::Count)> m_remainingFreezeSec{};
};

// Absolute-deadline frame rate limiter
class FramePacer {
public:
    void SetTargetFps(int fps) noexcept;
    [[nodiscard]] int GetTargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] uint64_t GetTargetFrameNs() const noexcept { return m_targetFrameNs; }
    [[nodiscard]] uint64_t GetTargetDeadlineNs() const noexcept { return m_targetDeadlineNs; }

    void WaitForTarget() noexcept;
    void Reset() noexcept;

private:
    int m_targetFps = 0; // 0 = uncapped / vsync-driven
    uint64_t m_targetFrameNs = 0;
    uint64_t m_targetDeadlineNs = 0;
};

// Central timeloop manager
struct TimeConfig {
    int simulationHz = static_cast<int>(kDefaultSimHz);
    int targetRenderFps = 0; // 0 = uncapped / vsync
    int maxCatchupTicks = 8;
};

enum class ChannelId : uint8_t {
    Real = 0,
    World = 1,
    Player = 2,
    Enemy = 3,
    Count = 4
};

class TimeManager {
public:
    void Init(const TimeConfig& config = {}) noexcept;

    void BeginFrame() noexcept;
    void BeginFrameManual(uint64_t rawDeltaNs) noexcept;
    [[nodiscard]] bool ConsumeTick() noexcept;
    void EndTick() noexcept;
    void EndFrame() noexcept;

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

    void ResetClocksOnRefocus() noexcept;

    // Presentation and telemetry metrics
    [[nodiscard]] double GetAlpha() const noexcept { return m_alpha; }
    [[nodiscard]] uint64_t GetAccumulatorNs() const noexcept { return m_accumulatorNs; }
    [[nodiscard]] uint64_t GetFixedDtNs() const noexcept { return m_fixedDtNs; }
    [[nodiscard]] double GetFixedDtSec() const noexcept { return m_fixedDtSec; }
    [[nodiscard]] uint64_t GetSimTickCount() const noexcept { return m_simTickCount; }
    [[nodiscard]] int GetTicksThisFrame() const noexcept { return m_ticksThisFrame; }
    [[nodiscard]] double GetLastRawDeltaMs() const noexcept { return m_lastRawDeltaMs; }

    // Channel accessors
    TimeChannel& RealTime() noexcept { return m_channels[static_cast<size_t>(ChannelId::Real)]; }
    TimeChannel& WorldTime() noexcept { return m_channels[static_cast<size_t>(ChannelId::World)]; }
    TimeChannel& PlayerTime() noexcept { return m_channels[static_cast<size_t>(ChannelId::Player)]; }
    TimeChannel& EnemyTime() noexcept { return m_channels[static_cast<size_t>(ChannelId::Enemy)]; }

    // Subsystem accessors
    HitstopEngine& Hitstop() noexcept { return m_hitstop; }
    FramePacer& Pacer() noexcept { return m_pacer; }
    HighResolutionClock& Clock() noexcept { return m_clock; }

private:
    TimeConfig m_config{};
    uint64_t m_fixedDtNs = kDefaultFixedDtNs;
    double m_fixedDtSec = kDefaultFixedDtSec;
    uint64_t m_maxFrameTimeNs = kDefaultFixedDtNs * 8ULL;

    HighResolutionClock m_clock{};
    uint64_t m_accumulatorNs = 0;
    double m_alpha = 0.0;
    uint64_t m_simTickCount = 0;
    int m_ticksThisFrame = 0;
    double m_lastRawDeltaMs = 0.0;
    bool m_paused = false;

    std::array<TimeChannel, static_cast<size_t>(ChannelId::Count)> m_channels{};
    HitstopEngine m_hitstop{};
    FramePacer m_pacer{};
};

} // namespace EngineN

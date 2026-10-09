#pragma once

#include "Clock.h"
#include "HitstopEngine.h"
#include "FramePacer.h"
#include <cstdint>

namespace EngineN {

struct TimeConfig {
    int simulationHz = 120;
    int targetRenderFps = 0; // 0 = uncapped / vsync
    int maxCatchupTicks = 8;
};

class TimeManager {
public:
    void Init(const TimeConfig& config = {});

    void BeginFrame();
    void BeginFrameManual(uint64_t rawDeltaNs);
    [[nodiscard]] bool ConsumeTick();
    void EndTick();
    void EndFrame();

    void SetPaused(bool paused);
    [[nodiscard]] bool IsPaused() const { return m_paused; }

    void ResetClocksOnRefocus();

    // Accessors
    [[nodiscard]] double GetAlpha() const { return m_alpha; }
    [[nodiscard]] uint64_t GetAccumulatorNs() const { return m_accumulatorNs; }
    [[nodiscard]] uint64_t GetFixedDtNs() const { return m_fixedDtNs; }
    [[nodiscard]] double GetFixedDtSec() const { return m_fixedDtSec; }
    [[nodiscard]] uint64_t GetSimTickCount() const { return m_simTickCount; }
    [[nodiscard]] int GetTicksThisFrame() const { return m_ticksThisFrame; }
    [[nodiscard]] double GetLastRawDeltaMs() const { return m_lastRawDeltaMs; }

    TimeChannel& RealTime() { return m_realTime; }
    TimeChannel& WorldTime() { return m_worldTime; }
    TimeChannel& PlayerTime() { return m_playerTime; }
    TimeChannel& EnemyTime() { return m_enemyTime; }

    HitstopEngine& Hitstop() { return m_hitstop; }
    FramePacer& Pacer() { return m_pacer; }
    HighResolutionClock& Clock() { return m_clock; }

private:
    TimeConfig m_config{};
    uint64_t m_fixedDtNs = 8'333'333; // Default 120Hz
    double m_fixedDtSec = 1.0 / 120.0;
    uint64_t m_maxFrameTimeNs = 66'666'664; // 8 ticks clamp

    HighResolutionClock m_clock{};
    uint64_t m_accumulatorNs = 0;
    double m_alpha = 0.0;
    uint64_t m_simTickCount = 0;
    int m_ticksThisFrame = 0;
    double m_lastRawDeltaMs = 0.0;
    bool m_paused = false;

    TimeChannel m_realTime{"RealTime"};
    TimeChannel m_worldTime{"WorldTime"};
    TimeChannel m_playerTime{"PlayerTime"};
    TimeChannel m_enemyTime{"EnemyTime"};

    HitstopEngine m_hitstop{};
    FramePacer m_pacer{};
};

} // namespace EngineN

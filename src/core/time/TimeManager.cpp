#include "core/time/TimeManager.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace EngineN {

void TimeManager::Init(const TimeConfig& config) noexcept {
    m_config = config;
    m_hitstop.Reset();

    m_prevFrameNs = 0;
    m_accUnits = 0;
    m_realDt = 0.0;
    m_alpha = 0.0f;
    m_simTickCount = 0;
    m_droppedTicks = 0;
    m_ticksThisFrame = 0;
    m_paused = false;
    m_tickOpen = false;

    m_channelScales.fill(1.0f);
}

void TimeManager::BeginFrame() noexcept {
    const u64 now = SDL_GetTicksNS();
    if (m_prevFrameNs == 0) {
        m_prevFrameNs = now;
        BeginFrameManual(0ULL);
        return;
    }

    const u64 deltaNs = (now >= m_prevFrameNs) ? (now - m_prevFrameNs) : 0ULL;
    m_prevFrameNs = now;
    BeginFrameManual(deltaNs);
}

void TimeManager::BeginFrameManual(u64 rawDeltaNs) noexcept {
    if (m_tickOpen) {
        return;
    }

    m_ticksThisFrame = 0;

    // Clamp abnormal frame delta (OS suspend, window drag, debugging pause)
    const u64 clampedDeltaNs = std::min(rawDeltaNs, m_config.maxFrameDeltaNs);
    m_realDt = static_cast<f64>(clampedDeltaNs) / static_cast<f64>(kNsPerSecond);

    if (m_paused || clampedDeltaNs == 0) {
        return;
    }

    // Accumulate in exact integer rational units (ns * kSimHz)
    m_accUnits += clampedDeltaNs * kSimHz;
}

bool TimeManager::ConsumeTick() noexcept {
    if (m_paused || m_tickOpen || m_accUnits < kNsPerSecond) {
        return false;
    }

    // Spiral-of-death overload policy
    if (m_ticksThisFrame >= m_config.maxCatchupTicks) {
        m_droppedTicks += m_accUnits / kNsPerSecond;
        m_accUnits %= kNsPerSecond;
        return false;
    }

    m_tickOpen = true;
    return true;
}

void TimeManager::EndTick() noexcept {
    if (!m_tickOpen) {
        return;
    }

    m_accUnits -= kNsPerSecond;
    ++m_simTickCount;
    ++m_ticksThisFrame;

    // Hitstop is counted strictly in simulation ticks
    m_hitstop.AdvanceTick();

    m_tickOpen = false;
}

void TimeManager::EndFrame() noexcept {
    if (m_tickOpen) {
        return;
    }

    // Presentation interpolation alpha in [0.0, 1.0)
    const u64 remainder = m_accUnits % kNsPerSecond;
    m_alpha = std::clamp(static_cast<f32>(static_cast<f64>(remainder) / static_cast<f64>(kNsPerSecond)), 0.0f, 1.0f);
}

void TimeManager::SetPaused(bool paused) noexcept {
    if (m_paused == paused) {
        return;
    }

    m_paused = paused;
    if (!m_paused) {
        // Reset clock baseline so pause elapsed time does not spike on resume
        m_prevFrameNs = SDL_GetTicksNS();
    }
}

void TimeManager::ResetClocksOnRefocus() noexcept {
    if (m_tickOpen) {
        return;
    }

    m_prevFrameNs = SDL_GetTicksNS();
    m_accUnits = 0;
    m_alpha = 0.0f;
    m_hitstop.Reset();
}

void TimeManager::SetTimeScale(u8 channel, f32 scale) noexcept {
    if (channel >= kMaxTimeChannels || !std::isfinite(scale) || scale < 0.0f) {
        return;
    }
    m_channelScales[channel] = std::clamp(scale, 0.0f, 8.0f);
}

} // namespace EngineN

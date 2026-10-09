#include "TimeManager.h"
#include <algorithm>

namespace EngineN {

void TimeManager::Init(const TimeConfig& config) {
    m_config = config;
    if (m_config.simulationHz <= 0) m_config.simulationHz = 120;
    if (m_config.maxCatchupTicks <= 0) m_config.maxCatchupTicks = 8;

    m_fixedDtNs = 1'000'000'000ULL / static_cast<uint64_t>(m_config.simulationHz);
    m_fixedDtSec = 1.0 / static_cast<double>(m_config.simulationHz);
    m_maxFrameTimeNs = m_fixedDtNs * static_cast<uint64_t>(m_config.maxCatchupTicks);

    m_clock.Init();
    m_accumulatorNs = 0;
    m_alpha = 0.0;
    m_simTickCount = 0;
    m_ticksThisFrame = 0;

    m_pacer.SetTargetFps(m_config.targetRenderFps);
}

void TimeManager::BeginFrame() {
    uint64_t rawDeltaNs = m_clock.Sample();
    BeginFrameManual(rawDeltaNs);
}

void TimeManager::BeginFrameManual(uint64_t rawDeltaNs) {
    m_lastRawDeltaMs = static_cast<double>(rawDeltaNs) / 1'000'000.0;

    // Spiral-of-death safety clamp
    uint64_t clampedDeltaNs = std::min(rawDeltaNs, m_maxFrameTimeNs);

    // Wall-clock delta capped at 1 second to reject extreme suspension spikes
    uint64_t realClampedDeltaNs = std::min(rawDeltaNs, 1'000'000'000ULL);
    double realDtSec = static_cast<double>(realClampedDeltaNs) / 1'000'000'000.0;
    m_realTime.Advance(realDtSec, 1.0);

    if (!m_paused) {
        // Strictly bound total accumulated backlog to prevent runaway
        m_accumulatorNs = std::min(m_accumulatorNs + clampedDeltaNs, m_maxFrameTimeNs);

        // Update hitstop decay using real delta only when unpaused
        m_hitstop.Update(realDtSec);
    }

    m_ticksThisFrame = 0;
}

bool TimeManager::ConsumeTick() {
    if (m_paused || m_accumulatorNs < m_fixedDtNs) {
        return false;
    }

    // Prepare channel scaledDt for the tick that is about to execute
    m_worldTime.SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::World));
    m_playerTime.SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::Player));
    m_enemyTime.SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::Enemy));

    return true;
}

void TimeManager::EndTick() {
    m_accumulatorNs -= m_fixedDtNs;
    m_simTickCount++;
    m_ticksThisFrame++;

    // Commit time and tick counts to channels
    m_worldTime.CommitTick();
    m_playerTime.CommitTick();
    m_enemyTime.CommitTick();
}

void TimeManager::EndFrame() {
    m_alpha = static_cast<double>(m_accumulatorNs) / static_cast<double>(m_fixedDtNs);
}

void TimeManager::SetPaused(bool paused) {
    if (m_paused == paused) return;
    m_paused = paused;
    if (!m_paused) {
        // Reset clock delta so pause duration doesn't cause a spike on resume
        m_clock.ResetDelta();
    }
}

void TimeManager::ResetClocksOnRefocus() {
    m_clock.ResetDelta();
    m_accumulatorNs = 0;
    m_pacer.Reset();
}

} // namespace EngineN

#include "TimeManager.h"
#include <algorithm>
#include <cmath>

namespace EngineN {

// HitstopEngine
void HitstopEngine::Request(const HitstopRequest& req) noexcept {
    const size_t idx = static_cast<size_t>(req.channel);
    if (idx < m_remainingFreezeSec.size() && req.freezeDurationSec > 0.0) {
        // Non-stacking merge-by-max: prevents infinite freeze runaway during combos
        m_remainingFreezeSec[idx] = std::max(m_remainingFreezeSec[idx], req.freezeDurationSec);
    }
}

void HitstopEngine::Update(double realDtSec) noexcept {
    for (double& sec : m_remainingFreezeSec) {
        if (sec > 0.0) {
            sec = std::max(0.0, sec - realDtSec);
        }
    }
}

void HitstopEngine::Reset() noexcept {
    m_remainingFreezeSec.fill(0.0);
}

bool HitstopEngine::IsFrozen(HitstopChannel ch) const noexcept {
    const size_t idx = static_cast<size_t>(ch);
    return (idx < m_remainingFreezeSec.size()) && (m_remainingFreezeSec[idx] > 1e-9);
}

double HitstopEngine::GetModifier(HitstopChannel ch) const noexcept {
    return IsFrozen(ch) ? 0.0 : 1.0;
}

// FramePacer
void FramePacer::SetTargetFps(int fps) noexcept {
    m_targetFps = fps;
    m_targetFrameNs = (fps > 0) ? (kNsPerSec / static_cast<uint64_t>(fps)) : 0ULL;
    m_targetDeadlineNs = 0;
}

void FramePacer::Reset() noexcept {
    m_targetDeadlineNs = 0;
}

void FramePacer::WaitForTarget() noexcept {
    if (m_targetFrameNs == 0) {
        return; // Uncapped / vsync-driven presentation
    }

    uint64_t now = SDL_GetTicksNS();
    if (m_targetDeadlineNs == 0) {
        m_targetDeadlineNs = now + m_targetFrameNs;
        return;
    }

    if (now < m_targetDeadlineNs) {
        SDL_DelayPrecise(m_targetDeadlineNs - now);
    }

    // Step deadline forward by exact frame period (eliminates cumulative sleep drift)
    m_targetDeadlineNs += m_targetFrameNs;

    // Resynchronize if a severe hitch occurred (> 1 frame behind) to avoid burst catch-up
    now = SDL_GetTicksNS();
    if (now > m_targetDeadlineNs + m_targetFrameNs) {
        m_targetDeadlineNs = now + m_targetFrameNs;
    }
}

// TimeManager
void TimeManager::Init(const TimeConfig& config) noexcept {
    m_config = config;
    if (m_config.simulationHz <= 0) m_config.simulationHz = static_cast<int>(kDefaultSimHz);
    if (m_config.maxCatchupTicks <= 0) m_config.maxCatchupTicks = 8;

    m_fixedDtNs = kNsPerSec / static_cast<uint64_t>(m_config.simulationHz);
    m_fixedDtSec = 1.0 / static_cast<double>(m_config.simulationHz);
    m_maxFrameTimeNs = m_fixedDtNs * static_cast<uint64_t>(m_config.maxCatchupTicks);

    m_clock.Init();
    m_accumulatorNs = 0;
    m_alpha = 0.0;
    m_simTickCount = 0;
    m_ticksThisFrame = 0;
    m_lastRawDeltaMs = 0.0;
    m_paused = false;

    for (auto& ch : m_channels) {
        ch = TimeChannel{};
    }

    m_hitstop.Reset();
    m_pacer.SetTargetFps(m_config.targetRenderFps);
}

void TimeManager::BeginFrame() noexcept {
    const uint64_t rawDeltaNs = m_clock.Sample();
    BeginFrameManual(rawDeltaNs);
}

void TimeManager::BeginFrameManual(uint64_t rawDeltaNs) noexcept {
    m_lastRawDeltaMs = static_cast<double>(rawDeltaNs) / 1'000'000.0;

    // Spiral-of-death safety clamp
    const uint64_t clampedDeltaNs = std::min(rawDeltaNs, m_maxFrameTimeNs);

    // Wall-clock delta capped at 1 second to reject extreme OS suspension spikes
    const uint64_t realClampedDeltaNs = std::min(rawDeltaNs, kNsPerSec);
    const double realDtSec = static_cast<double>(realClampedDeltaNs) / static_cast<double>(kNsPerSec);

    // RealTime advances unconditionally via wall clock
    RealTime().Advance(realDtSec, 1.0);

    if (!m_paused) {
        // Strictly bound total accumulated backlog to prevent runaway
        m_accumulatorNs = std::min(m_accumulatorNs + clampedDeltaNs, m_maxFrameTimeNs);

        // Update hitstop decay using real delta only when unpaused
        m_hitstop.Update(realDtSec);
    }

    m_ticksThisFrame = 0;
}

bool TimeManager::ConsumeTick() noexcept {
    if (m_paused || m_accumulatorNs < m_fixedDtNs) {
        return false;
    }

    WorldTime().SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::World));
    PlayerTime().SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::Player));
    EnemyTime().SetModifier(m_fixedDtSec, m_hitstop.GetModifier(HitstopChannel::Enemy));

    return true;
}

void TimeManager::EndTick() noexcept {
    m_accumulatorNs -= m_fixedDtNs;
    m_simTickCount++;
    m_ticksThisFrame++;

    WorldTime().CommitTick();
    PlayerTime().CommitTick();
    EnemyTime().CommitTick();
}

void TimeManager::EndFrame() noexcept {
    m_alpha = static_cast<double>(m_accumulatorNs) / static_cast<double>(m_fixedDtNs);
}

void TimeManager::SetPaused(bool paused) noexcept {
    if (m_paused == paused) return;
    m_paused = paused;
    if (!m_paused) {
        // Reset clock delta so pause duration doesn't cause a spike on resume
        m_clock.ResetDelta();
    }
}

void TimeManager::ResetClocksOnRefocus() noexcept {
    m_clock.ResetDelta();
    m_accumulatorNs = 0;
    m_pacer.Reset();
    m_hitstop.Reset();
}

} // namespace EngineN

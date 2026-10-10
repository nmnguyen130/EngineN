#include "TimeManager.h"

#include <algorithm>
#include <cmath>

namespace EngineN {

namespace {

constexpr std::size_t Index(ChannelId channel) noexcept {
    return static_cast<std::size_t>(channel);
}

constexpr std::size_t Index(HitstopChannel channel) noexcept {
    return static_cast<std::size_t>(channel);
}

} // namespace

// HighResolutionClock
void HighResolutionClock::Reset() noexcept {
    m_prevTicksNs = SDL_GetTicksNS();
    m_initialized = true;
}

std::uint64_t HighResolutionClock::Sample() noexcept {
    const std::uint64_t now = SDL_GetTicksNS();
    if (!m_initialized) {
        m_prevTicksNs = now;
        m_initialized = true;
        return 0ULL;
    }

    const std::uint64_t deltaNs = (now >= m_prevTicksNs) ? (now - m_prevTicksNs) : 0ULL;
    m_prevTicksNs = now;
    return deltaNs;
}

// TimeChannel
void TimeChannel::SetTimeScale(double scale) noexcept {
    if (!std::isfinite(scale) || scale < 0.0) {
        return;
    }
    m_timeScale = std::clamp(scale, 0.0, 8.0);
    m_scaleQ16 = ToQ16(static_cast<float>(m_timeScale));
}

void TimeChannel::SetRealDelta(double dtSec) noexcept {
    m_scaledDt = dtSec;
    m_totalTime += dtSec;
    ++m_simTicks;
}

void TimeChannel::PrepareTick(double fixedDtSec, double hitstopModifier) noexcept {
    m_scaledDt = fixedDtSec * m_timeScale * hitstopModifier;
}

void TimeChannel::CommitTick() noexcept {
    m_totalTime += m_scaledDt;
    ++m_simTicks;
    // Advance Q16 fractional phase for domain sub-tick interpolation
    m_accQ16 = (m_accQ16 + m_effScaleQ16) & 0xFFFFu;
}

// HitstopEngine
void HitstopEngine::Request(const HitstopRequest& req) noexcept {
    const std::size_t idx = Index(req.channel);
    if (idx >= m_remainingSec.size() || !std::isfinite(req.durationSec) || req.durationSec <= 0.0) {
        return;
    }

    // Non-stacking merge-by-max: retains longest freeze to prevent infinite stun locks
    m_remainingSec[idx] = std::max(m_remainingSec[idx], req.durationSec);
}

void HitstopEngine::Update(double realDtSec) noexcept {
    if (!std::isfinite(realDtSec) || realDtSec <= 0.0) {
        return;
    }

    for (double& sec : m_remainingSec) {
        if (sec > 0.0) {
            sec = (sec > realDtSec + 1e-9) ? (sec - realDtSec) : 0.0;
        }
    }
}

void HitstopEngine::Reset() noexcept {
    m_remainingSec.fill(0.0);
}

// FramePacer
void FramePacer::SetTargetFps(int fps) noexcept {
    m_targetFps = (fps <= 0) ? 0 : std::min(fps, 1000);
    m_targetFrameNs = (m_targetFps > 0) ? (kNsPerSec / static_cast<std::uint64_t>(m_targetFps)) : 0ULL;
    Reset();
}

void FramePacer::Reset() noexcept {
    m_targetDeadlineNs = 0;
}

void FramePacer::WaitForTarget() noexcept {
    if (m_targetFrameNs == 0) {
        return; // Uncapped or VSync-driven presentation
    }

    std::uint64_t now = SDL_GetTicksNS();
    if (m_targetDeadlineNs == 0) {
        m_targetDeadlineNs = now + m_targetFrameNs;
        return;
    }

    if (now < m_targetDeadlineNs) {
        SDL_DelayPrecise(m_targetDeadlineNs - now);
    }

    // Advance deadline by exact frame period (eliminates cumulative sleep drift)
    m_targetDeadlineNs += m_targetFrameNs;

    // Resynchronize if a severe hitch occurred (> 1 frame behind) to avoid burst catch-up
    now = SDL_GetTicksNS();
    if (now > m_targetDeadlineNs && (now - m_targetDeadlineNs) > m_targetFrameNs) {
        m_targetDeadlineNs = now + m_targetFrameNs;
    }
}

// TimeManager
void TimeManager::Init(const TimeConfig& config) noexcept {
    m_config = config;
    m_simHz = (m_config.simulationHz > 0) ? static_cast<std::uint64_t>(m_config.simulationHz) : kDefaultSimHz;
    m_fixedDtNs = kNsPerSec / m_simHz;
    m_fixedDtSec = 1.0 / static_cast<double>(m_simHz);

    const std::uint64_t catchupCap = static_cast<std::uint64_t>(std::max(1, m_config.maxCatchupTicks));
    m_maxFrameNs = (m_config.maxFrameNs > 0) ? m_config.maxFrameNs : (m_fixedDtNs * catchupCap);

    m_clock.Init();
    m_accNsHz = 0;
    m_alpha = 0.0;
    m_simTickCount = 0;
    m_ticksThisFrame = 0;
    m_lastRawDeltaMs = 0.0;
    m_paused = false;
    m_droppedTime = false;
    m_tickOpen = false;

    for (TimeChannel& ch : m_channels) {
        ch = TimeChannel{};
    }

    m_hitstop.Reset();
    m_pacer.SetTargetFps(m_config.targetRenderFps);
}

void TimeManager::BeginFrame() noexcept {
    BeginFrameManual(m_clock.Sample());
}

void TimeManager::BeginFrameManual(std::uint64_t rawDeltaNs) noexcept {
    m_lastRawDeltaMs = static_cast<double>(rawDeltaNs) / 1'000'000.0;
    m_droppedTime = false;
    m_ticksThisFrame = 0;

    // Real wall-clock delta capped at 1 second to reject extreme OS sleep spikes
    const std::uint64_t realClampedDeltaNs = std::min(rawDeltaNs, kNsPerSec);
    const double realDtSec = static_cast<double>(realClampedDeltaNs) / static_cast<double>(kNsPerSec);

    // RealTime channel advances unconditionally via wall clock
    m_channels[Index(ChannelId::Real)].SetRealDelta(realDtSec);

    // During pause, preserve accumulator backlog & hitstop decay without advancing sim
    if (m_paused || rawDeltaNs == 0) {
        m_alpha = static_cast<double>(m_accNsHz) / static_cast<double>(kNsPerSec);
        return;
    }

    // Decay combat hitstop only while unpaused
    m_hitstop.Update(realDtSec);

    // Spiral-of-death safety clamp
    std::uint64_t dtNs = rawDeltaNs;
    if (dtNs > m_maxFrameNs) {
        dtNs = m_maxFrameNs;
        m_droppedTime = true;
    }

    // Accumulate in integer ns x Hz units (Zero-Drift exact math)
    m_accNsHz += dtNs * m_simHz;

    UpdateDomainDeltas();
}

void TimeManager::UpdateDomainDeltas() noexcept {
    const double worldMod = m_hitstop.GetModifier(HitstopChannel::World);
    const double playerMod = m_hitstop.GetModifier(HitstopChannel::Player);
    const double enemyMod = m_hitstop.GetModifier(HitstopChannel::Enemy);

    WorldTime().m_effScaleQ16 = (worldMod > 0.0) ? WorldTime().m_scaleQ16 : 0;
    PlayerTime().m_effScaleQ16 = (playerMod > 0.0) ? PlayerTime().m_scaleQ16 : 0;
    EnemyTime().m_effScaleQ16 = (enemyMod > 0.0) ? EnemyTime().m_scaleQ16 : 0;

    WorldTime().PrepareTick(m_fixedDtSec, worldMod);
    PlayerTime().PrepareTick(m_fixedDtSec, playerMod);
    EnemyTime().PrepareTick(m_fixedDtSec, enemyMod);
}

bool TimeManager::ConsumeTick() noexcept {
    if (m_paused || m_tickOpen || m_accNsHz < kNsPerSec) {
        return false;
    }

    // Spiral-of-death catchup cap
    if (m_ticksThisFrame >= m_config.maxCatchupTicks) {
        // Discard whole backlog, preserve sub-tick phase to avoid visual judder
        m_accNsHz %= kNsPerSec;
        m_droppedTime = true;
        return false;
    }

    UpdateDomainDeltas();
    m_tickOpen = true;
    return true;
}

void TimeManager::EndTick() noexcept {
    if (!m_tickOpen) {
        return;
    }

    m_accNsHz -= kNsPerSec;
    ++m_simTickCount;
    ++m_ticksThisFrame;

    WorldTime().CommitTick();
    PlayerTime().CommitTick();
    EnemyTime().CommitTick();

    m_tickOpen = false;
}

void TimeManager::EndFrame() noexcept {
    if (m_tickOpen) {
        return;
    }

    m_alpha = std::clamp(static_cast<double>(m_accNsHz) / static_cast<double>(kNsPerSec), 0.0, 1.0);
}

void TimeManager::SetPaused(bool paused) noexcept {
    if (m_paused == paused) {
        return;
    }

    m_paused = paused;
    if (!m_paused) {
        // Reset clock delta so pause duration doesn't cause a spike on resume
        m_clock.Reset();
    }
}

void TimeManager::ResetClocksOnRefocus() noexcept {
    if (m_tickOpen) {
        return;
    }

    m_clock.Reset();
    m_accNsHz = 0;
    m_alpha = 0.0;
    m_pacer.Reset();
    m_hitstop.Reset();
}

double TimeManager::GetDomainAlpha(ChannelId ch) const noexcept {
    const std::size_t idx = Index(ch);
    if (idx >= m_channels.size()) {
        return m_alpha;
    }

    const TimeChannel& d = m_channels[idx];
    const double progress = (static_cast<double>(d.m_accQ16) + m_alpha * static_cast<double>(d.m_effScaleQ16)) / static_cast<double>(kQ16One);
    return std::clamp(progress, 0.0, 1.0);
}

} // namespace EngineN

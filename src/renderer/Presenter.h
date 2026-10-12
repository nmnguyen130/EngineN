#pragma once

#include "core/Types.h"
#include "renderer/RenderBackend.h"
#include <SDL3/SDL.h>
#include <algorithm>

namespace EngineN {

/**
 * FramePacer: Frame rate limiter using SDL_DelayPrecise.
 * Active exclusively in Capped presentation mode.
 */
class FramePacer {
public:
    void SetTargetFps(u32 fps) noexcept {
        m_targetFps = fps;
        m_frameNs = (fps > 0) ? (1'000'000'000ULL / fps) : 0ULL;
        Reset();
    }

    [[nodiscard]] u32 GetTargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] u64 GetTargetFrameNs() const noexcept { return m_frameNs; }
    [[nodiscard]] u64 GetTargetDeadlineNs() const noexcept { return m_targetDeadlineNs; }

    void Reset() noexcept {
        m_targetDeadlineNs = 0;
    }

    void WaitForTarget() noexcept {
        if (m_frameNs == 0) {
            return;
        }

        const u64 now = SDL_GetTicksNS();
        if (m_targetDeadlineNs == 0) {
            m_targetDeadlineNs = now + m_frameNs;
            return;
        }

        if (now < m_targetDeadlineNs) {
            SDL_DelayPrecise(m_targetDeadlineNs - now);
        }

        m_targetDeadlineNs += m_frameNs;

        const u64 afterWait = SDL_GetTicksNS();
        if (afterWait > m_targetDeadlineNs && (afterWait - m_targetDeadlineNs) > m_frameNs) {
            m_targetDeadlineNs = afterWait + m_frameNs;
        }
    }

private:
    u32 m_targetFps = 0;
    u64 m_frameNs = 0;
    u64 m_targetDeadlineNs = 0;
};

/**
 * Presenter: Consolidates swapchain policy, presentation execution, and software pacing.
 */
class Presenter {
public:
    void SetPolicy(const PresentationPolicy& p) noexcept {
        m_policy = p;
        m_pacer.SetTargetFps(p.mode == PacingMode::Capped ? p.capFps : 0);
        m_pacer.Reset();
    }

    [[nodiscard]] const PresentationPolicy& Policy() const noexcept { return m_policy; }
    [[nodiscard]] FramePacer& Pacer() noexcept { return m_pacer; }

    void BeginFrame(RenderBackend& b) noexcept {
        b.WaitForNextFrame();
    }

    [[nodiscard]] Result<PresentResult, EngineError> EndFrame(RenderBackend& b) noexcept {
        auto r = b.Present(m_policy);
        if (m_policy.mode == PacingMode::Capped) {
            m_pacer.WaitForTarget();
        }
        return r;
    }

private:
    PresentationPolicy m_policy{};
    FramePacer         m_pacer{};
};

} // namespace EngineN

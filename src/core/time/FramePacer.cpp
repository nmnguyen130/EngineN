#include "FramePacer.h"
#include <SDL3/SDL.h>

namespace EngineN {

void FramePacer::SetTargetFps(int fps) {
    m_targetFps = fps;
    m_targetFrameNs = (fps > 0) ? (1'000'000'000ULL / static_cast<uint64_t>(fps)) : 0;
    m_targetDeadlineNs = 0;
}

void FramePacer::Reset() {
    m_targetDeadlineNs = 0;
}

void FramePacer::WaitForTarget() {
    if (m_targetFrameNs == 0) {
        return; // Uncapped / VSync handled by presentation
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

} // namespace EngineN

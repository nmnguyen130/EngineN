#include "time/SdlClock.h"
#include "time/TimePipeline.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>

namespace en {

u64 SdlClock::NowNs() noexcept {
    return static_cast<u64>(SDL_GetTicksNS());
}

void SdlClock::SleepNs(u64 ns) noexcept {
    if (ns > 0ULL) SDL_DelayPrecise(ns);
}

void SdlClock::YieldThread() noexcept {
    SDL_DelayNS(0ULL);
}

void FramePacer::BeginFrame() noexcept {
    m_start = SdlClock::NowNs();
}

void FramePacer::EndFrameAndPace() noexcept {
    m_lastSleep = 0U;
    m_lastSpin  = 0U;
    if (m_targetNs == 0ULL) return;

    u64 now = SdlClock::NowNs();
    if (m_start == 0ULL || now < m_start) {
        m_start = now;
        return;
    }

    const u64 end = m_start + m_targetNs;
    if (now < end) {
        const u64 remain = end - now;
        if (remain > m_margin) {
            const u64 req = remain - m_margin;
            SdlClock::SleepNs(req);
            const u64 after = SdlClock::NowNs();
            const u64 slept = (after > now) ? (after - now) : req;
            m_lastSleep = static_cast<u32>(std::min<u64>(slept, 0xFFFFFFFFULL));

            const u64 over = (slept > req) ? (slept - req) : 0ULL;
            const u32 sample = static_cast<u32>(
                std::clamp<u64>(over + kMinMarginNs, kMinMarginNs, kMaxMarginNs));
            m_margin = static_cast<u32>((static_cast<u64>(m_margin) * 7ULL + sample) >> 3);
            now = after;
        }

        const u64 spinStart = now;
        while (now < end) {
            SdlClock::YieldThread();
            now = SdlClock::NowNs();
        }
        m_lastSpin = static_cast<u32>(std::min<u64>(now - spinStart, 0xFFFFFFFFULL));
    }

    m_start = (now - end < m_targetNs) ? end : now;
}

} // namespace en

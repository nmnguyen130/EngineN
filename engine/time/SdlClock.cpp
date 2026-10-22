#include "time/SdlClock.h"
#include "time/TimePipeline.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>

namespace en {

u64 SdlClock::NowNs() noexcept {
    return static_cast<u64>(SDL_GetTicksNS());
}

void SdlClock::SleepNs(u64 durationNs) noexcept {
    if (durationNs > 0ULL) {
        SDL_DelayNS(durationNs);
    }
}

void SdlClock::YieldThread() noexcept {
    SDL_DelayNS(0ULL);
}

void FramePacer::EndFrameAndPace() noexcept {
    m_lastSleepNs = 0U;
    m_lastSpinNs = 0U;
    if (m_targetFrameNs == 0ULL) return;

    u64 now = SdlClock::NowNs();
    if (m_frameStartNs == 0ULL || now < m_frameStartNs) {
        m_frameStartNs = now;
        return;
    }

    const u64 targetEndNs = m_frameStartNs + m_targetFrameNs;
    if (now < targetEndNs) {
        const u64 remainingNs = targetEndNs - now;
        if (remainingNs > m_sleepMarginNs) {
            const u64 reqSleepNs = remainingNs - m_sleepMarginNs;
            SdlClock::SleepNs(reqSleepNs);
            const u64 afterSleep = SdlClock::NowNs();
            const u64 actualSleptNs = (afterSleep > now) ? (afterSleep - now) : reqSleepNs;
            m_lastSleepNs = static_cast<u32>(std::min<u64>(actualSleptNs, 0xFFFFFFFFULL));

            const u64 overshootNs = (actualSleptNs > reqSleepNs) ? (actualSleptNs - reqSleepNs) : 0ULL;
            const u32 sampleMargin = static_cast<u32>(
                std::clamp<u64>(overshootNs + kMinSpinMarginNs, kMinSpinMarginNs, kMaxSpinMarginNs));
            m_sleepMarginNs = static_cast<u32>((static_cast<u64>(m_sleepMarginNs) * 7ULL + sampleMargin) >> 3U);
            now = afterSleep;
        }

        const u64 spinStart = now;
        while (now < targetEndNs) {
            SdlClock::YieldThread();
            now = SdlClock::NowNs();
        }
        m_lastSpinNs = static_cast<u32>(std::min<u64>((now > spinStart) ? (now - spinStart) : 0ULL, 0xFFFFFFFFULL));
    }

    m_frameStartNs = (now - targetEndNs < m_targetFrameNs) ? targetEndNs : now;
}

} // namespace en

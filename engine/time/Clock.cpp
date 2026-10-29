#include "time/Clock.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace en {

u64 Clock::NowNs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void Clock::SleepNs(u64 ns) noexcept {
    if (ns > 0ULL) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void Clock::YieldThread() noexcept {
    std::this_thread::yield();
}

void FramePacer::BeginFrame() noexcept {
    if (m_frameStartNs == 0ULL) m_frameStartNs = Clock::NowNs();
}

void FramePacer::EndFrame() noexcept {
    if (m_framePeriodNs == 0ULL) return;
    u64 now = Clock::NowNs();
    if (m_frameStartNs == 0ULL || now < m_frameStartNs) {
        m_frameStartNs = now;
        return;
    }

    const u64 targetEndNs = m_frameStartNs + m_framePeriodNs;
    if (now < targetEndNs) {
        const u64 remain = targetEndNs - now;
        if (remain > m_marginNs) {
            const u64 req = remain - m_marginNs;
            Clock::SleepNs(req);
            const u64 after = Clock::NowNs();
            const u64 slept = (after > now) ? (after - now) : req;
            const u64 over  = (slept > req) ? (slept - req) : 0ULL;
            const u32 sample = static_cast<u32>(
                std::clamp<u64>(over + kMinMarginNs, kMinMarginNs, kMaxMarginNs));
            m_marginNs = static_cast<u32>((static_cast<u64>(m_marginNs) * 7ULL + sample) >> 3U);
            now = after;
        }
        while (now < targetEndNs) {
            Clock::YieldThread();
            now = Clock::NowNs();
        }
    }
    m_frameStartNs = (now - targetEndNs < m_framePeriodNs) ? targetEndNs : now;
}

} // namespace en

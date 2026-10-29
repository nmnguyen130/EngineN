#pragma once

#include "core/Types.h"

namespace en {

// Monotonic high-resolution OS clock and thread yield wrapper using standard C++23 chrono.
struct Clock {
    [[nodiscard]] static u64 NowNs() noexcept;
    static void SleepNs(u64 ns) noexcept;
    static void YieldThread() noexcept;
};

// Adaptive sleep-margin EMA + spin-yield frame rate limiter with phase-locked deadline stepping.
class FramePacer {
public:
    static constexpr u32 kMinMarginNs     = 50'000U;
    static constexpr u32 kDefaultMarginNs = 150'000U;
    static constexpr u32 kMaxMarginNs     = 500'000U;

    constexpr explicit FramePacer(u32 targetFps = 0U) noexcept { SetTargetFps(targetFps); }

    constexpr void SetTargetFps(u32 fps) noexcept {
        m_targetFps     = fps;
        m_framePeriodNs = (fps > 0U) ? (1'000'000'000ULL / fps) : 0ULL;
    }

    [[nodiscard]] constexpr u32 TargetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] constexpr u32 SleepMarginNs() const noexcept { return m_marginNs; }

    void BeginFrame() noexcept;
    void EndFrame() noexcept;

private:
    u64 m_frameStartNs{0};
    u64 m_framePeriodNs{0};
    u32 m_marginNs{kDefaultMarginNs};
    u32 m_targetFps{0};
};
static_assert(sizeof(FramePacer) == 24);

} // namespace en

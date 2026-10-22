#pragma once

#include "core/Types.h"

namespace en {

// Layer 2 thin hardware clock and high-precision sleep seam backed by SDL3 (<SDL3/SDL_timer.h>).
// Stateless static interface; isolates OS/SDL timer dependencies from Layer 1 constexpr TimePipeline.
struct SdlClock {
    [[nodiscard]] static u64 NowNs() noexcept;
    static void SleepNs(u64 durationNs) noexcept;
    static void YieldThread() noexcept;
};

} // namespace en

#pragma once

#include "core/Types.h"

namespace en {

// Thin SDL3 monotonic clock + sleep. No sim logic.
struct SdlClock {
    [[nodiscard]] static u64 NowNs() noexcept;
    static void SleepNs(u64 durationNs) noexcept;
    static void YieldThread() noexcept;
};

} // namespace en

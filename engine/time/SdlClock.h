#pragma once

#include "core/Types.h"

namespace en {

// Monotonic high-resolution clock and thread sleep wrapper backed by SDL3 timer routines.
struct SdlClock {
    [[nodiscard]] static u64 NowNs() noexcept;
    static void SleepNs(u64 durationNs) noexcept;
    static void YieldThread() noexcept;
};

} // namespace en

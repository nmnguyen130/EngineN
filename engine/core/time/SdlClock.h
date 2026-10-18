#pragma once

#include "core/time/TimePipeline.h"

namespace en::time {

// Layer 2 wall-clock adapter querying SDL3's nanosecond monotonic timer.
// Confines <SDL3/SDL_timer.h> strictly to SdlClock.cpp.
struct SdlClock {
    [[nodiscard]] u64 NowNs() const noexcept;
};

using TimeManager = TimePipeline<SdlClock>;

} // namespace en::time

#pragma once

#include "core/time/TimePipeline.h"

#include <SDL3/SDL.h>

namespace EngineN {

/**
 * SdlClock: Production wall-clock source querying SDL3's nanosecond timer.
 */
struct SdlClock {
    [[nodiscard]] u64 NowNs() const noexcept {
        return SDL_GetTicksNS();
    }
};

/**
 * ManualClock: Deterministic mock clock for unit tests and headless replay.
 * Advances simulated time explicitly without platform or OS timing dependencies.
 */
struct ManualClock {
    u64 currentNs = 0;

    [[nodiscard]] constexpr u64 NowNs() const noexcept { return currentNs; }

    constexpr void AdvanceNs(u64 deltaNs) noexcept { currentNs += deltaNs; }
    constexpr void AdvanceMs(u64 deltaMs) noexcept { currentNs += deltaMs * 1'000'000ULL; }
};

using TimeManager = TimePipeline<SdlClock>;

} // namespace EngineN
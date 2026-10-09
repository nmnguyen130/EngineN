#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace EngineN {

struct HighResolutionClock {
    uint64_t previousTicks = 0;

    void Init() {
        previousTicks = SDL_GetTicksNS();
    }

    // Samples elapsed nanoseconds since last call to Sample() (rejects backward jumps)
    uint64_t Sample() {
        uint64_t now = SDL_GetTicksNS();
        uint64_t deltaNs = (now >= previousTicks) ? (now - previousTicks) : 0ULL;
        previousTicks = now;
        return deltaNs;
    }

    // Reset previous ticks after focus loss / pause to prevent delta spikes
    void ResetDelta() {
        previousTicks = SDL_GetTicksNS();
    }
};

struct TimeChannel {
    const char* name = "Channel";
    double timeScale = 1.0;
    double scaledDt = 0.0;
    double totalTime = 0.0;

    void SetModifier(double rawFixedDtSec, double hitstopModifier) {
        double effectiveScale = timeScale * hitstopModifier;
        scaledDt = rawFixedDtSec * effectiveScale;
    }

    void CommitTick() {
        totalTime += scaledDt;
    }

    void Advance(double rawFixedDtSec, double hitstopModifier) {
        SetModifier(rawFixedDtSec, hitstopModifier);
        CommitTick();
    }

    void SetTimeScale(double scale) {
        if (!std::isfinite(scale) || scale < 0.0) {
            return; // Reject negative, NaN, Inf
        }
        timeScale = std::clamp(scale, 0.0, 8.0);
    }
};

} // namespace EngineN

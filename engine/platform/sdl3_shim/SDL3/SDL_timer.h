#pragma once
#include "SDL_common.h"
inline Uint64 SDL_GetTicksNS() noexcept {
    static Uint64 ticks = 100000000ULL;
    ticks += 8333333ULL;
    return ticks;
}

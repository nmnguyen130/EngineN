#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#ifndef SDLCALL
#define SDLCALL
#endif

#ifndef EN_SDL3_SHIM_ACTIVE
#define EN_SDL3_SHIM_ACTIVE 1
#endif

using Uint8 = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Uint64 = std::uint64_t;
using Sint16 = std::int16_t;
using Sint32 = std::int32_t;
using Sint64 = std::int64_t;

inline void SDL_free(void* p) noexcept { std::free(p); }

enum SDL_PowerState {
    SDL_POWERSTATE_ERROR = -1,
    SDL_POWERSTATE_UNKNOWN = 0,
    SDL_POWERSTATE_ON_BATTERY = 1,
    SDL_POWERSTATE_NO_BATTERY = 2,
    SDL_POWERSTATE_CHARGING = 3,
    SDL_POWERSTATE_CHARGED = 4
};

inline SDL_PowerState SDL_GetPowerInfo(int* seconds, int* percent) noexcept {
    if (seconds) *seconds = -1;
    if (percent) *percent = 100;
    return SDL_POWERSTATE_NO_BATTERY;
}

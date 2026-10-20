#pragma once
#include "SDL_common.h"
using SDL_InitFlags = Uint32;
inline constexpr SDL_InitFlags SDL_INIT_VIDEO = 0x00000020u;
inline constexpr SDL_InitFlags SDL_INIT_GAMEPAD = 0x00002000u;
inline bool SDL_Init(SDL_InitFlags) noexcept { return true; }
inline void SDL_Quit() noexcept {}

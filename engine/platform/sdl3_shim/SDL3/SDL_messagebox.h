#pragma once
#include "SDL_common.h"
struct SDL_Window;
inline constexpr Uint32 SDL_MESSAGEBOX_ERROR = 0x00000010u;
inline bool SDL_ShowSimpleMessageBox(Uint32, const char*, const char*, SDL_Window*) noexcept { return true; }

#pragma once
#include "SDL_video.h"
inline bool SDL_StartTextInput(SDL_Window*) noexcept { return true; }
inline bool SDL_StopTextInput(SDL_Window*) noexcept { return true; }

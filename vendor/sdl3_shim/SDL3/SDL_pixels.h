#pragma once
#include "SDL_common.h"
enum SDL_PixelFormat : Uint32 {
    SDL_PIXELFORMAT_RGBA32 = 0x16762004u,
    SDL_PIXELFORMAT_BGRA32 = 0x16362004u
};
struct SDL_FColor { float r, g, b, a; };

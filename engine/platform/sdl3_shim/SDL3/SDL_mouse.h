#pragma once
#include "SDL_video.h"
inline constexpr Uint32 SDL_TOUCH_MOUSEID = 0xFFFFFFFFu;
inline constexpr int SDL_MOUSEWHEEL_NORMAL = 0;
inline constexpr int SDL_MOUSEWHEEL_FLIPPED = 1;
inline bool SDL_SetWindowRelativeMouseMode(SDL_Window* win, bool enabled) noexcept {
    if (!win) return false;
    win->relativeMouse = enabled;
    return true;
}
inline bool SDL_GetWindowRelativeMouseMode(SDL_Window* win) noexcept { return win ? win->relativeMouse : false; }
inline bool SDL_ShowCursor() noexcept { return true; }
inline bool SDL_HideCursor() noexcept { return true; }

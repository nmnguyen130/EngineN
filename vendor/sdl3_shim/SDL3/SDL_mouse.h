#pragma once
#include "SDL_video.h"

inline constexpr Uint32 SDL_TOUCH_MOUSEID = 0xFFFFFFFFu;
inline constexpr int SDL_MOUSEWHEEL_NORMAL = 0;
inline constexpr int SDL_MOUSEWHEEL_FLIPPED = 1;

enum SDL_SystemCursor {
    SDL_SYSTEM_CURSOR_DEFAULT = 0,
    SDL_SYSTEM_CURSOR_TEXT = 1,
    SDL_SYSTEM_CURSOR_WAIT = 2,
    SDL_SYSTEM_CURSOR_CROSSHAIR = 3,
    SDL_SYSTEM_CURSOR_PROGRESS = 4,
    SDL_SYSTEM_CURSOR_NWSE_RESIZE = 5,
    SDL_SYSTEM_CURSOR_NESW_RESIZE = 6,
    SDL_SYSTEM_CURSOR_EW_RESIZE = 7,
    SDL_SYSTEM_CURSOR_NS_RESIZE = 8,
    SDL_SYSTEM_CURSOR_MOVE = 9,
    SDL_SYSTEM_CURSOR_NOT_ALLOWED = 10,
    SDL_SYSTEM_CURSOR_POINTER = 11
};

struct SDL_Cursor {
    SDL_SystemCursor id{SDL_SYSTEM_CURSOR_DEFAULT};
};

inline SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) noexcept {
    static SDL_Cursor s_cursors[16]{};
    const unsigned idx = static_cast<unsigned>(id) & 15U;
    s_cursors[idx].id = id;
    return &s_cursors[idx];
}

inline void SDL_DestroyCursor(SDL_Cursor*) noexcept {}
inline bool SDL_SetCursor(SDL_Cursor*) noexcept { return true; }

inline bool SDL_SetWindowRelativeMouseMode(SDL_Window* win, bool enabled) noexcept {
    if (!win) return false;
    win->relativeMouse = enabled;
    return true;
}
inline bool SDL_GetWindowRelativeMouseMode(SDL_Window* win) noexcept { return win ? win->relativeMouse : false; }
inline bool SDL_ShowCursor() noexcept { return true; }
inline bool SDL_HideCursor() noexcept { return true; }
inline void SDL_WarpMouseInWindow(SDL_Window*, float, float) noexcept {}

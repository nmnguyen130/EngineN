#pragma once
#include "SDL_common.h"
using SDL_WindowID = Uint32;
using SDL_DisplayID = Uint32;
using SDL_WindowFlags = Uint64;

inline constexpr SDL_WindowFlags SDL_WINDOW_FULLSCREEN = 0x1ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_OPENGL = 0x2ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_OCCLUDED = 0x4ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_HIDDEN = 0x8ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_RESIZABLE = 0x20ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_MINIMIZED = 0x40ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_INPUT_FOCUS = 0x200ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_HIGH_PIXEL_DENSITY = 0x2000ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_VULKAN = 0x10000000ULL;
inline constexpr SDL_WindowFlags SDL_WINDOW_METAL = 0x20000000ULL;

struct SDL_DisplayMode {
    SDL_DisplayID displayID{1};
    int w{1920};
    int h{1080};
    float pixel_density{1.0f};
    float refresh_rate{60.0f};
};

struct SDL_Window {
    SDL_WindowID id{1};
    int w{1280};
    int h{720};
    SDL_WindowFlags flags{SDL_WINDOW_INPUT_FOCUS};
    bool relativeMouse{false};
    void* nativeHwnd{nullptr};
};
inline SDL_WindowID SDL_GetWindowID(SDL_Window* win) noexcept { return win ? win->id : 0; }
inline bool SDL_SetWindowMinimumSize(SDL_Window*, int, int) noexcept { return true; }
inline bool SDL_SetWindowFullscreenMode(SDL_Window*, const SDL_DisplayMode*) noexcept { return true; }
inline bool SDL_SetWindowFullscreen(SDL_Window* win, bool fullscreen) noexcept {
    if (!win) return false;
    if (fullscreen) win->flags |= SDL_WINDOW_FULLSCREEN;
    else win->flags &= ~SDL_WINDOW_FULLSCREEN;
    return true;
}
inline bool SDL_SyncWindow(SDL_Window*) noexcept { return true; }
inline bool SDL_SetWindowTitle(SDL_Window*, const char*) noexcept { return true; }
inline void SDL_GetWindowSize(SDL_Window* win, int* w, int* h) noexcept {
    if (w) *w = win ? win->w : 0;
    if (h) *h = win ? win->h : 0;
}
inline void SDL_GetWindowSizeInPixels(SDL_Window* win, int* w, int* h) noexcept {
    if (w) *w = win ? win->w : 0;
    if (h) *h = win ? win->h : 0;
}
inline float SDL_GetWindowPixelDensity(SDL_Window*) noexcept { return 1.0f; }
inline float SDL_GetWindowDisplayScale(SDL_Window*) noexcept { return 1.0f; }
inline SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) noexcept { return 1; }
inline const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID) noexcept {
    static const SDL_DisplayMode dm{};
    return &dm;
}
inline const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID) noexcept {
    static const SDL_DisplayMode dm{};
    return &dm;
}
inline bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID, int w, int h, float, bool, SDL_DisplayMode* closest) noexcept {
    if (closest) { closest->w = w; closest->h = h; closest->refresh_rate = 60.0f; }
    return true;
}
inline SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* win) noexcept { return win ? win->flags : 0; }

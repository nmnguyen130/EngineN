#pragma once
#include "SDL_video.h"
#include "SDL_events.h"
#include "SDL_blendmode.h"
#include "SDL_pixels.h"
#include "SDL_rect.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct SDL_Renderer { SDL_Window* win{nullptr}; };
struct SDL_Texture { int w{0}, h{0}; SDL_BlendMode blend{SDL_BLENDMODE_BLEND_PREMULTIPLIED}; };

enum SDL_TextureAccess { SDL_TEXTUREACCESS_STATIC = 0 };
enum SDL_ScaleMode { SDL_SCALEMODE_NEAREST = 0, SDL_SCALEMODE_LINEAR = 1 };
inline constexpr int SDL_RENDERER_VSYNC_ADAPTIVE = -1;

inline SDL_Renderer* SDL_CreateRenderer(SDL_Window* win, const char*) noexcept {
    return new SDL_Renderer{win};
}
inline void SDL_DestroyRenderer(SDL_Renderer* r) noexcept { delete r; }
inline bool SDL_SetRenderVSync(SDL_Renderer*, int) noexcept { return true; }
inline SDL_Texture* SDL_CreateTexture(SDL_Renderer*, SDL_PixelFormat, SDL_TextureAccess, int w, int h) noexcept {
    return new SDL_Texture{w, h, SDL_BLENDMODE_BLEND_PREMULTIPLIED};
}
inline void SDL_DestroyTexture(SDL_Texture* t) noexcept { delete t; }
inline bool SDL_UpdateTexture(SDL_Texture*, const SDL_Rect*, const void*, int) noexcept { return true; }
inline bool SDL_SetTextureScaleMode(SDL_Texture*, SDL_ScaleMode) noexcept { return true; }
inline bool SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode b) noexcept { if (t) t->blend = b; return true; }
inline bool SDL_SetRenderViewport(SDL_Renderer*, const SDL_Rect*) noexcept { return true; }
inline bool SDL_SetRenderClipRect(SDL_Renderer*, const SDL_Rect*) noexcept { return true; }
inline bool SDL_SetRenderDrawColorFloat(SDL_Renderer*, float, float, float, float) noexcept { return true; }
inline bool SDL_RenderClear(SDL_Renderer*) noexcept { return true; }
inline bool SDL_RenderGeometryRaw(SDL_Renderer*, SDL_Texture*, const float*, int, const SDL_FColor*, int, const float*, int, int, const void*, int, int) noexcept { return true; }
inline bool SDL_RenderPresent(SDL_Renderer*) noexcept { return true; }

inline void SDL_PresentSoftwareFramebuffer(SDL_Renderer* r, const Uint32* argbPixels, int fbW, int fbH) noexcept {
#if defined(_WIN32)
    if (!r || !r->win || !r->win->nativeHwnd || !argbPixels || fbW <= 0 || fbH <= 0) return;
    HWND hwnd = static_cast<HWND>(r->win->nativeHwnd);
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int dstW = (clientRect.right > clientRect.left) ? (clientRect.right - clientRect.left) : r->win->w;
    const int dstH = (clientRect.bottom > clientRect.top) ? (clientRect.bottom - clientRect.top) : r->win->h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = fbW;
    bmi.bmiHeader.biHeight = -fbH; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, 0, 0, dstW, dstH, 0, 0, fbW, fbH, argbPixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, hdc);
#else
    (void)r;
    (void)argbPixels;
    (void)fbW;
    (void)fbH;
#endif
}

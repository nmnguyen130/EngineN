#pragma once
#include "SDL_common.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

inline char* ShimClipboardBuffer() noexcept {
    static char s_buf[1024]{};
    return s_buf;
}

inline bool SDL_HasClipboardText() noexcept {
    return ShimClipboardBuffer()[0] != '\0';
}

inline char* SDL_GetClipboardText() noexcept {
#if defined(_WIN32)
    if (OpenClipboard(nullptr)) {
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (hData) {
            const char* pszText = static_cast<const char*>(GlobalLock(hData));
            if (pszText) {
                const std::size_t n = std::min<std::size_t>(std::strlen(pszText), 1023U);
                std::memcpy(ShimClipboardBuffer(), pszText, n);
                ShimClipboardBuffer()[n] = '\0';
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }
#endif
    const char* s = ShimClipboardBuffer();
    const std::size_t len = std::strlen(s);
    char* p = static_cast<char*>(std::malloc(len + 1U));
    if (p) std::memcpy(p, s, len + 1U);
    return p;
}

inline bool SDL_SetClipboardText(const char* text) noexcept {
    const char* src = text ? text : "";
    const std::size_t len = std::min<std::size_t>(std::strlen(src), 1023U);
    std::memcpy(ShimClipboardBuffer(), src, len);
    ShimClipboardBuffer()[len] = '\0';
#if defined(_WIN32)
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, len + 1U);
        if (hGlob) {
            void* locked = GlobalLock(hGlob);
            if (locked) {
                std::memcpy(locked, ShimClipboardBuffer(), len + 1U);
                GlobalUnlock(hGlob);
                SetClipboardData(CF_TEXT, hGlob);
            } else {
                GlobalFree(hGlob);
            }
        }
        CloseClipboard();
    }
#endif
    return true;
}

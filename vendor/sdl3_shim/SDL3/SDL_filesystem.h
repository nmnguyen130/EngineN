#pragma once
#include "SDL_common.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

inline const char* SDL_GetBasePath() noexcept {
#if defined(_WIN32)
    static char s_base[512]{};
    if (s_base[0] == '\0') {
        DWORD len = GetModuleFileNameA(nullptr, s_base, static_cast<DWORD>(sizeof(s_base) - 1));
        if (len > 0 && len < sizeof(s_base)) {
            for (DWORD i = 0; i < len; ++i) {
                if (s_base[i] == '\\') s_base[i] = '/';
            }
            while (len > 0 && s_base[len - 1] != '/') {
                s_base[--len] = '\0';
            }
        }
        if (s_base[0] == '\0') {
            std::memcpy(s_base, "./", 3);
        }
    }
    return s_base;
#else
    return "./";
#endif
}

inline char* SDL_GetPrefPath(const char* org, const char* app) noexcept {
    (void)org;
    (void)app;
#if defined(_WIN32)
    char tempDir[512]{};
    DWORD len = GetTempPathA(static_cast<DWORD>(sizeof(tempDir) - 1), tempDir);
    if (len == 0 || len >= sizeof(tempDir)) {
        char* fallback = static_cast<char*>(std::malloc(3));
        if (fallback) std::memcpy(fallback, "./", 3);
        return fallback;
    }
    for (DWORD i = 0; i < len; ++i) {
        if (tempDir[i] == '\\') tempDir[i] = '/';
    }
    char* out = static_cast<char*>(std::malloc(len + 2));
    if (out) {
        std::memcpy(out, tempDir, len);
        if (out[len - 1] != '/') {
            out[len++] = '/';
        }
        out[len] = '\0';
    }
    return out;
#else
    char* p = static_cast<char*>(std::malloc(6));
    if (p) std::memcpy(p, "/tmp/", 6);
    return p;
#endif
}

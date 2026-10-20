#pragma once
#include "SDL_common.h"
inline const char* SDL_GetBasePath() noexcept { return "/tmp/"; }
inline char* SDL_GetPrefPath(const char*, const char*) noexcept {
    char* p = static_cast<char*>(std::malloc(6));
    std::memcpy(p, "/tmp/", 6);
    return p;
}

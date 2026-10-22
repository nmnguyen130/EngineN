#pragma once
#include "SDL_common.h"
inline constexpr const char* SDL_HINT_VIDEO_DRIVER = "SDL_VIDEO_DRIVER";
inline bool& SDL_IsDummyVideoDriver() noexcept {
    static bool s_dummy = false;
    return s_dummy;
}
inline bool SDL_SetHint(const char* name, const char* value) noexcept {
    if (name && value && std::strcmp(name, SDL_HINT_VIDEO_DRIVER) == 0) {
        SDL_IsDummyVideoDriver() = (std::strcmp(value, "dummy") == 0);
    }
    return true;
}

#pragma once
#include "SDL_common.h"
#include <string>
inline std::string& MockClipboardStorage() noexcept {
    static std::string s;
    return s;
}
inline bool SDL_HasClipboardText() noexcept { return !MockClipboardStorage().empty(); }
inline char* SDL_GetClipboardText() noexcept {
    const auto& s = MockClipboardStorage();
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
inline bool SDL_SetClipboardText(const char* text) noexcept {
    MockClipboardStorage() = text ? text : "";
    return true;
}

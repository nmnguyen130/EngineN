#pragma once
#include "SDL_common.h"
struct SDL_Gamepad { int slot{0}; };
inline SDL_Gamepad* SDL_OpenGamepad(Uint32) noexcept {
    static SDL_Gamepad pad{};
    return &pad;
}
inline void SDL_CloseGamepad(SDL_Gamepad*) noexcept {}
inline bool SDL_SetGamepadPlayerIndex(SDL_Gamepad*, int) noexcept { return true; }
inline bool SDL_RumbleGamepad(SDL_Gamepad*, Uint16, Uint16, Uint32) noexcept { return true; }

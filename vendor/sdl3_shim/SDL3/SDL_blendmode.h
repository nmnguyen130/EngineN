#pragma once
#include "SDL_common.h"

using SDL_BlendMode = Uint32;
inline constexpr SDL_BlendMode SDL_BLENDMODE_NONE = 0x00000000u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_BLEND = 0x00000001u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_ADD = 0x00000002u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_MOD = 0x00000004u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_MUL = 0x00000008u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_BLEND_PREMULTIPLIED = 0x00000010u;
inline constexpr SDL_BlendMode SDL_BLENDMODE_ADD_PREMULTIPLIED = 0x00000020u;

enum SDL_BlendOperation { SDL_BLENDOPERATION_ADD = 0x1 };
enum SDL_BlendFactor {
    SDL_BLENDFACTOR_ZERO = 0x1,
    SDL_BLENDFACTOR_ONE = 0x2,
    SDL_BLENDFACTOR_DST_COLOR = 0x9,
    SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA = 0x6
};
inline SDL_BlendMode SDL_ComposeCustomBlendMode(
    SDL_BlendFactor, SDL_BlendFactor, SDL_BlendOperation,
    SDL_BlendFactor, SDL_BlendFactor, SDL_BlendOperation) noexcept {
    return 0x00000040u;
}

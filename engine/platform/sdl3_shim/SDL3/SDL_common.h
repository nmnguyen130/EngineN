#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

using Uint8 = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Uint64 = std::uint64_t;
using Sint16 = std::int16_t;
using Sint32 = std::int32_t;
using Sint64 = std::int64_t;

inline void SDL_free(void* p) noexcept { std::free(p); }

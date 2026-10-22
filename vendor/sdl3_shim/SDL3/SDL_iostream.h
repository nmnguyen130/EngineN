#pragma once
#include "SDL_common.h"
#include <cstdio>

struct SDL_IOStream { std::FILE* fp{nullptr}; };
enum SDL_IOWhence { SDL_IO_SEEK_SET = 0 };

inline SDL_IOStream* SDL_IOFromFile(const char* path, const char* mode) noexcept {
    std::FILE* fp = std::fopen(path, mode);
    if (!fp) return nullptr;
    return new SDL_IOStream{fp};
}
inline void SDL_CloseIO(SDL_IOStream* io) noexcept {
    if (io) {
        if (io->fp) std::fclose(io->fp);
        delete io;
    }
}
inline Sint64 SDL_SeekIO(SDL_IOStream* io, Sint64 offset, SDL_IOWhence) noexcept {
    if (!io || !io->fp) return -1;
    return std::fseek(io->fp, static_cast<long>(offset), SEEK_SET) == 0 ? offset : -1;
}
inline std::size_t SDL_ReadIO(SDL_IOStream* io, void* dst, std::size_t size) noexcept {
    if (!io || !io->fp) return 0;
    return std::fread(dst, 1, size, io->fp);
}

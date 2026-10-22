#pragma once
#include "SDL_common.h"
#include <array>
#include <cstdio>

struct SDL_AsyncIO { std::FILE* fp{nullptr}; };
enum SDL_AsyncIOResult { SDL_ASYNCIO_COMPLETE = 0, SDL_ASYNCIO_FAILURE = 1 };
struct SDL_AsyncIOOutcome {
    SDL_AsyncIO* asyncio{nullptr};
    SDL_AsyncIOResult result{SDL_ASYNCIO_COMPLETE};
    void* buffer{nullptr};
    Uint64 offset{0};
    Uint64 bytes_requested{0};
    Uint64 bytes_transferred{0};
    void* userdata{nullptr};
};

// Zero-allocation fixed-capacity ring buffer replacing std::vector in SDL_AsyncIOQueue.
struct SDL_AsyncIOQueue {
    static constexpr std::size_t kCapacity = 128;
    std::array<SDL_AsyncIOOutcome, kCapacity> ring{};
    std::size_t head{0};
    std::size_t tail{0};
};

inline SDL_AsyncIOQueue* SDL_CreateAsyncIOQueue() noexcept { return new SDL_AsyncIOQueue{}; }
inline void SDL_DestroyAsyncIOQueue(SDL_AsyncIOQueue* q) noexcept { delete q; }
inline SDL_AsyncIO* SDL_AsyncIOFromFile(const char* path, const char*) noexcept {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return nullptr;
    return new SDL_AsyncIO{fp};
}
inline bool SDL_CloseAsyncIO(SDL_AsyncIO* aio, bool, SDL_AsyncIOQueue*, void*) noexcept {
    if (aio) {
        if (aio->fp) std::fclose(aio->fp);
        delete aio;
    }
    return true;
}
inline bool SDL_ReadAsyncIO(SDL_AsyncIO* aio, void* ptr, Uint64 offset, Uint64 size, SDL_AsyncIOQueue* queue, void* userdata) noexcept {
    if (!aio || !aio->fp || !queue) return false;
    if ((queue->tail - queue->head) >= SDL_AsyncIOQueue::kCapacity) return false;
    std::fseek(aio->fp, static_cast<long>(offset), SEEK_SET);
    const std::size_t n = std::fread(ptr, 1, static_cast<std::size_t>(size), aio->fp);
    queue->ring[queue->tail & (SDL_AsyncIOQueue::kCapacity - 1U)] = SDL_AsyncIOOutcome{
        aio,
        (n == size) ? SDL_ASYNCIO_COMPLETE : SDL_ASYNCIO_FAILURE,
        ptr,
        offset,
        size,
        static_cast<Uint64>(n),
        userdata,
    };
    ++queue->tail;
    return true;
}
inline bool SDL_GetAsyncIOResult(SDL_AsyncIOQueue* queue, SDL_AsyncIOOutcome* outcome) noexcept {
    if (!queue || queue->head == queue->tail) return false;
    if (outcome) *outcome = queue->ring[queue->head & (SDL_AsyncIOQueue::kCapacity - 1U)];
    ++queue->head;
    return true;
}
inline bool SDL_WaitAsyncIOResult(SDL_AsyncIOQueue* queue, SDL_AsyncIOOutcome* outcome, Sint32) noexcept {
    return SDL_GetAsyncIOResult(queue, outcome);
}

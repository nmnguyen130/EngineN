#pragma once
#include "SDL_common.h"
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

inline Uint64 SDL_GetTicksNS() noexcept {
#if defined(_WIN32)
    static LARGE_INTEGER s_freq{};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const Uint64 ticks = static_cast<Uint64>(counter.QuadPart);
    const Uint64 freq = static_cast<Uint64>(s_freq.QuadPart);
    return (ticks / freq) * 1'000'000'000ULL + ((ticks % freq) * 1'000'000'000ULL) / freq;
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

inline void SDL_DelayNS(Uint64 ns) noexcept {
    if (ns == 0ULL) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
}

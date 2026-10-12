#pragma once

#include "core/Types.h"
#include <SDL3/SDL_assert.h>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

namespace EngineN::Detail {

/**
 * Formats, logs, triggers IDE breakpoint, and aborts upon invariant violation.
 */
template <typename... Args>
inline void HandleAssert(
    bool pass,
    std::string_view condStr,
    std::source_location loc,
    std::format_string<Args...> fmt = "Assertion failed",
    Args&&... args) noexcept {
    if (pass) [[likely]] {
        return;
    }

    char buffer[1024];
    auto res = std::format_to_n(buffer, sizeof(buffer) - 1, fmt, std::forward<Args>(args)...);
    if (res.size >= sizeof(buffer) - 1) {
        buffer[sizeof(buffer) - 4] = '.';
        buffer[sizeof(buffer) - 3] = '.';
        buffer[sizeof(buffer) - 2] = '.';
    }
    *res.out = '\0';
    const std::string_view message(buffer, static_cast<usize>(res.out - buffer));

    std::fprintf(stderr,
                 "\n[FATAL ASSERTION]\n  Location:  %s:%u (%s)\n  Condition: %.*s\n  Message:   %.*s\n\n",
                 loc.file_name(), loc.line(), loc.function_name(),
                 static_cast<int>(condStr.size()), condStr.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);

    SDL_TriggerBreakpoint();
    std::abort(); // Enforces immediate termination to prevent corrupted execution
}

template <typename... Args>
[[noreturn]] inline void HandlePanic(
    std::source_location loc,
    std::format_string<Args...> fmt,
    Args&&... args) noexcept {
    HandleAssert(false, "ENGINE_PANIC", loc, fmt, std::forward<Args>(args)...);
    std::abort();
}

} // namespace EngineN::Detail

// Enabled in Debug mode or when explicitly forced via ENGINE_ENABLE_ASSERTS
#if !defined(NDEBUG) || defined(ENGINE_ENABLE_ASSERTS)
    #define ENGINE_ASSERT(cond, ...) \
        ::EngineN::Detail::HandleAssert(static_cast<bool>(cond), #cond, std::source_location::current() __VA_OPT__(, __VA_ARGS__))
    #define ENGINE_VERIFY(cond, ...) \
        ::EngineN::Detail::HandleAssert(static_cast<bool>(cond), #cond, std::source_location::current() __VA_OPT__(, __VA_ARGS__))
#else
    #define ENGINE_ASSERT(cond, ...) ((void)0)
    #define ENGINE_VERIFY(cond, ...) ((void)(cond))
#endif

// Unconditional fatal panic
#define ENGINE_PANIC(...) \
    ::EngineN::Detail::HandlePanic(std::source_location::current(), __VA_ARGS__)

// Exhaustive switch / unreachable code guard
#define ENGINE_UNREACHABLE() \
    ::EngineN::Detail::HandlePanic(std::source_location::current(), "Unreachable code path")

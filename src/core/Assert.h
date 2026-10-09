#pragma once

#include "core/Types.h"
#include <SDL3/SDL_assert.h>
#include <cstdio>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

namespace EngineN::Detail {

inline void ReportFailure(
    std::string_view condition,
    std::string_view message,
    std::source_location loc) noexcept {
    std::fprintf(stderr,
                 "\n[FATAL ASSERTION]\n  Location:  %s:%u (%s)\n  Condition: %.*s\n  Message:   %.*s\n\n",
                 loc.file_name(), loc.line(), loc.function_name(),
                 static_cast<int>(condition.size()), condition.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

template <typename... Args>
inline void FormatAndReport(
    std::string_view condition,
    std::source_location loc,
    std::format_string<Args...> fmt,
    Args&&... args) noexcept {
    char buffer[1024];
    auto res = std::format_to_n(buffer, sizeof(buffer) - 1, fmt, std::forward<Args>(args)...);
    *res.out = '\0';
    ReportFailure(condition, std::string_view(buffer, res.size), loc);
    SDL_TriggerBreakpoint();
}

template <typename... Args>
inline void HandleAssert(
    bool pass,
    std::string_view condStr,
    std::source_location loc,
    std::format_string<Args...> fmt = "Assertion failed",
    Args&&... args) noexcept {
    if (!pass) {
        FormatAndReport(condStr, loc, fmt, std::forward<Args>(args)...);
    }
}

template <typename... Args>
[[noreturn]] inline void HandlePanic(
    std::source_location loc,
    std::format_string<Args...> fmt,
    Args&&... args) noexcept {
    FormatAndReport("ENGINE_PANIC", loc, fmt, std::forward<Args>(args)...);
    std::abort();
}

} // namespace EngineN::Detail

#if !defined(NDEBUG)
    #define ENGINE_ASSERT(cond, ...) \
        ::EngineN::Detail::HandleAssert(static_cast<bool>(cond), #cond, std::source_location::current() __VA_OPT__(, __VA_ARGS__))
    #define ENGINE_VERIFY(cond, ...) \
        ::EngineN::Detail::HandleAssert(static_cast<bool>(cond), #cond, std::source_location::current() __VA_OPT__(, __VA_ARGS__))
#else
    #define ENGINE_ASSERT(cond, ...) ((void)0)
    #define ENGINE_VERIFY(cond, ...) ((void)(cond))
#endif

#define ENGINE_PANIC(...) \
    ::EngineN::Detail::HandlePanic(std::source_location::current(), __VA_ARGS__)

#define ENGINE_UNREACHABLE() \
    do { \
        ENGINE_ASSERT(false, "Unreachable code path"); \
        std::abort(); \
    } while (0)

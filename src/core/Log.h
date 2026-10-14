#pragma once

#include "core/Types.h"

#include <format>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>

namespace en {

enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

struct LogRecord {
    u64 timestampNs{0};
    LogLevel level{LogLevel::Info};
    char category[16]{};
    char message[256]{};
};

// Centralized thread-safe diagnostic logger and assertion handler.
// Owns internal file handle and mutex; safe for concurrent calls across engine threads.
// Invariant: history ring buffer holds at most 32 null-terminated records.
class Logger {
public:
    static void Init(std::string_view filePath = "engine.log") noexcept;
    static void Shutdown() noexcept;

    static void SetLevel(LogLevel minLevel) noexcept;
    [[nodiscard]] static LogLevel GetLevel() noexcept;

    static void Write(
        LogLevel level,
        std::string_view category,
        std::string_view message,
        std::source_location loc) noexcept;

    [[noreturn]] static void Panic(
        std::string_view cond,
        std::string_view message,
        std::source_location loc) noexcept;

    [[nodiscard]] static usize GetRecentLogs(std::span<LogRecord> outBuffer) noexcept;

    template <typename... Args>
    static void Dispatch(
        LogLevel level,
        std::string_view category,
        std::source_location loc,
        std::format_string<Args...> fmt,
        Args&&... args) noexcept {
        if (level < GetLevel()) {
            return;
        }
        char buf[1024];
        const auto res = std::format_to_n(buf, sizeof(buf) - 1, fmt, std::forward<Args>(args)...);
        const usize len = (res. out - buf);
        buf[len] = '\0';
        Write(level, category, {buf, len}, loc);
    }

    template <typename... Args>
    [[noreturn]] static void DispatchPanic(
        std::string_view cond,
        std::source_location loc,
        std::format_string<Args...> fmt = "Assertion failed",
        Args&&... args) noexcept {
        char buf[1024];
        const auto res = std::format_to_n(buf, sizeof(buf) - 1, fmt, std::forward<Args>(args)...);
        const usize len = (res.out - buf);
        buf[len] = '\0';
        Panic(cond, {buf, len}, loc);
    }
};

} // namespace en

#define ENGINE_LOG(level, cat, ...) \
    ::en::Logger::Dispatch(::en::LogLevel::level, #cat, std::source_location::current(), __VA_ARGS__)

#define LOG_TRACE(...) ENGINE_LOG(Trace, Core, __VA_ARGS__)
#define LOG_DEBUG(...) ENGINE_LOG(Debug, Core, __VA_ARGS__)
#define LOG_INFO(...)  ENGINE_LOG(Info,  Core, __VA_ARGS__)
#define LOG_WARN(...)  ENGINE_LOG(Warn,  Core, __VA_ARGS__)
#define LOG_ERROR(...) ENGINE_LOG(Error, Core, __VA_ARGS__)
#define LOG_FATAL(...) ENGINE_LOG(Fatal, Core, __VA_ARGS__)

#if !defined(NDEBUG) || defined(ENGINE_ENABLE_ASSERTS)
    #define ENGINE_ASSERT(cond, ...)                                                                       \
        do {                                                                                               \
            if (!(cond)) [[unlikely]] {                                                                    \
                ::en::Logger::DispatchPanic(#cond, std::source_location::current() __VA_OPT__(, __VA_ARGS__)); \
            }                                                                                              \
        } while (0)
    #define ENGINE_VERIFY(cond, ...) ENGINE_ASSERT(cond __VA_OPT__(, __VA_ARGS__))
#else
    #define ENGINE_ASSERT(cond, ...) ((void)0)
    #define ENGINE_VERIFY(cond, ...) ((void)(cond))
#endif

#define ENGINE_PANIC(...) \
    ::en::Logger::DispatchPanic("PANIC", std::source_location::current(), __VA_ARGS__)

#define ENGINE_UNREACHABLE() \
    ::en::Logger::Panic("UNREACHABLE", "Unreachable code path", std::source_location::current())
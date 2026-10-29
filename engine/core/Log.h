#pragma once

#include "core/Types.h"

#include <source_location>

namespace en {

enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
    Off   = 6,
};

// Allocation-free structured log entry passed to registered log sinks.
// Value type; message is truncated to fit FixedString<96>.
struct LogEntry {
    StringId        category{};
    const char*     file{nullptr};
    u32             line{0};
    LogLevel        level{LogLevel::Info};
    FixedString<96> message{};
};
static_assert(sizeof(LogEntry) <= 128);

using LogSinkFn = void (*)(const LogEntry& entry, void* userCtx) noexcept;

// Process-wide thread-safe logger with atomic minimum level filtering.
class Log {
public:
    static void SetLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel Level() noexcept;

    static void SetSink(LogSinkFn sinkFn, void* userCtx = nullptr) noexcept;
    static void StderrSink(const LogEntry& entry, void* userCtx) noexcept;

    static void Write(
        LogLevel level,
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept;

    static void Info(
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept {
        Write(LogLevel::Info, category, message, loc);
    }

    static void Warn(
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept {
        Write(LogLevel::Warn, category, message, loc);
    }

    static void Error(
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept {
        Write(LogLevel::Error, category, message, loc);
    }
};

using AssertHandlerFn = bool (*)(
    const char* expr,
    std::string_view message,
    std::source_location loc,
    void* userCtx) noexcept;

void SetAssertHandler(AssertHandlerFn handler, void* userCtx = nullptr) noexcept;
void SetAbortOnAssert(bool abortOnFail) noexcept;

bool HandleAssertFailure(
    const char* expr,
    std::string_view message = {},
    std::source_location loc = std::source_location::current()) noexcept;

#define EN_VERIFY(expr, ...)                                                                        \
    do {                                                                                            \
        if (!(expr)) [[unlikely]] {                                                                 \
            (void)::en::HandleAssertFailure(#expr __VA_OPT__(, ) __VA_ARGS__);                      \
        }                                                                                           \
    } while (false)

#if defined(NDEBUG)
#define EN_ASSERT(expr, ...) ((void)0)
#else
#define EN_ASSERT(expr, ...) EN_VERIFY(expr __VA_OPT__(, ) __VA_ARGS__)
#endif

} // namespace en

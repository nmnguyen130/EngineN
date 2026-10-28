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

// Minimal structured log entry. FixedString keeps message allocation-free.
struct alignas(8) LogRecord {
    StringId      category{};
    const char*   file{nullptr};
    u32           line{0};
    LogLevel      level{LogLevel::Info};
    FixedString<96> message{};
};
static_assert(sizeof(LogRecord) <= 128);

using LogSinkFn = void (*)(const LogRecord& record, void* userCtx) noexcept;

// Process-wide logger. Messages below MinLevel are discarded before any work.
class Logger {
public:
    static void SetMinLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel MinLevel() noexcept;

    static void SetSink(LogSinkFn sinkFn, void* userCtx = nullptr) noexcept;
    static void DefaultStderrSink(const LogRecord& record, void* userCtx) noexcept;

    static void Log(
        LogLevel level,
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept;
};

// Assert support. EN_VERIFY always runs; EN_ASSERT is stripped under NDEBUG.
using AssertHandlerFn = bool (*)(
    const char* expression,
    std::string_view message,
    std::source_location loc,
    void* userCtx) noexcept;

void SetAssertHandler(AssertHandlerFn handler, void* userCtx = nullptr) noexcept;
void SetAbortOnUnhandledAssert(bool abortOnFail) noexcept;

bool ReportAssertionFailure(
    const char* expression,
    std::string_view message = {},
    std::source_location loc = std::source_location::current()) noexcept;

#define EN_VERIFY(expr, ...)                                                                        \
    do {                                                                                            \
        if (!(expr)) [[unlikely]] {                                                                 \
            (void)::en::ReportAssertionFailure(#expr __VA_OPT__(, ) __VA_ARGS__);                   \
        }                                                                                           \
    } while (false)

#if defined(NDEBUG)
#define EN_ASSERT(expr, ...) ((void)0)
#else
#define EN_ASSERT(expr, ...) EN_VERIFY(expr __VA_OPT__(, ) __VA_ARGS__)
#endif

} // namespace en

#pragma once

#include "core/Types.h"

#include <source_location>

namespace en {

enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
    Off = 6,
};

// Cache-line-aligned 128-byte structured log entry with zero internal padding.
// Stores static string literals from std::source_location by pointer and message text inline.
struct alignas(64) LogRecord {
    u64 sequence{0};
    StringId category{};
    const char* file{nullptr};
    const char* function{nullptr};
    u32 line{0};
    LogLevel level{LogLevel::Info};
    u8 reserved{0};
    FixedString<90> message{};
};
static_assert(sizeof(LogRecord) == 128);

using LogSinkFn = void (*)(const LogRecord& record, void* userCtx) noexcept;

// Thread-safe, zero-heap-allocation structured logger with an in-memory ring history for crash dumps.
// Static process-wide service; retains the most recent kHistoryCapacity records in a circular buffer.
// Invariant: messages below MinLevel() are filtered out in O(1) before formatting or locking.
class Logger {
public:
    static constexpr usize kHistoryCapacity = 64;

    static void SetMinLevel(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel MinLevel() noexcept;

    static void SetSink(LogSinkFn sinkFn, void* userCtx = nullptr) noexcept;
    static void Clear() noexcept;

    static void Log(
        LogLevel level,
        StringId category,
        std::string_view message,
        std::source_location loc = std::source_location::current()) noexcept;

    [[nodiscard]] static u32 RecentRecords(std::span<LogRecord> outRecords) noexcept;
    [[nodiscard]] static u64 TotalLoggedCount() noexcept;
};

using AssertHandlerFn = bool (*)(
    const char* expression,
    std::string_view message,
    std::source_location loc,
    void* userCtx) noexcept;

void SetAssertHandler(AssertHandlerFn handler, void* userCtx = nullptr) noexcept;

bool ReportAssertionFailure(
    const char* expression,
    std::string_view message = {},
    std::source_location loc = std::source_location::current()) noexcept;

// EN_VERIFY always evaluates expr in all builds; EN_ASSERT is stripped when NDEBUG is defined.
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

struct alignas(8) TraceFrameEntry {
    const char* functionName{nullptr};
    const char* fileName{nullptr};
    u32 lineNumber{0};
    u32 depth{0};
};
static_assert(sizeof(TraceFrameEntry) == 24);

// Captures thread-local call-stack breadcrumbs and writes diagnostic crash dumps on fatal standard signals.
// Static process-wide utility with zero dynamic allocation and zero OS-header pollution.
// Invariant: PushFrame and PopFrame remain balanced even when call depth exceeds kMaxTraceDepth.
class CrashHandler {
public:
    static constexpr usize kMaxTraceDepth = 32;

    static void Install(const char* dumpFilePath = "enginen_crash.dmp.txt") noexcept;
    static void PushFrame(std::source_location loc = std::source_location::current()) noexcept;
    static void PopFrame() noexcept;
    static void ResetThreadTrace() noexcept;

    [[nodiscard]] static u32 CaptureStackTrace(std::span<TraceFrameEntry> outFrames) noexcept;
    [[nodiscard]] static usize FormatCrashReport(const char* reason, std::span<char> outText) noexcept;
    static bool WriteCrashDump(const char* reason) noexcept;
    [[nodiscard]] static u32 CurrentDepth() noexcept;
};

// RAII call-stack breadcrumb guard balanced with CrashHandler::PushFrame / PopFrame.
class ScopedTraceFrame {
public:
    explicit ScopedTraceFrame(std::source_location loc = std::source_location::current()) noexcept {
        CrashHandler::PushFrame(loc);
    }
    ~ScopedTraceFrame() noexcept { CrashHandler::PopFrame(); }

    ScopedTraceFrame(const ScopedTraceFrame&) = delete;
    ScopedTraceFrame& operator=(const ScopedTraceFrame&) = delete;
};

#define EN_TRACE_SCOPE() ::en::ScopedTraceFrame _enTraceGuard_##__LINE__(::std::source_location::current())

} // namespace en

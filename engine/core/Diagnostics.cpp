#include "core/Diagnostics.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace en {

namespace {

[[nodiscard]] constexpr std::string_view Basename(const char* path) noexcept {
    if (path == nullptr) return "<file>";
    const std::string_view sv{path};
    const usize slash = sv.find_last_of("/\\");
    return (slash == std::string_view::npos) ? sv : sv.substr(slash + 1U);
}

[[nodiscard]] constexpr std::string_view LevelName(LogLevel level) noexcept {
    constexpr std::string_view kNames[] = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    const auto idx = static_cast<usize>(level);
    return (idx < std::size(kNames)) ? kNames[idx] : "INFO";
}

[[nodiscard]] constexpr bool PassesMinLevel(LogLevel level, LogLevel minimum) noexcept {
    return static_cast<u8>(level) >= static_cast<u8>(minimum) && level != LogLevel::Off;
}

std::mutex                g_logMutex{};
std::atomic<LogLevel>     g_minLogLevel{LogLevel::Info};
LogSinkFn                 g_logSink{&Logger::DefaultStderrSink};
void*                     g_logSinkCtx{nullptr};

std::atomic<AssertHandlerFn> g_assertHandler{nullptr};
std::atomic<void*>           g_assertUserCtx{nullptr};
#if defined(NDEBUG)
std::atomic<bool> g_abortOnUnhandledAssert{false};
#else
std::atomic<bool> g_abortOnUnhandledAssert{true};
#endif

} // namespace

void Logger::DefaultStderrSink(const LogRecord& record, void*) noexcept {
    if (static_cast<u8>(record.level) < static_cast<u8>(LogLevel::Warn)) return;

    char line[256]{};
    const auto level = LevelName(record.level);
    const auto file  = Basename(record.file);
    const auto msg   = record.message.View();

    // Simple fixed-format line; no dynamic allocation.
    std::snprintf(line, sizeof(line), "[%.*s] %.*s:%u %.*s\n",
                  static_cast<int>(level.size()), level.data(),
                  static_cast<int>(file.size()),  file.data(),
                  record.line,
                  static_cast<int>(msg.size()),   msg.data());
    (void)std::fwrite(line, 1U, std::strlen(line), stderr);
    (void)std::fflush(stderr);
}

void Logger::SetMinLevel(LogLevel level) noexcept {
    g_minLogLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::MinLevel() noexcept {
    return g_minLogLevel.load(std::memory_order_relaxed);
}

void Logger::SetSink(LogSinkFn sinkFn, void* userCtx) noexcept {
    std::lock_guard lock(g_logMutex);
    g_logSink    = sinkFn != nullptr ? sinkFn : &Logger::DefaultStderrSink;
    g_logSinkCtx = sinkFn != nullptr ? userCtx : nullptr;
}

void Logger::Log(LogLevel level, StringId category, std::string_view message,
                 std::source_location loc) noexcept {
    if (!PassesMinLevel(level, g_minLogLevel.load(std::memory_order_relaxed))) return;

    if (const usize nul = message.find('\0'); nul != std::string_view::npos) {
        message = message.substr(0, nul);
    }

    LogRecord rec{};
    rec.category = category;
    rec.file     = loc.file_name() != nullptr ? loc.file_name() : "<file>";
    rec.line     = loc.line();
    rec.level    = level;
    (void)rec.message.Assign(message.substr(0, FixedString<96>::kMaxLength));

    LogSinkFn sinkCopy = nullptr;
    void*     sinkCtx  = nullptr;
    {
        std::lock_guard lock(g_logMutex);
        sinkCopy = g_logSink;
        sinkCtx  = g_logSinkCtx;
    }
    if (sinkCopy) sinkCopy(rec, sinkCtx);
}

void SetAssertHandler(AssertHandlerFn handler, void* userCtx) noexcept {
    g_assertUserCtx.store(userCtx, std::memory_order_relaxed);
    g_assertHandler.store(handler, std::memory_order_release);
}

void SetAbortOnUnhandledAssert(bool abortOnFail) noexcept {
    g_abortOnUnhandledAssert.store(abortOnFail, std::memory_order_relaxed);
}

bool ReportAssertionFailure(const char* expression, std::string_view message,
                            std::source_location loc) noexcept {
    using namespace en::literals;
    const char* exprStr = expression != nullptr ? expression : "Assertion failed";
    Logger::Log(LogLevel::Fatal, "core/assert"_sid,
                message.empty() ? std::string_view{exprStr} : message, loc);

    if (const AssertHandlerFn handler = g_assertHandler.load(std::memory_order_acquire)) {
        return handler(expression != nullptr ? expression : "", message, loc,
                       g_assertUserCtx.load(std::memory_order_relaxed));
    }

    if (g_abortOnUnhandledAssert.load(std::memory_order_relaxed)) {
        std::abort();
    }
    return false;
}

} // namespace en

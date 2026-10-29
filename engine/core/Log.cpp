#include "core/Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace en {
namespace {

[[nodiscard]] constexpr std::string_view Basename(const char* path) noexcept {
    if (!path) return "<file>";
    const std::string_view sv{path};
    const usize slash = sv.find_last_of("/\\");
    return (slash == std::string_view::npos) ? sv : sv.substr(slash + 1U);
}

[[nodiscard]] constexpr std::string_view LevelName(LogLevel level) noexcept {
    constexpr std::string_view kNames[]{"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    const auto idx = static_cast<usize>(level);
    return (idx < std::size(kNames)) ? kNames[idx] : "INFO";
}

[[nodiscard]] constexpr bool PassesLevel(LogLevel level, LogLevel minLevel) noexcept {
    return static_cast<u8>(level) >= static_cast<u8>(minLevel) && level != LogLevel::Off;
}

std::mutex                   g_logMutex{};
std::atomic<LogLevel>        g_minLevel{LogLevel::Info};
LogSinkFn                    g_sink{&Log::StderrSink};
void*                        g_sinkCtx{nullptr};
std::atomic<AssertHandlerFn> g_assertHandler{nullptr};
std::atomic<void*>           g_assertCtx{nullptr};
#if defined(NDEBUG)
std::atomic<bool>            g_abortOnAssert{false};
#else
std::atomic<bool>            g_abortOnAssert{true};
#endif

} // namespace

void Log::StderrSink(const LogEntry& entry, void*) noexcept {
    if (static_cast<u8>(entry.level) < static_cast<u8>(LogLevel::Warn)) return;

    char line[256]{};
    const auto lvl  = LevelName(entry.level);
    const auto file = Basename(entry.file);
    const auto msg  = entry.message.View();

    std::snprintf(
        line, sizeof(line), "[%.*s] %.*s:%u %.*s\n",
        static_cast<int>(lvl.size()), lvl.data(),
        static_cast<int>(file.size()), file.data(),
        entry.line,
        static_cast<int>(msg.size()), msg.data());
    (void)std::fwrite(line, 1U, std::strlen(line), stderr);
    (void)std::fflush(stderr);
}

void Log::SetLevel(LogLevel level) noexcept {
    g_minLevel.store(level, std::memory_order_relaxed);
}

LogLevel Log::Level() noexcept {
    return g_minLevel.load(std::memory_order_relaxed);
}

void Log::SetSink(LogSinkFn sinkFn, void* userCtx) noexcept {
    std::lock_guard lock(g_logMutex);
    g_sink    = sinkFn ? sinkFn : &Log::StderrSink;
    g_sinkCtx = sinkFn ? userCtx : nullptr;
}

void Log::Write(LogLevel level, StringId category, std::string_view message,
                std::source_location loc) noexcept {
    if (!PassesLevel(level, g_minLevel.load(std::memory_order_relaxed))) return;

    if (const usize nul = message.find('\0'); nul != std::string_view::npos) {
        message = message.substr(0U, nul);
    }

    LogEntry entry{
        .category = category,
        .file     = loc.file_name() ? loc.file_name() : "<file>",
        .line     = loc.line(),
        .level    = level,
    };
    (void)entry.message.Assign(message.substr(0U, FixedString<96>::kMaxLength));

    LogSinkFn sinkCopy = nullptr;
    void*     sinkCtx  = nullptr;
    {
        std::lock_guard lock(g_logMutex);
        sinkCopy = g_sink;
        sinkCtx  = g_sinkCtx;
    }
    if (sinkCopy) sinkCopy(entry, sinkCtx);
}

void SetAssertHandler(AssertHandlerFn handler, void* userCtx) noexcept {
    g_assertCtx.store(userCtx, std::memory_order_relaxed);
    g_assertHandler.store(handler, std::memory_order_release);
}

void SetAbortOnAssert(bool abortOnFail) noexcept {
    g_abortOnAssert.store(abortOnFail, std::memory_order_relaxed);
}

bool HandleAssertFailure(const char* expr, std::string_view message,
                         std::source_location loc) noexcept {
    using namespace en::literals;
    const char* exprStr = expr ? expr : "Assertion failed";
    Log::Write(LogLevel::Fatal, "core/assert"_sid,
               message.empty() ? std::string_view{exprStr} : message, loc);

    if (const AssertHandlerFn handler = g_assertHandler.load(std::memory_order_acquire)) {
        return handler(expr ? expr : "", message, loc, g_assertCtx.load(std::memory_order_relaxed));
    }
    if (g_abortOnAssert.load(std::memory_order_relaxed)) {
        std::abort();
    }
    return false;
}

} // namespace en

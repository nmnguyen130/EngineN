#pragma once

#include "core/Types.h"

#include <format>
#include <source_location>
#include <string_view>

namespace EngineN {

enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};


/**
 * LogRecord: Stored log entry for in-game debug HUD, console, and telemetry overlays.
 */
struct LogRecord {
    LogLevel level = LogLevel::Info;
    char category[16] = {};
    char message[256] = {};
    u64 timestampNs = 0;
};

/**
 * Logger: Centralized thread-safe diagnostic logging subsystem.
 * Supports leveled filtering, category tagging, ANSI colors, file output,
 * and a circular history ring-buffer for in-game debug consoles.
 */
class Logger {
public:
    static void Init(std::string_view filePath = "engine.log");
    static void Shutdown();
    static void SetLevel(LogLevel minLevel) noexcept;
    static LogLevel GetLevel() noexcept;

    static void LogMessage(LogLevel level,
                           std::string_view category,
                           std::string_view message,
                           std::source_location loc) noexcept;

    // Drains recent logs in chronological order (oldest to newest) for in-game overlays
    static usize GetRecentLogs(LogRecord* outBuffer, usize maxCount) noexcept;

    template <typename... Args>
    static void Dispatch(LogLevel level,
                         std::string_view category,
                         std::source_location loc,
                         std::format_string<Args...> fmt,
                         Args&&... args) noexcept {
        if (level < GetLevel()) {
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
        LogMessage(level, category, std::string_view(buffer, static_cast<usize>(res.out - buffer)), loc);
    }
};

} // namespace EngineN

// Category-explicit logging macro
#define ENGINE_LOG(level, cat, ...) \
    ::EngineN::Logger::Dispatch(::EngineN::LogLevel::level, #cat, std::source_location::current(), __VA_ARGS__)


// Default [Core] category convenience macros
#define LOG_TRACE(...) ENGINE_LOG(Trace, Core, __VA_ARGS__)
#define LOG_DEBUG(...) ENGINE_LOG(Debug, Core, __VA_ARGS__)
#define LOG_INFO(...)  ENGINE_LOG(Info,  Core, __VA_ARGS__)
#define LOG_WARN(...)  ENGINE_LOG(Warn,  Core, __VA_ARGS__)
#define LOG_ERROR(...) ENGINE_LOG(Error, Core, __VA_ARGS__)
#define LOG_FATAL(...) ENGINE_LOG(Fatal, Core, __VA_ARGS__)

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

enum class LogCategory : u8 {
    Core = 0,
    Platform,
    Physics,
    Render,
    Audio,
    Gameplay
};

struct LogRecord {
    LogLevel level = LogLevel::Info;
    LogCategory category = LogCategory::Core;
    char message[192] = {};
    u64 timestampNs = 0;
};

class Logger {
public:
    static void Init(std::string_view logFilePath = "engine.log");
    static void Shutdown();
    static void SetLevel(LogLevel minLevel) noexcept;
    static LogLevel GetLevel() noexcept;

    static void LogMessage(LogLevel level,
                           LogCategory category,
                           std::string_view formattedMessage,
                           std::source_location loc) noexcept;

    static usize GetRecentLogs(LogRecord* outBuffer, usize maxCount) noexcept;

    template <typename... Args>
    static void Dispatch(LogLevel level,
                         LogCategory category,
                         std::source_location loc,
                         std::format_string<Args...> fmt,
                         Args&&... args) noexcept {
        if (level < GetLevel()) {
            return;
        }

        char buffer[1024];
        auto res = std::format_to_n(buffer, sizeof(buffer) - 1, fmt, std::forward<Args>(args)...);
        *res.out = '\0';
        LogMessage(level, category, std::string_view(buffer, res.size), loc);
    }
};

} // namespace EngineN

// Category-explicit macros
#define ENGINE_LOG(level, cat, ...) \
    ::EngineN::Logger::Dispatch(::EngineN::LogLevel::level, ::EngineN::LogCategory::cat, std::source_location::current(), __VA_ARGS__)

// Default [Core] category convenience macros
#define LOG_TRACE(...) ENGINE_LOG(Trace, Core, __VA_ARGS__)
#define LOG_DEBUG(...) ENGINE_LOG(Debug, Core, __VA_ARGS__)
#define LOG_INFO(...)  ENGINE_LOG(Info,  Core, __VA_ARGS__)
#define LOG_WARN(...)  ENGINE_LOG(Warn,  Core, __VA_ARGS__)
#define LOG_ERROR(...) ENGINE_LOG(Error, Core, __VA_ARGS__)
#define LOG_FATAL(...) ENGINE_LOG(Fatal, Core, __VA_ARGS__)

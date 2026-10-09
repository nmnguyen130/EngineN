#include "core/Log.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace EngineN {

namespace {

std::atomic<LogLevel> s_minLevel{LogLevel::Trace};
std::FILE* s_fileHandle = nullptr;
std::mutex s_logMutex;

const char* LevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

const char* CategoryToString(LogCategory category) noexcept {
    switch (category) {
        case LogCategory::Core:     return "Core";
        case LogCategory::Platform: return "Platform";
        case LogCategory::Physics:  return "Physics";
        case LogCategory::Render:   return "Render";
        case LogCategory::Audio:    return "Audio";
        case LogCategory::Gameplay: return "Gameplay";
    }
    return "Unknown";
}

const char* LevelColorAnsi(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "\033[90m"; // Gray
        case LogLevel::Debug: return "\033[36m"; // Cyan
        case LogLevel::Info:  return "\033[32m"; // Green
        case LogLevel::Warn:  return "\033[33m"; // Yellow
        case LogLevel::Error: return "\033[31m"; // Red
        case LogLevel::Fatal: return "\033[35m"; // Magenta
    }
    return "\033[0m";
}

} // namespace

void Logger::Init(std::string_view logFilePath) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_fileHandle && !logFilePath.empty()) {
        std::string path(logFilePath);
        s_fileHandle = std::fopen(path.c_str(), "w");
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_fileHandle) {
        std::fflush(s_fileHandle);
        std::fclose(s_fileHandle);
        s_fileHandle = nullptr;
    }
}

void Logger::SetLevel(LogLevel minLevel) noexcept {
    s_minLevel.store(minLevel, std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() noexcept {
    return s_minLevel.load(std::memory_order_relaxed);
}

void Logger::LogMessage(LogLevel level,
                       LogCategory category,
                       std::string_view formattedMessage,
                       std::source_location loc) noexcept {
    std::lock_guard<std::mutex> lock(s_logMutex);

    const char* lvlStr = LevelToString(level);
    const char* catStr = CategoryToString(category);
    const char* colStr = LevelColorAnsi(level);
    const char* rstStr = "\033[0m";

    // Console output (with ANSI color)
    if (level >= LogLevel::Warn) {
        std::fprintf(stderr,
                     "%s[%s][%s]%s %.*s \033[90m(%s:%u)%s\n",
                     colStr, lvlStr, catStr, rstStr,
                     static_cast<int>(formattedMessage.size()), formattedMessage.data(),
                     loc.file_name(), loc.line(),
                     rstStr);
        std::fflush(stderr);
    } else {
        std::fprintf(stdout,
                     "%s[%s][%s]%s %.*s\n",
                     colStr, lvlStr, catStr, rstStr,
                     static_cast<int>(formattedMessage.size()), formattedMessage.data());
    }

    // Thread-safe file output
    if (s_fileHandle) {
        if (level >= LogLevel::Warn) {
            std::fprintf(s_fileHandle,
                         "[%s][%s] %.*s (%s:%u)\n",
                         lvlStr, catStr,
                         static_cast<int>(formattedMessage.size()), formattedMessage.data(),
                         loc.file_name(), loc.line());
        } else {
            std::fprintf(s_fileHandle,
                         "[%s][%s] %.*s\n",
                         lvlStr, catStr,
                         static_cast<int>(formattedMessage.size()), formattedMessage.data());
        }
        std::fflush(s_fileHandle);
    }
}

} // namespace EngineN

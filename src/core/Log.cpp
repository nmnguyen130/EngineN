#include "core/Log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace EngineN {

namespace {

std::atomic<LogLevel> s_minLevel{LogLevel::Trace};
std::FILE* s_fileHandle = nullptr;
std::mutex s_logMutex;

constexpr usize kHistoryCapacity = 16;
LogRecord s_logHistory[kHistoryCapacity];
usize s_historyHead = 0;
usize s_historyCount = 0;

constexpr std::string_view kLevelNames[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

constexpr std::string_view kCategoryNames[] = {
    "Core", "Platform", "Physics", "Render", "Audio", "Gameplay", "Asset"
};

constexpr std::string_view kLevelColors[] = {
    "\033[90m", // TRACE (gray)
    "\033[36m", // DEBUG (cyan)
    "\033[32m", // INFO  (green)
    "\033[33m", // WARN  (yellow)
    "\033[31m", // ERROR (red)
    "\033[35m"  // FATAL (magenta)
};

inline std::string_view LevelToString(LogLevel level) noexcept {
    const auto idx = static_cast<usize>(level);
    return (idx < std::size(kLevelNames)) ? kLevelNames[idx] : "UNKNOWN";
}

inline std::string_view CategoryToString(LogCategory category) noexcept {
    const auto idx = static_cast<usize>(category);
    return (idx < std::size(kCategoryNames)) ? kCategoryNames[idx] : "Unknown";
}

inline std::string_view LevelColorAnsi(LogLevel level) noexcept {
    const auto idx = static_cast<usize>(level);
    return (idx < std::size(kLevelColors)) ? kLevelColors[idx] : "\033[0m";
}

} // namespace

void Logger::Init(std::string_view logFilePath) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_fileHandle && !logFilePath.empty()) {
        char pathBuf[512];
        const usize len = std::min(logFilePath.size(), sizeof(pathBuf) - 1);
        std::memcpy(pathBuf, logFilePath.data(), len);
        pathBuf[len] = '\0';
        s_fileHandle = std::fopen(pathBuf, "w");
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

    const auto lvlStr = LevelToString(level);
    const auto catStr = CategoryToString(category);
    const auto colStr = LevelColorAnsi(level);
    constexpr const char* rstStr = "\033[0m";

    // Console output
    std::FILE* console = (level >= LogLevel::Warn) ? stderr : stdout;
    if (level >= LogLevel::Warn) {
        std::fprintf(console,
                     "%s[%.*s][%.*s]%s %.*s \033[90m(%s:%u)%s\n",
                     colStr.data(), static_cast<int>(lvlStr.size()), lvlStr.data(),
                     static_cast<int>(catStr.size()), catStr.data(), rstStr,
                     static_cast<int>(formattedMessage.size()), formattedMessage.data(),
                     loc.file_name(), loc.line(), rstStr);
        std::fflush(console);
    } else {
        std::fprintf(console,
                     "%s[%.*s][%.*s]%s %.*s\n",
                     colStr.data(), static_cast<int>(lvlStr.size()), lvlStr.data(),
                     static_cast<int>(catStr.size()), catStr.data(), rstStr,
                     static_cast<int>(formattedMessage.size()), formattedMessage.data());
    }

    // Thread-safe file output
    if (s_fileHandle) {
        if (level >= LogLevel::Warn) {
            std::fprintf(s_fileHandle,
                         "[%.*s][%.*s] %.*s (%s:%u)\n",
                         static_cast<int>(lvlStr.size()), lvlStr.data(),
                         static_cast<int>(catStr.size()), catStr.data(),
                         static_cast<int>(formattedMessage.size()), formattedMessage.data(),
                         loc.file_name(), loc.line());
        } else {
            std::fprintf(s_fileHandle,
                         "[%.*s][%.*s] %.*s\n",
                         static_cast<int>(lvlStr.size()), lvlStr.data(),
                         static_cast<int>(catStr.size()), catStr.data(),
                         static_cast<int>(formattedMessage.size()), formattedMessage.data());
        }
        std::fflush(s_fileHandle);
    }

    // Circular history buffer for HUD
    LogRecord& rec = s_logHistory[s_historyHead];
    rec.level = level;
    rec.category = category;
    const usize copyLen = std::min(formattedMessage.size(), sizeof(rec.message) - 1);
    std::memcpy(rec.message, formattedMessage.data(), copyLen);
    rec.message[copyLen] = '\0';
    rec.timestampNs = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    s_historyHead = (s_historyHead + 1) % kHistoryCapacity;
    if (s_historyCount < kHistoryCapacity) {
        s_historyCount++;
    }
}

usize Logger::GetRecentLogs(LogRecord* outBuffer, usize maxCount) noexcept {
    if (!outBuffer || maxCount == 0) return 0;
    std::lock_guard<std::mutex> lock(s_logMutex);

    const usize count = std::min(maxCount, s_historyCount);
    for (usize i = 0; i < count; ++i) {
        // Read oldest to newest
        const usize idx = (s_historyHead + kHistoryCapacity - count + i) % kHistoryCapacity;
        outBuffer[i] = s_logHistory[idx];
    }
    return count;
}

} // namespace EngineN

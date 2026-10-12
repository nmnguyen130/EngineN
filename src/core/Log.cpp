#include "core/Log.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace EngineN {

namespace {

std::atomic<LogLevel> s_minLevel{LogLevel::Trace};
std::FILE* s_fileHandle = nullptr;
std::mutex s_logMutex;

// In-game debug overlay ring buffer (32 recent entries)
constexpr usize kHistoryCapacity = 32;
LogRecord s_logHistory[kHistoryCapacity];
usize s_historyHead = 0;
usize s_historyCount = 0;

struct LevelMeta {
    std::string_view name;
    std::string_view color;
};

constexpr LevelMeta kLevelMetas[] = {
    { "TRACE", "\033[90m" }, // Gray
    { "DEBUG", "\033[36m" }, // Cyan
    { "INFO",  "\033[32m" }, // Green
    { "WARN",  "\033[33m" }, // Yellow
    { "ERROR", "\033[31m" }, // Red
    { "FATAL", "\033[35m" }  // Magenta
};

inline const LevelMeta& GetLevelMeta(LogLevel level) noexcept {
    const auto idx = static_cast<usize>(level);
    static constexpr LevelMeta kUnknown{ "UNKNOWN", "\033[0m" };
    return (idx < std::size(kLevelMetas)) ? kLevelMetas[idx] : kUnknown;
}

} // namespace

void Logger::Init(std::string_view filePath) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (!s_fileHandle && !filePath.empty()) {
        char pathBuf[512];
        const usize len = std::min(filePath.size(), sizeof(pathBuf) - 1);
        std::memcpy(pathBuf, filePath.data(), len);
        pathBuf[len] = '\0';
        s_fileHandle = std::fopen(pathBuf, "w");
        if (!s_fileHandle) {
            std::fprintf(stderr, "[WARN][Core] Failed to open log file: %s\n", pathBuf);
        }
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
                        std::string_view category,
                        std::string_view message,
                        std::source_location loc) noexcept {
    std::lock_guard<std::mutex> lock(s_logMutex);

    const auto& meta = GetLevelMeta(level);
    const bool isWarnOrFatal = (level >= LogLevel::Warn);

    // 1. Console Output
    std::FILE* console = isWarnOrFatal ? stderr : stdout;
    if (isWarnOrFatal) {
        std::fprintf(console,
                     "%s[%.*s][%.*s]\033[0m %.*s \033[90m(%s:%u)\033[0m\n",
                     meta.color.data(), static_cast<int>(meta.name.size()), meta.name.data(),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data(),
                     loc.file_name(), loc.line());
    } else {
        std::fprintf(console,
                     "%s[%.*s][%.*s]\033[0m %.*s\n",
                     meta.color.data(), static_cast<int>(meta.name.size()), meta.name.data(),
                     static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
    }
    std::fflush(console);

    // 2. Thread-safe file output
    if (s_fileHandle) {
        if (isWarnOrFatal) {
            std::fprintf(s_fileHandle,
                         "[%.*s][%.*s] %.*s (%s:%u)\n",
                         static_cast<int>(meta.name.size()), meta.name.data(),
                         static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data(),
                         loc.file_name(), loc.line());
        } else {
            std::fprintf(s_fileHandle,
                         "[%.*s][%.*s] %.*s\n",
                         static_cast<int>(meta.name.size()), meta.name.data(),
                         static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data());
        }
        std::fflush(s_fileHandle);
    }

    // 3. Circular history ring-buffer for in-game HUD/debug overlay
    LogRecord& record = s_logHistory[s_historyHead];
    record.level = level;
    record.timestampNs = SDL_GetTicksNS();

    const usize catLen = std::min(category.size(), sizeof(record.category) - 1);
    std::memcpy(record.category, category.data(), catLen);
    record.category[catLen] = '\0';

    const usize copyLen = std::min(message.size(), sizeof(record.message) - 1);
    std::memcpy(record.message, message.data(), copyLen);
    if (message.size() >= sizeof(record.message) - 1 && copyLen >= 3) {
        record.message[copyLen - 3] = '.';
        record.message[copyLen - 2] = '.';
        record.message[copyLen - 1] = '.';
    }
    record.message[copyLen] = '\0';

    s_historyHead = (s_historyHead + 1) % kHistoryCapacity;
    if (s_historyCount < kHistoryCapacity) {
        ++s_historyCount;
    }
}

usize Logger::GetRecentLogs(LogRecord* outBuffer, usize maxCount) noexcept {
    if (!outBuffer || maxCount == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_logMutex);

    const usize count = std::min(maxCount, s_historyCount);
    for (usize i = 0; i < count; ++i) {
        const usize idx = (s_historyHead + kHistoryCapacity - count + i) % kHistoryCapacity;
        outBuffer[i] = s_logHistory[idx];
    }
    return count;
}

} // namespace EngineN

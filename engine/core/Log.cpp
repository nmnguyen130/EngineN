#include "core/Log.h"

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace en {
namespace {

std::atomic<LogLevel> s_minLevel{LogLevel::Trace};
std::FILE* s_file = nullptr;
std::mutex s_mutex;

constexpr usize kHistoryCap = 32;
LogRecord s_history[kHistoryCap]{};
usize s_head = 0;
usize s_count = 0;

constexpr const char* kLevelNames[]  = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
constexpr const char* kLevelColors[] = {"\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[35m"};

} // namespace

void Logger::Init(std::string_view filePath) noexcept {
    std::lock_guard lock(s_mutex);
    if (!s_file && !filePath.empty()) {
        char path[512];
        std::snprintf(path, sizeof(path), "%.*s", static_cast<int>(filePath.size()), filePath.data());
        s_file = std::fopen(path, "w");
    }
}

void Logger::Shutdown() noexcept {
    std::lock_guard lock(s_mutex);
    if (s_file) {
        std::fclose(s_file);
        s_file = nullptr;
    }
}

void Logger::SetLevel(LogLevel minLevel) noexcept {
    s_minLevel.store(minLevel, std::memory_order_relaxed);
}

LogLevel Logger::GetLevel() noexcept {
    return s_minLevel.load(std::memory_order_relaxed);
}

void Logger::Write(
    LogLevel level,
    std::string_view category,
    std::string_view message,
    std::source_location loc) noexcept {
    std::lock_guard lock(s_mutex);

    const auto idx = std::min<usize>(static_cast<usize>(level), 5);
    const char* name = kLevelNames[idx];
    const char* color = kLevelColors[idx];
    const bool showLoc = (level >= LogLevel::Warn);

    std::FILE* out = showLoc ? stderr : stdout;
    if (showLoc) {
        std::fprintf(out, "%s[%s][%.*s]\033[0m %.*s \033[90m(%s:%u)\033[0m\n",
                     color, name, static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data(), loc.file_name(), loc.line());
        if (s_file) {
            std::fprintf(s_file, "[%s][%.*s] %.*s (%s:%u)\n",
                         name, static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data(), loc.file_name(), loc.line());
        }
    } else {
        std::fprintf(out, "%s[%s][%.*s]\033[0m %.*s\n",
                     color, name, static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        if (s_file) {
            std::fprintf(s_file, "[%s][%.*s] %.*s\n",
                         name, static_cast<int>(category.size()), category.data(),
                         static_cast<int>(message.size()), message.data());
        }
    }
    std::fflush(out);
    if (s_file) {
        std::fflush(s_file);
    }

    LogRecord& rec = s_history[s_head];
    rec.timestampNs = SDL_GetTicksNS();
    rec.level = level;
    std::snprintf(rec.category, sizeof(rec.category), "%.*s", static_cast<int>(category.size()), category.data());
    std::snprintf(rec.message, sizeof(rec.message), "%.*s", static_cast<int>(message.size()), message.data());

    s_head = (s_head + 1) % kHistoryCap;
    if (s_count < kHistoryCap) {
        ++s_count;
    }
}

void Logger::Panic(
    std::string_view cond,
    std::string_view message,
    std::source_location loc) noexcept {
    char fullMsg[1024];
    std::snprintf(fullMsg, sizeof(fullMsg), "Condition (%.*s) failed: %.*s",
                  static_cast<int>(cond.size()), cond.data(),
                  static_cast<int>(message.size()), message.data());

    Write(LogLevel::Fatal, "Assert", fullMsg, loc);
    SDL_TriggerBreakpoint();
    std::abort();
}

usize Logger::GetRecentLogs(std::span<LogRecord> outBuffer) noexcept {
    if (outBuffer.empty()) {
        return 0;
    }
    std::lock_guard lock(s_mutex);
    const usize count = std::min(outBuffer.size(), s_count);
    for (usize i = 0; i < count; ++i) {
        outBuffer[i] = s_history[(s_head + kHistoryCap - count + i) % kHistoryCap];
    }
    return count;
}

} // namespace en
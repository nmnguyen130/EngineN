#include "core/Diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace en {

namespace {

struct BufferWriter {
    std::span<char> buf{};
    usize pos{0};

    constexpr explicit BufferWriter(std::span<char> out) noexcept : buf(out) {
        if (!buf.empty()) {
            buf[0] = '\0';
        }
    }

    void Append(std::string_view sv) noexcept {
        if (buf.size() <= 1U || sv.empty()) {
            return;
        }
        const usize n = std::min(sv.size(), (buf.size() - 1U) - pos);
        std::memcpy(buf.data() + pos, sv.data(), n);
        pos += n;
        buf[pos] = '\0';
    }

    void AppendU64(u64 val) noexcept {
        char tmp[24]{};
        auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), val);
        if (ec == std::errc{}) {
            Append({tmp, static_cast<usize>(ptr - tmp)});
        }
    }
};

[[nodiscard]] constexpr std::string_view Basename(const char* path) noexcept {
    if (!path) {
        return "<file>";
    }
    const std::string_view sv{path};
    const usize slash = sv.find_last_of("/\\");
    return (slash == std::string_view::npos) ? sv : sv.substr(slash + 1U);
}

[[nodiscard]] constexpr std::string_view LevelName(LogLevel level) noexcept {
    constexpr std::string_view kNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    const usize idx = std::to_underlying(level);
    return (idx < std::size(kNames)) ? kNames[idx] : "INFO";
}

void AppendLogLine(BufferWriter& w, const LogRecord& r, std::string_view prefix = {}) noexcept {
    w.Append(prefix);
    w.Append("[");
    w.Append(LevelName(r.level));
    w.Append("] ");
    w.Append(Basename(r.file));
    w.Append(":");
    w.AppendU64(r.line);
    w.Append(" ");
    w.Append(r.message.View());
    w.Append("\n");
}

std::mutex g_logMutex{};
std::atomic<LogLevel> g_minLogLevel{LogLevel::Info};
LogSinkFn g_logSink{&Logger::DefaultStderrSink};
void* g_logSinkCtx{nullptr};
std::array<LogRecord, Logger::kHistoryCapacity> g_logRing{};
std::atomic<u64> g_logSequence{0};

std::atomic<AssertHandlerFn> g_assertHandler{nullptr};
std::atomic<void*> g_assertUserCtx{nullptr};
#if defined(NDEBUG)
std::atomic<bool> g_abortOnUnhandledAssert{false};
#else
std::atomic<bool> g_abortOnUnhandledAssert{true};
#endif

char g_dumpPath[256] = "enginen_crash.dmp.txt";
thread_local std::array<TraceFrameEntry, CrashHandler::kMaxTraceDepth> g_traceStack{};
thread_local u32 g_traceDepth = 0U;

void SignalHandlerCallback(int sig) noexcept {
    const char* name = "FATAL_SIGNAL";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE:  name = "SIGFPE";  break;
        case SIGILL:  name = "SIGILL";  break;
        default: break;
    }
    (void)CrashHandler::WriteCrashDump(name);
    std::_Exit(128 + sig);
}

} // namespace

void Logger::DefaultStderrSink(const LogRecord& record, void*) noexcept {
    if (record.level < LogLevel::Warn) {
        return;
    }
    char lineBuf[256]{};
    BufferWriter w(lineBuf);
    AppendLogLine(w, record);
    (void)std::fwrite(lineBuf, 1U, w.pos, stderr);
    (void)std::fflush(stderr);
}

void Logger::SetMinLevel(LogLevel level) noexcept { g_minLogLevel.store(level, std::memory_order_relaxed); }
LogLevel Logger::MinLevel() noexcept { return g_minLogLevel.load(std::memory_order_relaxed); }

void Logger::SetSink(LogSinkFn sinkFn, void* userCtx) noexcept {
    std::lock_guard lock(g_logMutex);
    g_logSink = sinkFn ? sinkFn : &Logger::DefaultStderrSink;
    g_logSinkCtx = sinkFn ? userCtx : nullptr;
}

void Logger::Clear() noexcept {
    std::lock_guard lock(g_logMutex);
    g_logRing.fill(LogRecord{});
    g_logSequence.store(0ULL, std::memory_order_release);
}

void Logger::Log(LogLevel level, StringId category, std::string_view message, std::source_location loc) noexcept {
    if (level < g_minLogLevel.load(std::memory_order_relaxed) || level == LogLevel::Off) {
        return;
    }

    LogRecord rec{};
    rec.category = category;
    rec.file = loc.file_name() ? loc.file_name() : "<file>";
    rec.function = loc.function_name() ? loc.function_name() : "<unknown>";
    rec.line = loc.line();
    rec.level = level;
    (void)rec.message.Assign(message.substr(0, std::min(message.size(), FixedString<90>::kMaxLength)));

    LogSinkFn sinkCopy = nullptr;
    void* sinkCtxCopy = nullptr;
    {
        std::lock_guard lock(g_logMutex);
        const u64 seq = g_logSequence.load(std::memory_order_relaxed) + 1ULL;
        rec.sequence = seq;
        g_logRing[(seq - 1ULL) & (kHistoryCapacity - 1ULL)] = rec;
        g_logSequence.store(seq, std::memory_order_release);
        sinkCopy = g_logSink;
        sinkCtxCopy = g_logSinkCtx;
    }

    if (sinkCopy != nullptr) {
        sinkCopy(rec, sinkCtxCopy);
    }
}

u32 Logger::RecentRecords(std::span<LogRecord> outRecords) noexcept {
    if (outRecords.empty()) {
        return 0U;
    }
    std::lock_guard lock(g_logMutex);
    const u64 total = g_logSequence.load(std::memory_order_relaxed);
    const usize maxOut = std::min(kHistoryCapacity, outRecords.size());
    const u32 count = static_cast<u32>(std::min<u64>(total, maxOut));
    const u64 startSeq = total - count + 1ULL;
    for (u32 i = 0U; i < count; ++i) {
        outRecords[i] = g_logRing[(startSeq + i - 1ULL) & (kHistoryCapacity - 1ULL)];
    }
    return count;
}

u64 Logger::TotalLoggedCount() noexcept { return g_logSequence.load(std::memory_order_acquire); }

void SetAssertHandler(AssertHandlerFn handler, void* userCtx) noexcept {
    g_assertUserCtx.store(userCtx, std::memory_order_relaxed);
    g_assertHandler.store(handler, std::memory_order_release);
}

void SetAbortOnUnhandledAssert(bool abortOnFail) noexcept {
    g_abortOnUnhandledAssert.store(abortOnFail, std::memory_order_relaxed);
}

bool ReportAssertionFailure(const char* expression, std::string_view message, std::source_location loc) noexcept {
    using namespace en::literals;
    const char* exprStr = expression ? expression : "Assertion failed";
    Logger::Log(LogLevel::Fatal, "core/assert"_sid, message.empty() ? exprStr : message, loc);

    if (const AssertHandlerFn handler = g_assertHandler.load(std::memory_order_acquire)) {
        return handler(expression ? expression : "", message, loc, g_assertUserCtx.load(std::memory_order_relaxed));
    }

    (void)CrashHandler::WriteCrashDump(exprStr);
    if (g_abortOnUnhandledAssert.load(std::memory_order_relaxed)) {
        std::abort();
    }
    return false;
}

void CrashHandler::Install(const char* dumpFilePath) noexcept {
    if (dumpFilePath) {
        const usize len = std::min<usize>(std::strlen(dumpFilePath), sizeof(g_dumpPath) - 1U);
        std::memcpy(g_dumpPath, dumpFilePath, len);
        g_dumpPath[len] = '\0';
    }
    std::signal(SIGSEGV, &SignalHandlerCallback);
    std::signal(SIGABRT, &SignalHandlerCallback);
    std::signal(SIGFPE, &SignalHandlerCallback);
    std::signal(SIGILL, &SignalHandlerCallback);
}

void CrashHandler::PushFrame(std::source_location loc) noexcept {
    if (g_traceDepth < kMaxTraceDepth) {
        g_traceStack[g_traceDepth] = TraceFrameEntry{
            loc.function_name() ? loc.function_name() : "<unknown>",
            loc.file_name() ? loc.file_name() : "<file>",
            loc.line(),
            g_traceDepth,
        };
    }
    ++g_traceDepth;
}

void CrashHandler::PopFrame() noexcept {
    if (g_traceDepth > 0U) {
        --g_traceDepth;
    }
}

void CrashHandler::ResetThreadTrace() noexcept { g_traceDepth = 0U; }
u32 CrashHandler::CurrentDepth() noexcept { return g_traceDepth; }

u32 CrashHandler::CaptureStackTrace(std::span<TraceFrameEntry> outFrames) noexcept {
    const u32 stored = std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth));
    const u32 count = std::min<u32>(stored, static_cast<u32>(outFrames.size()));
    for (u32 i = 0U; i < count; ++i) {
        outFrames[i] = g_traceStack[stored - 1U - i];
    }
    return count;
}

usize CrashHandler::FormatCrashReport(const char* reason, std::span<char> outText) noexcept {
    if (outText.empty()) {
        return 0U;
    }
    BufferWriter w(outText);
    w.Append("EngineN 2026 Crash Report\nReason: ");
    w.Append((reason && reason[0] != '\0') ? reason : "Unknown");
    w.Append("\nStack Trace (Most Recent Call First):\n");

    const u32 stored = std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth));
    for (u32 i = 0U; i < stored; ++i) {
        const TraceFrameEntry& e = g_traceStack[stored - 1U - i];
        w.Append("  #");
        w.AppendU64(i);
        w.Append(" ");
        w.Append(e.functionName ? e.functionName : "<unknown>");
        w.Append(" at ");
        w.Append(Basename(e.fileName));
        w.Append(":");
        w.AppendU64(e.lineNumber);
        w.Append("\n");
    }

    const u64 total = g_logSequence.load(std::memory_order_acquire);
    const u32 logCount = static_cast<u32>(std::min<u64>(total, 8ULL));
    if (logCount > 0U) {
        const u64 startSeq = total - logCount + 1ULL;
        w.Append("Recent Log History:\n");
        for (u32 i = 0U; i < logCount; ++i) {
            const LogRecord& r = g_logRing[(startSeq + i - 1ULL) & (Logger::kHistoryCapacity - 1ULL)];
            if (r.sequence != 0ULL) {
                AppendLogLine(w, r, "  ");
            }
        }
    }
    return w.pos;
}

bool CrashHandler::WriteCrashDump(const char* reason) noexcept {
    if (g_dumpPath[0] == '\0') {
        return false;
    }
    char buf[4096]{};
    const usize len = FormatCrashReport(reason, buf);
    std::FILE* fp = std::fopen(g_dumpPath, "wb");
    if (!fp) {
        return false;
    }
    const bool ok = std::fwrite(buf, 1U, len, fp) == len;
    (void)std::fclose(fp);
    return ok;
}

} // namespace en

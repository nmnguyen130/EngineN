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

class FixedBufferWriter {
public:
    constexpr explicit FixedBufferWriter(std::span<char> buf) noexcept : m_buf(buf) {
        if (!m_buf.empty()) m_buf[0] = '\0';
    }

    void Append(std::string_view sv) noexcept {
        if (m_buf.size() <= 1U || sv.empty()) return;
        const usize avail = (m_buf.size() - 1U) - m_pos;
        const usize toCopy = std::min(sv.size(), avail);
        if (toCopy > 0U) {
            std::memcpy(m_buf.data() + m_pos, sv.data(), toCopy);
            m_pos += toCopy;
            m_buf[m_pos] = '\0';
        }
    }

    void AppendU64(u64 val) noexcept {
        if (m_buf.size() <= 1U || m_pos + 1U >= m_buf.size()) return;
        char scratch[24]{};
        auto [ptr, ec] = std::to_chars(scratch, scratch + sizeof(scratch), val);
        if (ec == std::errc{}) {
            Append(std::string_view(scratch, static_cast<usize>(ptr - scratch)));
        }
    }

    [[nodiscard]] constexpr usize Size() const noexcept { return m_pos; }

private:
    std::span<char> m_buf{};
    usize m_pos{0};
};

[[nodiscard]] constexpr std::string_view ExtractBasename(std::string_view path) noexcept {
    const usize slashPos = path.find_last_of("/\\");
    return (slashPos == std::string_view::npos) ? path : path.substr(slashPos + 1U);
}

[[nodiscard]] constexpr std::string_view LevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off:   return "OFF";
    }
    return "INFO";
}

std::mutex g_logMutex{};
std::atomic<LogLevel> g_minLogLevel{LogLevel::Info};
LogSinkFn g_logSink{nullptr};
void* g_logSinkCtx{nullptr};
std::array<LogRecord, Logger::kHistoryCapacity> g_logRing{};
std::atomic<u64> g_logSequence{0};

std::atomic<AssertHandlerFn> g_assertHandler{nullptr};
std::atomic<void*> g_assertUserCtx{nullptr};

char g_dumpPath[256] = "enginen_crash.dmp.txt";
thread_local std::array<TraceFrameEntry, CrashHandler::kMaxTraceDepth> g_traceStack{};
thread_local u32 g_traceDepth = 0U;

void SignalHandlerCallback(int sig) noexcept {
    const char* sigName = "FATAL_SIGNAL";
    if (sig == SIGSEGV) sigName = "SIGSEGV (Segmentation Fault)";
    else if (sig == SIGABRT) sigName = "SIGABRT (Abort)";
    else if (sig == SIGFPE) sigName = "SIGFPE (Floating Point Exception)";
    else if (sig == SIGILL) sigName = "SIGILL (Illegal Instruction)";
    (void)CrashHandler::WriteCrashDump(sigName);
    std::_Exit(128 + sig);
}

} // namespace

void Logger::SetMinLevel(LogLevel level) noexcept {
    g_minLogLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::MinLevel() noexcept {
    return g_minLogLevel.load(std::memory_order_relaxed);
}

void Logger::SetSink(LogSinkFn sinkFn, void* userCtx) noexcept {
    std::lock_guard lock(g_logMutex);
    g_logSink = sinkFn;
    g_logSinkCtx = userCtx;
}

void Logger::Clear() noexcept {
    std::lock_guard lock(g_logMutex);
    g_logRing.fill(LogRecord{});
    g_logSequence.store(0ULL, std::memory_order_release);
}

void Logger::Log(
    LogLevel level,
    StringId category,
    std::string_view message,
    std::source_location loc) noexcept {
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

    if (sinkCopy) {
        sinkCopy(rec, sinkCtxCopy);
    }
}

u32 Logger::RecentRecords(std::span<LogRecord> outRecords) noexcept {
    if (outRecords.empty()) return 0U;
    std::lock_guard lock(g_logMutex);
    const u64 total = g_logSequence.load(std::memory_order_relaxed);
    const u64 available = std::min<u64>(total, kHistoryCapacity);
    const u32 count = static_cast<u32>(std::min<u64>(available, outRecords.size()));
    const u64 startSeq = total - count + 1ULL;
    for (u32 i = 0U; i < count; ++i) {
        outRecords[i] = g_logRing[(startSeq + i - 1ULL) & (kHistoryCapacity - 1ULL)];
    }
    return count;
}

u64 Logger::TotalLoggedCount() noexcept {
    return g_logSequence.load(std::memory_order_acquire);
}

void SetAssertHandler(AssertHandlerFn handler, void* userCtx) noexcept {
    g_assertUserCtx.store(userCtx, std::memory_order_relaxed);
    g_assertHandler.store(handler, std::memory_order_release);
}

bool ReportAssertionFailure(
    const char* expression,
    std::string_view message,
    std::source_location loc) noexcept {
    using namespace en::literals;
    Logger::Log(
        LogLevel::Fatal,
        "core/assert"_sid,
        message.empty() ? (expression ? expression : "Assertion failed") : message,
        loc);

    if (const AssertHandlerFn handler = g_assertHandler.load(std::memory_order_acquire)) {
        return handler(
            expression ? expression : "",
            message,
            loc,
            g_assertUserCtx.load(std::memory_order_relaxed));
    }

    (void)CrashHandler::WriteCrashDump(expression ? expression : "Assertion Failure");
    return false;
}

void CrashHandler::Install(const char* dumpFilePath) noexcept {
    if (dumpFilePath && dumpFilePath[0] != '\0') {
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

void CrashHandler::ResetThreadTrace() noexcept {
    g_traceDepth = 0U;
}

u32 CrashHandler::CurrentDepth() noexcept {
    return g_traceDepth;
}

u32 CrashHandler::CaptureStackTrace(std::span<TraceFrameEntry> outFrames) noexcept {
    const u32 storedDepth = std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth));
    const u32 count = std::min<u32>(storedDepth, static_cast<u32>(outFrames.size()));
    for (u32 i = 0U; i < count; ++i) {
        outFrames[i] = g_traceStack[storedDepth - 1U - i];
    }
    return count;
}

usize CrashHandler::FormatCrashReport(const char* reason, std::span<char> outText) noexcept {
    if (outText.empty()) return 0U;
    FixedBufferWriter w(outText);
    w.Append("EngineN 2026 Crash Report\nReason: ");
    w.Append((reason && reason[0] != '\0') ? reason : "Unknown");
    w.Append("\nStack Trace (Most Recent Call First):\n");

    const u32 storedDepth = std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth));
    for (u32 i = 0U; i < storedDepth; ++i) {
        const TraceFrameEntry& entry = g_traceStack[storedDepth - 1U - i];
        w.Append("  #");
        w.AppendU64(i);
        w.Append(" ");
        w.Append(entry.functionName ? entry.functionName : "<unknown>");
        w.Append(" at ");
        w.Append(entry.fileName ? ExtractBasename(entry.fileName) : "<file>");
        w.Append(":");
        w.AppendU64(entry.lineNumber);
        w.Append("\n");
    }

    // Lock-free snapshot of recent log history so crash signals inside Logger::Log never deadlock on g_logMutex
    const u64 total = g_logSequence.load(std::memory_order_acquire);
    const u64 available = std::min<u64>(total, Logger::kHistoryCapacity);
    const u32 logCount = static_cast<u32>(std::min<u64>(available, 8ULL));
    if (logCount > 0U) {
        const u64 startSeq = total - logCount + 1ULL;
        w.Append("Recent Log History:\n");
        for (u32 i = 0U; i < logCount; ++i) {
            const LogRecord r = g_logRing[(startSeq + i - 1ULL) & (Logger::kHistoryCapacity - 1ULL)];
            w.Append("  [");
            w.Append(LevelToString(r.level));
            w.Append("] ");
            w.Append(r.file ? ExtractBasename(r.file) : "<file>");
            w.Append(":");
            w.AppendU64(r.line);
            w.Append(" ");
            w.Append(r.message.View());
            w.Append("\n");
        }
    }
    return w.Size();
}

bool CrashHandler::WriteCrashDump(const char* reason) noexcept {
    if (g_dumpPath[0] == '\0') return false;
    char buf[4096]{};
    const usize len = FormatCrashReport(reason, buf);
    std::FILE* fp = std::fopen(g_dumpPath, "wb");
    if (!fp) return false;
    const usize written = std::fwrite(buf, 1U, len, fp);
    std::fclose(fp);
    return written == len;
}

} // namespace en

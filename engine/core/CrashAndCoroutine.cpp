#include "core/CrashAndCoroutine.h"
#include "core/Serialization.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace en {

namespace {
char g_dumpPath[256] = "enginen_crash.dmp.txt";
thread_local std::array<TraceFrameEntry, CrashHandler::kMaxTraceDepth> g_traceStack{};
thread_local u32 g_traceDepth = 0;

void SignalHandlerCallback(int sig) noexcept {
    const char* sigName = "FATAL_SIGNAL";
    if (sig == SIGSEGV) sigName = "SIGSEGV (Segmentation Fault)";
    else if (sig == SIGABRT) sigName = "SIGABRT (Abort)";
    else if (sig == SIGFPE) sigName = "SIGFPE (Floating Point Exception)";
    else if (sig == SIGILL) sigName = "SIGILL (Illegal Instruction)";
    (void)CrashHandler::WriteCrashDump(sigName);
    std::_Exit(128 + sig);
}

#if defined(_WIN32)
LONG WINAPI Win32UnhandledExceptionFilter(EXCEPTION_POINTERS*) noexcept {
    (void)CrashHandler::WriteCrashDump("Win32 Unhandled Structured Exception");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
} // namespace

void CrashHandler::Install(const char* dumpFilePath) noexcept {
    if (dumpFilePath && dumpFilePath[0] != '\0') {
        const usize n = std::min<usize>(std::strlen(dumpFilePath), sizeof(g_dumpPath) - 1U);
        std::memcpy(g_dumpPath, dumpFilePath, n);
        g_dumpPath[n] = '\0';
    }
    std::signal(SIGSEGV, &SignalHandlerCallback);
    std::signal(SIGABRT, &SignalHandlerCallback);
    std::signal(SIGFPE, &SignalHandlerCallback);
    std::signal(SIGILL, &SignalHandlerCallback);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(&Win32UnhandledExceptionFilter);
#endif
}

void CrashHandler::PushFrame(const char* fn, const char* file, u32 line) noexcept {
    if (g_traceDepth < kMaxTraceDepth) {
        g_traceStack[g_traceDepth] = TraceFrameEntry{fn, file, line, g_traceDepth};
    }
    ++g_traceDepth;
}

void CrashHandler::PopFrame() noexcept {
    if (g_traceDepth > 0) --g_traceDepth;
}

u32 CrashHandler::CurrentDepth() noexcept { return g_traceDepth; }

u32 CrashHandler::CaptureStackTrace(std::span<TraceFrameEntry> outFrames) noexcept {
    const u32 count = std::min<u32>(
        std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth)),
        static_cast<u32>(outFrames.size()));
    for (u32 i = 0; i < count; ++i) {
        outFrames[i] = g_traceStack[count - 1U - i];
    }
    return count;
}

usize CrashHandler::FormatCrashReport(const char* reason, std::span<char> outText) noexcept {
    if (outText.empty()) return 0;
    JsonLiteWriter w(outText);
    w.AppendRaw("EngineN 2026 Crash Report\nReason: ");
    w.AppendRaw(reason ? reason : "Unknown");
    w.AppendRaw("\nStack Trace (Most Recent Call First):\n");
    const u32 count = std::min<u32>(g_traceDepth, static_cast<u32>(kMaxTraceDepth));
    for (u32 i = 0; i < count; ++i) {
        const TraceFrameEntry& e = g_traceStack[count - 1U - i];
        w.AppendRaw("  #");
        w.AppendInt(i);
        w.AppendRaw(" ");
        w.AppendRaw(e.functionName ? e.functionName : "<unknown>");
        w.AppendRaw(" at ");
        w.AppendRaw(e.fileName ? e.fileName : "<file>");
        w.AppendRaw(":");
        w.AppendInt(e.lineNumber);
        w.AppendRaw("\n");
    }
    return w.Size();
}

bool CrashHandler::WriteCrashDump(const char* reason) noexcept {
    char buf[4096]{};
    const usize len = FormatCrashReport(reason, buf);
    std::FILE* fp = std::fopen(g_dumpPath, "wb");
    if (!fp) return false;
    const usize written = std::fwrite(buf, 1, len, fp);
    std::fclose(fp);
    return written == len;
}

bool CoroutineScheduler::Spawn(StringId name, StepFn fn, void* userCtx) noexcept {
    if (!fn) return false;
    for (TaskSlot& s : m_slots) {
        if (!s.active) {
            s.fn = fn;
            s.userCtx = userCtx;
            s.name = name;
            s.stage = 0;
            s.currentWait = CoroutineWait{};
            s.active = true;
            ++m_activeCount;
            return true;
        }
    }
    return false;
}

bool CoroutineScheduler::CancelByName(StringId name) noexcept {
    bool cancelled = false;
    for (TaskSlot& s : m_slots) {
        if (s.active && s.name == name) {
            s.active = false;
            if (m_activeCount > 0) --m_activeCount;
            cancelled = true;
        }
    }
    return cancelled;
}

void CoroutineScheduler::Step(f32 dtSec, std::span<const u64> worldFlagWords) noexcept {
    for (TaskSlot& s : m_slots) {
        if (!s.active || !s.fn) continue;

        bool ready = true;
        switch (s.currentWait.kind) {
            case CoroutineWaitKind::None:
                ready = true;
                break;
            case CoroutineWaitKind::Ticks:
                if (s.currentWait.waitTicks > 0) {
                    --s.currentWait.waitTicks;
                    ready = (s.currentWait.waitTicks == 0);
                }
                break;
            case CoroutineWaitKind::Seconds:
                s.currentWait.waitSeconds -= dtSec;
                ready = (s.currentWait.waitSeconds <= 0.0f);
                break;
            case CoroutineWaitKind::WorldFlag: {
                const u16 fid = s.currentWait.worldFlagId;
                const u16 wordIdx = fid >> 6U;
                ready = (wordIdx < worldFlagWords.size()) &&
                        ((worldFlagWords[wordIdx] & (1ULL << (fid & 63U))) != 0ULL);
                break;
            }
            case CoroutineWaitKind::Completed:
                s.active = false;
                if (m_activeCount > 0) --m_activeCount;
                continue;
        }

        if (ready) {
            s.currentWait = s.fn(s.stage, s.userCtx);
            if (s.currentWait.finished || s.currentWait.kind == CoroutineWaitKind::Completed) {
                s.active = false;
                if (m_activeCount > 0) --m_activeCount;
            }
        }
    }
}

} // namespace en

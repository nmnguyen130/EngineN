#pragma once

#include "core/Types.h"
#include <array>

namespace en {

struct TraceFrameEntry {
    const char* functionName{nullptr};
    const char* fileName{nullptr};
    u32 lineNumber{0};
    u32 depth{0};
};
static_assert(sizeof(TraceFrameEntry) == 24);

// Captures breadcrumb call-stack frames and writes diagnostic crash dump files on fatal signals.
// Static process-wide utility with thread-safe ring storage and zero heap allocation.
// Invariant: PushFrame and PopFrame remain balanced via ScopedTraceFrame RAII guards.
class CrashHandler {
public:
    static constexpr usize kMaxTraceDepth = 32;

    static void Install(const char* dumpFilePath = "enginen_crash.dmp.txt") noexcept;
    static void PushFrame(const char* fn, const char* file, u32 line) noexcept;
    static void PopFrame() noexcept;

    [[nodiscard]] static u32 CaptureStackTrace(std::span<TraceFrameEntry> outFrames) noexcept;
    [[nodiscard]] static usize FormatCrashReport(const char* reason, std::span<char> outText) noexcept;
    static bool WriteCrashDump(const char* reason) noexcept;
    [[nodiscard]] static u32 CurrentDepth() noexcept;
};

// RAII call-stack breadcrumb guard for CrashHandler.
class ScopedTraceFrame {
public:
    ScopedTraceFrame(const char* fn, const char* file, u32 line) noexcept {
        CrashHandler::PushFrame(fn, file, line);
    }
    ~ScopedTraceFrame() noexcept { CrashHandler::PopFrame(); }
    ScopedTraceFrame(const ScopedTraceFrame&) = delete;
    ScopedTraceFrame& operator=(const ScopedTraceFrame&) = delete;
};

#define EN_TRACE_SCOPE() ::en::ScopedTraceFrame _enTraceGuard_##__LINE__(__func__, __FILE__, __LINE__)

enum class CoroutineWaitKind : u8 {
    None = 0,
    Ticks = 1,
    Seconds = 2,
    WorldFlag = 3,
    Completed = 4,
};

struct CoroutineWait {
    f32 waitSeconds{0.0f};
    u32 waitTicks{0};
    u16 worldFlagId{0};
    CoroutineWaitKind kind{CoroutineWaitKind::None};
    bool finished{false};

    [[nodiscard]] static constexpr CoroutineWait NextTick() noexcept {
        return {0.0f, 1, 0, CoroutineWaitKind::Ticks, false};
    }
    [[nodiscard]] static constexpr CoroutineWait Ticks(u32 ticks) noexcept {
        return {0.0f, ticks, 0, CoroutineWaitKind::Ticks, false};
    }
    [[nodiscard]] static constexpr CoroutineWait Seconds(f32 sec) noexcept {
        return {sec, 0, 0, CoroutineWaitKind::Seconds, false};
    }
    [[nodiscard]] static constexpr CoroutineWait UntilFlag(u16 flagId) noexcept {
        return {0.0f, 0, flagId, CoroutineWaitKind::WorldFlag, false};
    }
    [[nodiscard]] static constexpr CoroutineWait Done() noexcept {
        return {0.0f, 0, 0, CoroutineWaitKind::Completed, true};
    }
};
static_assert(sizeof(CoroutineWait) == 12);

// Stackless multi-frame coroutine / fiber task scheduler for async gameplay sequences (cutscenes, fades, delayed spawns).
// Fixed-capacity value container; zero heap allocation.
// Invariant: active tasks advance their stepIndex only after their current CoroutineWait condition is satisfied.
class CoroutineScheduler {
public:
    static constexpr usize kMaxTasks = 64;
    using StepFn = CoroutineWait (*)(u32& ioStage, void* userCtx) noexcept;

    struct TaskSlot {
        StepFn fn{nullptr};
        void* userCtx{nullptr};
        StringId name{};
        u32 stage{0};
        CoroutineWait currentWait{};
        bool active{false};
    };

    constexpr void Clear() noexcept {
        m_slots.fill(TaskSlot{});
        m_activeCount = 0;
    }

    bool Spawn(StringId name, StepFn fn, void* userCtx = nullptr) noexcept;
    bool CancelByName(StringId name) noexcept;
    void Step(f32 dtSec, std::span<const u64> worldFlagWords = {}) noexcept;

    [[nodiscard]] constexpr u32 ActiveCount() const noexcept { return m_activeCount; }

private:
    std::array<TaskSlot, kMaxTasks> m_slots{};
    u32 m_activeCount{0};
};

} // namespace en

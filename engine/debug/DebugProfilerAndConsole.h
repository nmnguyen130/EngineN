#pragma once

#include "debug/DebugTypes.h"
#include "debug/DebugBatcher.h"
#include "core/time/TimeTypes.h"
#include "audio/AudioTypes.h"
#include "platform/PlatformTypes.h"

#include <array>
#include <span>
#include <string_view>

namespace en::debug {

// O(1) rolling 256-frame telemetry profiler with 64-bucket quantized percentile tracker.
// Non-owning view over the 32 KB FrameProfileSample ring in DebugSystem's arena.
// Invariant: m_writeIdx < kHistoryFrames (256) and sum(m_buckets) <= kHistoryFrames.
class DebugProfiler {
public:
    static constexpr usize kHistoryFrames = 256;
    static constexpr usize kBucketCount   = 64;

    void BindArena(std::span<FrameProfileSample, kHistoryFrames> samples) noexcept;
    void Clear() noexcept;

    void BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept;
    void RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept;
    void RecordSubsystemTelemetry(u8 catchupTicks, u16 aliveEntities,
                                  u16 submittedQuads, u16 culledQuads, u16 drawBatches,
                                  u8 radixPasses,
                                  const audio::AudioTelemetrySnapshot& audioSnap,
                                  std::span<const f32, 8> channelScales,
                                  u8 frozenChannelMask) noexcept;
    void EndFrame() noexcept;

    [[nodiscard]] u32 ComputePercentileUs(u32 targetRankOutOf256) const noexcept;
    [[nodiscard]] constexpr const FrameProfileSample& Latest() const noexcept {
        return m_samples[m_latestIdx];
    }

    void EmitProfilerAndTimeHud(OverlayFlags flags,
                                platform::Extent2i viewportSize,
                                DebugBatcher& hudBatcher) const noexcept;

private:
    std::span<FrameProfileSample> m_samples{};
    std::array<u16, kBucketCount> m_buckets{};
    u32 m_writeIdx{0};
    u32 m_latestIdx{0};
    u32 m_validFrames{0};
};

// RAII scope timer compiled out when EN_ENABLE_DEBUG_TOOLS == 0.
template <time::ClockSource ClockT>
class ScopedProfileTimer {
public:
    ScopedProfileTimer(DebugProfiler& profiler, const ClockT& clock, ProfileScopeId scope) noexcept
        : m_profiler(profiler), m_clock(clock), m_scope(scope), m_startNs(clock.NowNs()) {}

    ~ScopedProfileTimer() noexcept {
        const u64 elapsedUs = (m_clock.NowNs() - m_startNs) / 1000ull;
        m_profiler.RecordScopeUs(m_scope, static_cast<u32>(elapsedUs));
    }

    ScopedProfileTimer(const ScopedProfileTimer&) = delete;
    ScopedProfileTimer& operator=(const ScopedProfileTimer&) = delete;

private:
    DebugProfiler& m_profiler;
    const ClockT& m_clock;
    ProfileScopeId m_scope;
    u64 m_startNs;
};

enum class SymbolKind : u8 { Empty = 0, Command, CVarF32, CVarI32, CVarU32, CVarBool };
enum class LogSeverity : u8 { Info = 0, Success, Warning, Error, Echo };

// Zero-allocation tokenized view over a command line (supports up to 8 tokens).
class ConsoleArgs {
public:
    static constexpr usize kMaxTokens = 8;

    constexpr ConsoleArgs() noexcept = default;
    explicit ConsoleArgs(std::string_view rawLine) noexcept;

    [[nodiscard]] constexpr usize Count() const noexcept { return m_count; }
    [[nodiscard]] constexpr std::string_view operator[](usize idx) const noexcept {
        return idx < m_count ? m_tokens[idx] : std::string_view{};
    }
    [[nodiscard]] Result<i64, DebugErrc> GetInt(usize idx) const noexcept;
    [[nodiscard]] Result<f32, DebugErrc> GetFloat(usize idx) const noexcept;
    [[nodiscard]] Result<bool, DebugErrc> GetBool(usize idx) const noexcept;

private:
    std::array<std::string_view, kMaxTokens> m_tokens{};
    usize m_count{0};
};

class DebugConsole;
using ConsoleCmdCallback = void (*)(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept;

// Exact 32-byte cache-aligned unified symbol entry (2 symbols per 64B cache line).
struct alignas(32) ConsoleSymbol {
    u32 nameHash{0};
    SymbolKind kind{SymbolKind::Empty};
    u8 nameLen{0};
    u16 reserved{0};
    const char* namePtr{nullptr};
    void* targetPtr{nullptr};
    union {
        ConsoleCmdCallback cmdFn{nullptr};
        struct { f32 minVal; f32 maxVal; } f32Bounds;
        struct { i32 minVal; i32 maxVal; } i32Bounds;
    };
};
static_assert(sizeof(ConsoleSymbol) == 32);

// 8-byte log descriptor indexing into the 16 KB contiguous circular text buffer.
struct LogEntryMeta {
    u16 textOffset{0};
    u16 tickLow16{0};
    u8  textLength{0};
    LogSeverity severity{LogSeverity::Info};
    u16 reserved{0};
};
static_assert(sizeof(LogEntryMeta) == 8);

// Zero-allocation in-game command & CVar console with Fibonacci open-addressed symbol lookup.
// Non-owning view over symbol, log metadata, log text, and command history slices in DebugSystem's arena.
class DebugConsole {
public:
    static constexpr usize kMaxSymbols     = 256;
    static constexpr usize kMaxLogEntries  = 512;
    static constexpr usize kLogTextBytes   = 16384;
    static constexpr usize kMaxHistory     = 32;
    static constexpr usize kMaxPendingCmds = 4;

    void BindArena(std::span<ConsoleSymbol, kMaxSymbols> symbolTable,
                   std::span<LogEntryMeta, kMaxLogEntries> logMeta,
                   std::span<char, kLogTextBytes> logText,
                   std::span<std::array<char, 128>, kMaxHistory> historyBuf) noexcept;
    void ClearLog() noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCommand(
        std::string_view staticName,
        ConsoleCmdCallback fn,
        void* userCtx = nullptr) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarF32(
        std::string_view staticName,
        f32* target, f32 minVal, f32 maxVal) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarI32(
        std::string_view staticName,
        i32* target, i32 minVal, i32 maxVal) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarBool(
        std::string_view staticName,
        bool* target) noexcept;

    void Print(LogSeverity severity, std::string_view msg, u64 tickIndex = 0) noexcept;

    void EnqueueCommandLine(std::string_view line) noexcept;
    void FlushPendingCommands(u64 currentTickIndex) noexcept;
    void ExecuteImmediate(std::string_view line, u64 currentTickIndex = 0) noexcept;

    constexpr void SetOpen(bool open) noexcept { m_isOpen = open; }
    [[nodiscard]] constexpr bool IsOpen() const noexcept { return m_isOpen; }
    void OnTextInput(std::string_view utf8) noexcept;
    void OnKeyPress(u16 scancode, bool shift, bool ctrl) noexcept;
    void AutoCompleteInput() noexcept;

    [[nodiscard]] constexpr std::span<const ConsoleSymbol> Symbols() const noexcept { return m_symbols; }

    void EmitConsoleOverlay(platform::Extent2i viewportSize, DebugBatcher& hudBatcher) const noexcept;

private:
    [[nodiscard]] Result<ConsoleSymbol*, DebugErrc> InsertSymbolSlot(std::string_view staticName) noexcept;
    [[nodiscard]] ConsoleSymbol* FindSymbol(std::string_view name) noexcept;

    std::span<ConsoleSymbol> m_symbols{};
    std::span<LogEntryMeta>  m_logMeta{};
    std::span<char>          m_logText{};
    std::span<std::array<char, 128>> m_history{};
    std::array<std::array<char, 120>, kMaxPendingCmds> m_pendingCmds{};
    std::array<u8, kMaxPendingCmds> m_pendingLens{};
    std::array<char, 120> m_inputBuf{};
    u8  m_pendingCount{0};
    u8  m_inputLen{0};
    u8  m_cursorPos{0};
    u8  m_historyWriteIdx{0};
    u8  m_historyCount{0};
    i8  m_historyBrowseIdx{-1};
    u16 m_textWriteCursor{0};
    u32 m_logWriteIdx{0};
    u32 m_logCount{0};
    u32 m_scrollOffset{0};
    bool m_isOpen{false};
};

} // namespace en::debug

#pragma once

#include "debug/DebugBatcher.h"

namespace en {

inline constexpr u32 kMaxConsoleSymbols = 256U;
inline constexpr u32 kMaxConsoleLogs = 512U;
inline constexpr u32 kConsoleLogTextBytes = 16384U;

enum class ConsoleSymbolKind : u8 {
    Empty    = 0,
    Command  = 1,
    CVarF32  = 2,
    CVarI32  = 3,
    CVarU32  = 4,
    CVarBool = 5,
};

class DebugConsole;

// 32-byte (2 per 64B cache line, zero padding) open-addressed command or live CVar table entry.
struct alignas(32) ConsoleSymbol {
    const char* namePtr{nullptr};
    void* targetPtr{nullptr};
    union {
        void (*cmdFn)(std::string_view args, void* userCtx, DebugConsole& console) noexcept;
        struct {
            f32 minVal;
            f32 maxVal;
        } f32Bounds;
        struct {
            i32 minVal;
            i32 maxVal;
        } i32Bounds;
        struct {
            u32 minVal;
            u32 maxVal;
        } u32Bounds;
    } spec{};
    u32 nameHash{0U};
    ConsoleSymbolKind kind{ConsoleSymbolKind::Empty};
    u8 nameLen{0U};
    u16 flags{0U};

    [[nodiscard]] constexpr std::string_view Name() const noexcept {
        return namePtr ? std::string_view{namePtr, nameLen} : std::string_view{};
    }
};
static_assert(sizeof(ConsoleSymbol) == 32);

// 8-byte circular log ring metadata entry pointing into the 16 KB log character slab.
struct alignas(8) LogEntryMeta {
    u32 textOffset{0U};
    u16 tickLow16{0U};
    u8 textLength{0U};
    LogSeverity severity{LogSeverity::Info};
};
static_assert(sizeof(LogEntryMeta) == 8);

// 160-byte (5 x 32B blocks exact, zero padding) O(1) 64-bucket quantized percentile frame profiler and sparkline plotter.
// Non-owning view over 256 FrameProfileSamples and 4 WatchPlotTracks; computes P50/P95/P99 via bit-scanning without sorting.
// Invariant: sizeof(DebugProfiler) == 160 and m_activeBucketMask bit B is set iff m_buckets[B] > 0.
class alignas(32) DebugProfiler {
public:
    constexpr void Init(
        std::span<FrameProfileSample, kProfileHistoryFrames> sampleStorage,
        std::span<WatchPlotTrack, kMaxWatchPlots> plotStorage) noexcept {
        m_samples = sampleStorage.data();
        m_plots = plotStorage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        m_buckets.fill(0U);
        if (m_samples) {
            for (u32 i = 0U; i < kProfileHistoryFrames; ++i) {
                m_samples[i] = FrameProfileSample{};
            }
        }
        if (m_plots) {
            for (u32 i = 0U; i < kMaxWatchPlots; ++i) {
                m_plots[i] = WatchPlotTrack{};
            }
        }
        m_activeBucketMask = 0ULL;
        m_currentIdx = 0U;
        m_validFrames = 0U;
        m_spikeCount = 0U;
        m_spikeThresholdUs = 18000U;
    }

    void BeginFrame(u32 frameIndex, u32 simTickIndex) noexcept;
    void RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept;
    void RecordSubsystemTelemetry(const SubsystemTelemetryDesc& desc) noexcept;
    void EndFrame() noexcept;

    void PlotValue(std::string_view label, f32 value) noexcept;

    [[nodiscard]] u32 ComputePercentileUs(u32 targetRankOutOf256) const noexcept;
    [[nodiscard]] const FrameProfileSample& Latest() const noexcept;
    [[nodiscard]] const WatchPlotTrack* FindPlotTrack(std::string_view label) const noexcept;
    [[nodiscard]] constexpr u16 ValidFrames() const noexcept { return m_validFrames; }
    [[nodiscard]] constexpr u16 SpikeCount() const noexcept { return m_spikeCount; }

    void EmitProfilerAndTimeHud(
        OverlayFlags flags,
        ViewportExtent viewportSize,
        DebugBatcher& hudBatcher) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    [[nodiscard]] static constexpr u8 DurationToBucket(u32 durationUs) noexcept {
        const u32 b = durationUs / 500U;
        return static_cast<u8>(b < 63U ? b : 63U);
    }

    std::array<u16, kProfileBucketCount> m_buckets{}; // 128B (2 cache lines)
    FrameProfileSample* m_samples{nullptr};           // 8B
    WatchPlotTrack* m_plots{nullptr};                 // 8B
    u64 m_activeBucketMask{0ULL};                     // 8B
    u16 m_currentIdx{0U};                             // 2B
    u16 m_validFrames{0U};                            // 2B
    u16 m_spikeCount{0U};                             // 2B
    u16 m_spikeThresholdUs{18000U};                   // 2B
};
static_assert(sizeof(DebugProfiler) == 160);

// RAII scope timer that records elapsed microseconds into DebugProfiler (compiles to a zero-byte no-op when EN_ENABLE_DEBUG_TOOLS == 0).
template <typename ClockT>
class ScopedProfileTimer {
public:
#if EN_ENABLE_DEBUG_TOOLS
    constexpr ScopedProfileTimer(DebugProfiler& profiler, ProfileScopeId scope) noexcept
        : m_profiler(&profiler),
          m_startNs(ClockT::NowNs()),
          m_scope(scope) {}

    ~ScopedProfileTimer() noexcept {
        if (m_profiler) {
            const u64 elapsedNs = ClockT::NowNs() - m_startNs;
            m_profiler->RecordScopeUs(m_scope, static_cast<u32>(elapsedNs / 1000ULL));
        }
    }
#else
    constexpr ScopedProfileTimer(DebugProfiler&, ProfileScopeId) noexcept {}
    ~ScopedProfileTimer() noexcept = default;
#endif

    ScopedProfileTimer(const ScopedProfileTimer&) = delete;
    ScopedProfileTimer& operator=(const ScopedProfileTimer&) = delete;

private:
#if EN_ENABLE_DEBUG_TOOLS
    DebugProfiler* m_profiler{nullptr};
    u64 m_startNs{0ULL};
    ProfileScopeId m_scope{ProfileScopeId::FrameTotal};
#endif
};

// 2752-byte (43 cache lines exact, zero padding) Fibonacci open-addressed developer console and live CVar registry.
// Non-owning view over arena-allocated symbol and log tables; supports Tab prefix autocomplete and deterministic pre-tick command flushing.
// Invariant: sizeof(DebugConsole) == 2752 and history/pending line buffers pack their string length in byte 127.
class alignas(64) DebugConsole {
public:
    void Init(
        std::span<ConsoleSymbol, kMaxConsoleSymbols> symbolStorage,
        std::span<LogEntryMeta, kMaxConsoleLogs> logMetaStorage,
        std::span<char, kConsoleLogTextBytes> logTextStorage) noexcept;

    void Clear() noexcept;

    constexpr void ClearLogs() noexcept {
        m_logByteWriteCursor = 0U;
        m_logWriteIdx = 0U;
        m_logCount = 0U;
    }

    [[nodiscard]] Result<void, DebugErrc> RegisterCommand(
        std::string_view name,
        void (*fn)(std::string_view args, void* userCtx, DebugConsole& console) noexcept,
        void* userCtx = nullptr) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarF32(
        std::string_view name,
        f32* target,
        f32 minVal = -1e9f,
        f32 maxVal = 1e9f) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarI32(
        std::string_view name,
        i32* target,
        i32 minVal = -2147483647,
        i32 maxVal = 2147483647) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarU32(
        std::string_view name,
        u32* target,
        u32 minVal = 0U,
        u32 maxVal = 0xFFFFFFFFU) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarBool(
        std::string_view name,
        bool* target) noexcept;

    void Print(LogSeverity severity, std::string_view msg, u32 tickIndex = 0U) noexcept;
    void ListAllCVars(u32 tickIndex = 0U) noexcept;

    void EnqueueCommandLine(std::string_view line) noexcept;
    void FlushPendingCommands(u32 currentTickIndex = 0U) noexcept;
    void ExecuteImmediate(std::string_view line, u32 currentTickIndex = 0U) noexcept;

    bool AutocompleteInput() noexcept;
    void NavigateHistory(i32 direction) noexcept;
    void AppendInputText(std::string_view utf8) noexcept;
    void BackspaceInput() noexcept;
    void SubmitInputLine() noexcept;

    constexpr void SetOpen(bool open) noexcept { m_isOpen = open ? 1U : 0U; }
    constexpr void ToggleOpen() noexcept { m_isOpen ^= 1U; }
    [[nodiscard]] constexpr bool IsOpen() const noexcept { return m_isOpen != 0U; }

    [[nodiscard]] constexpr std::string_view InputView() const noexcept {
        return {m_inputLine.data(), m_inputLen};
    }
    [[nodiscard]] constexpr u16 SymbolCount() const noexcept { return m_symbolCount; }
    [[nodiscard]] constexpr u16 LogCount() const noexcept { return m_logCount; }
    [[nodiscard]] constexpr u64 ExecutedCommandCount() const noexcept { return m_executedCmdCount; }

    void EmitConsoleOverlay(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    [[nodiscard]] ConsoleSymbol* FindOrInsertSlot(std::string_view name, bool allowInsert) noexcept;

    // Cache Line 0 (64 bytes exact): pointers, hash accumulator, and counters
    ConsoleSymbol* m_symbols{nullptr};                      // 8B
    LogEntryMeta* m_logMeta{nullptr};                       // 8B
    char* m_logText{nullptr};                               // 8B
    u64 m_symbolHash{14695981039346656037ULL};              // 8B
    u64 m_executedCmdCount{0ULL};                           // 8B
    u64 m_autocompleteCursor{0ULL};                         // 8B
    u32 m_logByteWriteCursor{0U};                           // 4B
    u16 m_logWriteIdx{0U};                                  // 2B
    u16 m_logCount{0U};                                     // 2B
    u16 m_symbolCount{0U};                                  // 2B
    u8 m_historyWriteIdx{0U};                               // 1B
    u8 m_historyCount{0U};                                  // 1B
    i8 m_historyNavOffset{-1};                              // 1B
    u8 m_pendingCount{0U};                                  // 1B
    u8 m_inputLen{0U};                                      // 1B
    u8 m_isOpen{0U};                                        // 1B

    // Cache Lines 1..42 (2688 bytes exact): input line, 16-entry history ring, and 4-entry deterministic command queue
    std::array<char, 128> m_inputLine{};
    std::array<std::array<char, 128>, 16> m_historyLines{};
    std::array<std::array<char, 128>, 4> m_pendingLines{};
};
static_assert(sizeof(DebugConsole) == 2752);

} // namespace en

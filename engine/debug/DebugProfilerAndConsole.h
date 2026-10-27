#pragma once

#include "debug/DebugBatcher.h"

namespace en {

inline constexpr u32 kMaxConsoleSymbols = 256U;
inline constexpr u32 kMaxConsoleLogs = 512U;
inline constexpr u32 kConsoleLogTextBytes = 16384U;

class DebugConsole;

struct alignas(32) ConsoleSymbol {
    enum class Kind : u8 {
        Empty    = 0,
        Command  = 1,
        CVarF32  = 2,
        CVarI32  = 3,
        CVarBool = 4,
    };

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
    } spec{};
    u32 nameHash{0U};
    Kind kind{Kind::Empty};
    u8 nameLen{0U};

    [[nodiscard]] constexpr std::string_view Name() const noexcept {
        return namePtr ? std::string_view{namePtr, nameLen} : std::string_view{};
    }
};
static_assert(sizeof(ConsoleSymbol) == 32);

// Compact 4-byte log metadata entry (16 entries per 64B cache line).
struct alignas(4) LogEntryMeta {
    u16 textOffset{0U};
    u8 textLength{0U};
    LogSeverity severity{LogSeverity::Info};
};
static_assert(sizeof(LogEntryMeta) == 4);

// 48-bucket quantized percentile frame profiler and sparkline plotter (exact 128B / 2 cache lines).
class alignas(64) DebugProfiler {
public:
    static constexpr u16 kSpikeThresholdUs = 18000U;

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
    }

    void BeginFrame() noexcept;
    void RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept;
    void RecordSubsystemTelemetry(const SubsystemTelemetryDesc& desc) noexcept;
    void EndFrame() noexcept;

    void PlotValue(std::string_view label, f32 value) noexcept;

    [[nodiscard]] u32 ComputePercentileUs(u32 targetRankOutOf256) const noexcept;
    [[nodiscard]] const FrameProfileSample& Latest() const noexcept;
    [[nodiscard]] const WatchPlotTrack* FindPlotTrack(std::string_view label) const noexcept;
    [[nodiscard]] constexpr u16 ValidFrames() const noexcept { return m_validFrames; }

    void EmitProfilerAndTimeHud(
        OverlayFlags flags,
        ViewportExtent viewportSize,
        DebugBatcher& hudBatcher) const noexcept;

private:
    [[nodiscard]] static constexpr u8 DurationToBucket(u32 durationUs) noexcept {
        const u32 b = durationUs / 500U;
        return static_cast<u8>(b < (kProfileBucketCount - 1U) ? b : (kProfileBucketCount - 1U));
    }

    std::array<u16, kProfileBucketCount> m_buckets{};
    FrameProfileSample* m_samples{nullptr};
    WatchPlotTrack* m_plots{nullptr};
    u64 m_activeBucketMask{0ULL};
    u16 m_currentIdx{0U};
    u16 m_validFrames{0U};
};
static_assert(sizeof(DebugProfiler) == 128);

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

// Open-addressed developer console and live CVar registry.
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

    [[nodiscard]] Result<void, DebugErrc> RegisterCVarBool(
        std::string_view name,
        bool* target) noexcept;

    void Print(LogSeverity severity, std::string_view msg) noexcept;
    void ListAllCVars() noexcept;

    void EnqueueCommandLine(std::string_view line) noexcept;
    void FlushPendingCommands() noexcept;
    void ExecuteImmediate(std::string_view line) noexcept;

    bool AutocompleteInput() noexcept;
    void NavigateHistory(i32 direction) noexcept;
    void AppendInputText(std::string_view utf8) noexcept;
    void BackspaceInput() noexcept;
    void SubmitInputLine() noexcept;

    constexpr void SetOpen(bool open) noexcept { m_isOpen = open ? 1U : 0U; }
    constexpr void ToggleOpen() noexcept { m_isOpen ^= 1U; }
    [[nodiscard]] constexpr bool IsOpen() const noexcept { return m_isOpen != 0U; }

    [[nodiscard]] constexpr std::string_view InputView() const noexcept {
        return m_inputLine.View();
    }
    [[nodiscard]] constexpr u16 SymbolCount() const noexcept { return m_symbolCount; }
    [[nodiscard]] constexpr u16 LogCount() const noexcept { return m_logCount; }

    void EmitConsoleOverlay(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept;

private:
    [[nodiscard]] ConsoleSymbol* FindOrInsertSlot(std::string_view name, bool allowInsert) noexcept;
    [[nodiscard]] Result<ConsoleSymbol*, DebugErrc> AllocateSymbol(
        std::string_view name,
        ConsoleSymbol::Kind kind,
        void* targetPtr) noexcept;

    ConsoleSymbol* m_symbols{nullptr};
    LogEntryMeta* m_logMeta{nullptr};
    char* m_logText{nullptr};
    u16 m_logByteWriteCursor{0U};
    u16 m_logWriteIdx{0U};
    u16 m_logCount{0U};
    u16 m_symbolCount{0U};
    u8 m_autocompleteCursor{0U};
    u8 m_historyWriteIdx{0U};
    u8 m_historyCount{0U};
    i8 m_historyNavOffset{-1};
    u8 m_pendingCount{0U};
    u8 m_isOpen{0U};

    FixedString<128> m_inputLine{};
    std::array<FixedString<128>, 16> m_historyLines{};
    std::array<FixedString<128>, 4> m_pendingLines{};
};
static_assert(sizeof(DebugConsole) == 2752);

} // namespace en

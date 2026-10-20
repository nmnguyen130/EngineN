#pragma once

#include "debug/DebugInspector.h"

namespace en {

// 64-bucket percentile frame profiler and Chrome Tracing JSON exporter.
class alignas(64) DebugProfiler {
public:
    static constexpr usize kHistoryFrames = 256;
    static constexpr usize kBucketCount = 64;

    [[nodiscard]] bool Init(AlignedArena& arena) noexcept;
    void BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept;
    void RecordScopeUs(ProfileScopeId scope, u16 durationUs) noexcept;
    void EndFrame(u16 aliveEntities, const RenderStats& renderStats, const AudioTelemetrySnapshot& audioStats) noexcept;

    [[nodiscard]] u32 ComputePercentileUs(u32 rankOutOf256) const noexcept;
    [[nodiscard]] const FrameProfileSample& Latest() const noexcept { return m_samples[m_currentIdx]; }
    [[nodiscard]] usize ExportChromeTraceJson(std::span<char> outJsonBuf) const noexcept;
    void EmitProfilerHud(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept;

private:
    std::span<FrameProfileSample> m_samples{};
    std::array<u16, kBucketCount> m_buckets{};
    u32 m_currentIdx{0};
    u32 m_validFrames{0};
};

// Direct-memory CVar and command console with Fibonacci open addressing.
class alignas(64) DebugConsole {
public:
    using CommandFn = void (*)(std::string_view args, void* userCtx) noexcept;
    enum class SymbolKind : u8 { Empty = 0, Command = 1, Float = 2, Int = 3, Bool = 4 };

    struct alignas(32) ConsoleSymbol {
        StringId id{};
        FixedString<24> name{};
        void* targetPtr{nullptr};
        CommandFn cmdFn{nullptr};
        f32 minVal{0.0f};
        f32 maxVal{1.0f};
        SymbolKind kind{SymbolKind::Empty};
    };

    [[nodiscard]] bool Init(AlignedArena& arena, u32 maxSymbols = 128) noexcept;

    bool RegisterCommand(std::string_view name, CommandFn fn, void* userCtx = nullptr) noexcept;
    bool RegisterCVarF32(std::string_view name, f32* target, f32 minVal, f32 maxVal) noexcept;
    bool RegisterCVarI32(std::string_view name, i32* target, i32 minVal, i32 maxVal) noexcept;
    bool RegisterCVarBool(std::string_view name, bool* target) noexcept;

    void ExecuteImmediate(std::string_view cmdLine) noexcept;
    void Print(LogSeverity severity, std::string_view message) noexcept;

    [[nodiscard]] constexpr bool IsOpen() const noexcept { return m_open; }
    constexpr void SetOpen(bool open) noexcept { m_open = open; }
    [[nodiscard]] constexpr usize LogCount() const noexcept { return m_logCount; }
    void EmitConsoleOverlay(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept;

private:
    std::span<ConsoleSymbol> m_symbols{};
    std::array<FixedString<96>, 32> m_logs{};
    usize m_symbolCount{0};
    usize m_logCount{0};
    bool m_open{false};
};

// Unified in-game diagnostic coordinator owning shape pool, inspector, profiler, and console.
class alignas(64) DebugSystem {
public:
    [[nodiscard]] bool Init(usize arenaBytes = 256 * 1024) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] DebugLoopDirectives BeginFrame(const DebugFrameContext& ctx) noexcept;
    void EndSimTick(const World& world, const SimStepView& step, const TickInput& tickInput, const ActionBuffer& actionBuffer) noexcept;
    void EmitOverlays(const DebugFrameContext& ctx, RenderList& outList) noexcept;

    [[nodiscard]] constexpr OverlayFlags ActiveOverlays() const noexcept { return m_overlays; }
    constexpr void SetActiveOverlays(OverlayFlags flags) noexcept { m_overlays = flags; }
    [[nodiscard]] constexpr DebugShapePool& Shapes() noexcept { return m_shapes; }
    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr DebugProfiler& Profiler() noexcept { return m_profiler; }
    [[nodiscard]] constexpr DebugConsole& Console() noexcept { return m_console; }

private:
    OverlayFlags m_overlays{OverlayFlags::None};
    DebugLoopDirectives m_pendingDirectives{};
    DebugShapePool m_shapes{};
    DebugInspector m_inspector{};
    DebugProfiler m_profiler{};
    DebugConsole m_console{};
    AlignedArena m_arena{};
};

} // namespace en

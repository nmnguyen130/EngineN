#pragma once

#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"

namespace en {

// 64-byte (1 cache line exact, zero padding) 2D diagnostics, gizmo batching, entity inspection, profiling, and console coordinator.
// Move-only RAII subsystem owning a single AlignedArena slab; compiles to a stateless no-op when EN_ENABLE_DEBUG_TOOLS == 0.
// Invariant: sizeof(DebugSystem) == 64, zero heap allocations after Init(), and zero SDL/OS header dependencies.
class alignas(64) DebugSystem {
public:
    static constexpr usize kDefaultArenaBytes = 131072U;

    DebugSystem() noexcept = default;
    ~DebugSystem() noexcept { Shutdown(); }

    DebugSystem(const DebugSystem&) = delete;
    DebugSystem& operator=(const DebugSystem&) = delete;

    DebugSystem(DebugSystem&& other) noexcept;
    DebugSystem& operator=(DebugSystem&& other) noexcept;

#if EN_ENABLE_DEBUG_TOOLS
    [[nodiscard]] Result<void, DebugErrc> Init(const DebugSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_state != nullptr; }

    bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift = false, bool ctrl = false) noexcept;
    bool OnPlatformKeyEvent(
        u16 scancode,
        bool down,
        bool shift,
        bool ctrl,
        InputPipeline& inputPipeline) noexcept;
    bool OnPlatformTextInput(std::string_view textUtf8) noexcept;

    [[nodiscard]] DebugLoopDirectives BeginFrame(const DebugFrameContext& ctx) noexcept;

    void EndSimTick(
        const World& world,
        u32 tickIndex,
        const TickInput* tickInput = nullptr,
        const ActionBuffer* actionBuffer = nullptr) noexcept;

    void EmitOverlays(const DebugFrameContext& ctx, RenderList& outList) noexcept;

    [[nodiscard]] OverlayFlags ActiveOverlays() const noexcept;
    void SetActiveOverlays(OverlayFlags flags) noexcept;
    void ToggleOverlay(OverlayFlags flag) noexcept;

    constexpr void RequestPauseToggle() noexcept {
        m_pendingDirectives.controlFlags |= DebugLoopDirectives::kFlagTogglePause;
    }

    constexpr void RequestStateReset() noexcept {
        m_pendingDirectives.controlFlags |= DebugLoopDirectives::kFlagResetState;
    }

    constexpr void RequestStepTicks(u16 ticks = 1U) noexcept {
        m_pendingDirectives.stepTicks = static_cast<u16>(m_pendingDirectives.stepTicks + ticks);
        m_pendingDirectives.controlFlags |= DebugLoopDirectives::kFlagForcePause;
    }

    constexpr void RequestChannelTimeScale(f32 scale, TimeChannel ch = TimeChannel::World) noexcept {
        m_pendingDirectives.channelTimeScale = std::clamp(scale, 0.0f, 8.0f);
        m_pendingDirectives.targetTimeChannel = static_cast<u8>(ch) & 7U;
    }

    [[nodiscard]] DebugShapePool& Shapes() noexcept;
    [[nodiscard]] const DebugShapePool& Shapes() const noexcept;

    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr const DebugInspector& Inspector() const noexcept { return m_inspector; }

    [[nodiscard]] DebugProfiler& Profiler() noexcept;
    [[nodiscard]] const DebugProfiler& Profiler() const noexcept;

    [[nodiscard]] DebugConsole& Console() noexcept;
    [[nodiscard]] const DebugConsole& Console() const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(const DebugSystemConfig& = {}) noexcept { return {}; }
    constexpr void Shutdown() noexcept {}
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return false; }
    [[nodiscard]] constexpr bool OnPlatformKeyEvent(u16, bool, bool = false, bool = false) noexcept { return false; }
    [[nodiscard]] constexpr bool OnPlatformKeyEvent(u16, bool, bool, bool, InputPipeline&) noexcept { return false; }
    [[nodiscard]] constexpr bool OnPlatformTextInput(std::string_view) noexcept { return false; }
    [[nodiscard]] constexpr DebugLoopDirectives BeginFrame(const DebugFrameContext&) noexcept { return {}; }
    constexpr void EndSimTick(const World&, u32, const TickInput* = nullptr, const ActionBuffer* = nullptr) noexcept {}
    constexpr void EmitOverlays(const DebugFrameContext&, RenderList&) noexcept {}
    [[nodiscard]] constexpr OverlayFlags ActiveOverlays() const noexcept { return OverlayFlags::None; }
    constexpr void SetActiveOverlays(OverlayFlags) noexcept {}
    constexpr void ToggleOverlay(OverlayFlags) noexcept {}
    constexpr void RequestPauseToggle() noexcept {}
    constexpr void RequestStateReset() noexcept {}
    constexpr void RequestStepTicks(u16 = 1U) noexcept {}
    constexpr void RequestChannelTimeScale(f32, TimeChannel = TimeChannel::World) noexcept {}
    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept { return 0ULL; }
#endif

private:
    struct DebugArenaState;

    // Cache Line 0 (64 bytes exact, zero padding): 32B arena + 16B DebugInspector + 8B state pointer + 8B DebugLoopDirectives
    AlignedArena m_arena{};                     // 32B
    DebugInspector m_inspector{};               // 16B
    DebugArenaState* m_state{nullptr};          // 8B
    DebugLoopDirectives m_pendingDirectives{};  // 8B
};
static_assert(sizeof(DebugSystem) == 64);

} // namespace en

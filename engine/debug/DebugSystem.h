#pragma once

#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"

namespace en {

// 2D diagnostics, gizmo batching, entity inspection, profiling, and console coordinator.
// Compiles to a safe no-op interface when EN_ENABLE_DEBUG_TOOLS == 0 or when uninitialized at runtime.
class alignas(64) DebugSystem {
public:
    static constexpr usize kDefaultArenaBytes = 131072U;

    DebugSystem() noexcept = default;
    ~DebugSystem() noexcept { Shutdown(); }

    DebugSystem(const DebugSystem&) = delete;
    DebugSystem& operator=(const DebugSystem&) = delete;
    DebugSystem(DebugSystem&&) = delete;
    DebugSystem& operator=(DebugSystem&&) = delete;

#if EN_ENABLE_DEBUG_TOOLS
    [[nodiscard]] Result<void, DebugErrc> Init(const DebugSystemConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_state != nullptr; }

    bool OnPlatformKeyEvent(u16 scancode, bool down = true) noexcept;
    bool OnPlatformKeyEvent(u16 scancode, bool down, InputPipeline& inputPipeline) noexcept;
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
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(const DebugSystemConfig& = {}) noexcept { return {}; }
    constexpr void Shutdown() noexcept {}
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return false; }
    [[nodiscard]] constexpr bool OnPlatformKeyEvent(u16, bool = true) noexcept { return false; }
    [[nodiscard]] constexpr bool OnPlatformKeyEvent(u16, bool, InputPipeline&) noexcept { return false; }
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

    [[nodiscard]] DebugShapePool& Shapes() noexcept;
    [[nodiscard]] const DebugShapePool& Shapes() const noexcept;
    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr const DebugInspector& Inspector() const noexcept { return m_inspector; }
    [[nodiscard]] DebugProfiler& Profiler() noexcept;
    [[nodiscard]] const DebugProfiler& Profiler() const noexcept;
    [[nodiscard]] DebugConsole& Console() noexcept;
    [[nodiscard]] const DebugConsole& Console() const noexcept;
#endif

private:
    struct DebugArenaState;

    AlignedArena m_arena{};
    DebugInspector m_inspector{};
    DebugArenaState* m_state{nullptr};
    DebugLoopDirectives m_pendingDirectives{};
};
static_assert(sizeof(DebugSystem) == 64);

} // namespace en

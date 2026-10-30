#pragma once

#include "debug/DebugTools.h"

namespace en {

// Runtime developer diagnostics coordinator owning gizmo pool, profiler, console, and entity inspector.
// Move-disabled owner of an Arena; compiles to zero-overhead inline no-ops when EN_ENABLE_DEBUG_TOOLS == 0.
// Invariant: sizeof(DebugSystem) == 48.
class DebugSystem {
public:
    DebugSystem() noexcept = default;
    ~DebugSystem() noexcept { Shutdown(); }

    DebugSystem(const DebugSystem&) = delete;
    DebugSystem& operator=(const DebugSystem&) = delete;

#if EN_ENABLE_DEBUG_TOOLS
    [[nodiscard]] Result<void, DebugErrc> Init(const DebugConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool Ready() const noexcept { return m_state != nullptr; }

    bool OnKey(u16 scancode, bool down = true) noexcept;
    bool OnKey(u16 scancode, bool down, InputSystem& input) noexcept;
    bool OnText(std::string_view utf8) noexcept;

    [[nodiscard]] LoopDirectives BeginFrame(const FrameCtx& ctx) noexcept;
    void EndTick(
        const World& world,
        u32 tick,
        const TickInput* input = nullptr,
        const ActionBuffer* buffer = nullptr) noexcept;
    void Emit(const FrameCtx& ctx, RenderList& list) noexcept;

    [[nodiscard]] Overlay Overlays() const noexcept;
    void SetOverlays(Overlay flags) noexcept;
    void Toggle(Overlay flag) noexcept;

    constexpr void RequestPause() noexcept {
        m_pending.flags |= LoopDirectives::kTogglePause;
    }
    constexpr void RequestReset() noexcept {
        m_pending.flags |= LoopDirectives::kReset;
    }
    constexpr void RequestStep(u16 ticks = 1U) noexcept {
        m_pending.stepTicks = static_cast<u16>(m_pending.stepTicks + ticks);
        m_pending.flags    |= LoopDirectives::kForcePause;
    }
    constexpr void RequestTimeScale(f32 scale, TimeChannel ch = TimeChannel::World) noexcept {
        m_pending.timeScale   = std::clamp(scale, 0.0f, 8.0f);
        m_pending.timeChannel = static_cast<u8>(ch) & 7U;
    }

    [[nodiscard]] ShapePool& Shapes() noexcept;
    [[nodiscard]] const ShapePool& Shapes() const noexcept;
    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr const DebugInspector& Inspector() const noexcept { return m_inspector; }
    [[nodiscard]] DebugProfiler& Profiler() noexcept;
    [[nodiscard]] const DebugProfiler& Profiler() const noexcept;
    [[nodiscard]] DebugConsole& Console() noexcept;
    [[nodiscard]] const DebugConsole& Console() const noexcept;
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(const DebugConfig& = {}) noexcept { return {}; }
    constexpr void Shutdown() noexcept {}
    [[nodiscard]] constexpr bool Ready() const noexcept { return false; }
    [[nodiscard]] constexpr bool OnKey(u16, bool = true) noexcept { return false; }
    [[nodiscard]] constexpr bool OnKey(u16, bool, InputSystem&) noexcept { return false; }
    [[nodiscard]] constexpr bool OnText(std::string_view) noexcept { return false; }
    [[nodiscard]] constexpr LoopDirectives BeginFrame(const FrameCtx&) noexcept { return {}; }
    constexpr void EndTick(const World&, u32, const TickInput* = nullptr, const ActionBuffer* = nullptr) noexcept {}
    constexpr void Emit(const FrameCtx&, RenderList&) noexcept {}
    [[nodiscard]] constexpr Overlay Overlays() const noexcept { return Overlay::None; }
    constexpr void SetOverlays(Overlay) noexcept {}
    constexpr void Toggle(Overlay) noexcept {}
    constexpr void RequestPause() noexcept {}
    constexpr void RequestReset() noexcept {}
    constexpr void RequestStep(u16 = 1U) noexcept {}
    constexpr void RequestTimeScale(f32, TimeChannel = TimeChannel::World) noexcept {}
    [[nodiscard]] ShapePool& Shapes() noexcept;
    [[nodiscard]] const ShapePool& Shapes() const noexcept;
    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr const DebugInspector& Inspector() const noexcept { return m_inspector; }
    [[nodiscard]] DebugProfiler& Profiler() noexcept;
    [[nodiscard]] const DebugProfiler& Profiler() const noexcept;
    [[nodiscard]] DebugConsole& Console() noexcept;
    [[nodiscard]] const DebugConsole& Console() const noexcept;
#endif

private:
    struct State;

    Arena          m_arena{};
    DebugInspector m_inspector{};
    State*         m_state{nullptr};
    LoopDirectives m_pending{};
};
static_assert(sizeof(DebugSystem) == 48);

} // namespace en

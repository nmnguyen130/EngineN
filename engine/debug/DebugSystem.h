#pragma once

#include "debug/DebugBatcher.h"
#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"
#include "debug/DebugTypes.h"

#include <utility>

namespace en::input { class InputPipeline; }
namespace en::renderer { class Renderer; }

namespace en::debug {

struct DebugSystemConfig {
    OverlayFlags initialOverlays{OverlayFlags::ProfilerHud | OverlayFlags::TimeChannelHud};
    u16 toggleConsoleScancode{53}; // SDL_SCANCODE_GRAVE (`~`)
};

// Move-only RAII coordinator owning the single 192 KB debug arena and 128x64 unified debug atlas.
// Compiles down to a stateless no-op struct when EN_ENABLE_DEBUG_TOOLS == 0; stores zero subsystem pointers.
// Invariant: Zero heap allocations occur after Init().
class DebugSystem {
public:
    static constexpr usize kArenaSizeBytes = 192 * 1024;

    DebugSystem() noexcept = default;
    ~DebugSystem() noexcept { Shutdown(); }
    DebugSystem(DebugSystem&& other) noexcept;
    DebugSystem& operator=(DebugSystem&& other) noexcept;
    DebugSystem(const DebugSystem&) = delete;
    DebugSystem& operator=(const DebugSystem&) = delete;

#if EN_ENABLE_DEBUG_TOOLS
    [[nodiscard]] Result<void, DebugErrc> Init(
        renderer::Renderer& renderer,
        const DebugSystemConfig& cfg = {}) noexcept;
    void Shutdown(renderer::Renderer* renderer = nullptr) noexcept;

    [[nodiscard]] constexpr platform::EventCapture GetEventCapture() const noexcept {
        if (m_console.IsOpen()) {
            return static_cast<platform::EventCapture>(
                static_cast<u8>(platform::EventCapture::Keyboard) |
                static_cast<u8>(platform::EventCapture::Mouse));
        }
        return platform::EventCapture::None;
    }

    bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl) noexcept;
    bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl,
                            input::InputPipeline&) noexcept {
        return OnPlatformKeyEvent(scancode, down, shift, ctrl);
    }
    bool OnPlatformTextInput(std::string_view textUtf8) noexcept;

    [[nodiscard]] DebugLoopDirectives BeginFrame(const DebugBeginFrameContext& ctx) noexcept {
        m_shapePool.SetCurrentTick(ctx.timeView.tickIndex);
        m_console.FlushPendingCommands(ctx.timeView.tickIndex);

        const DebugLoopDirectives directives = std::exchange(m_pendingDirectives, DebugLoopDirectives{});

        if (ctx.isPaused || HasOverlay(m_overlays, OverlayFlags::EntityInspector)) {
            if ((ctx.frameInput.pressed & 1u) != 0 && !m_console.IsOpen()) {
                m_inspector.PickEntityAtScreenPos(ctx.world, ctx.timeView,
                                                  ctx.frameInput.mousePos, ctx.viewportSize);
            }
        }

        m_profiler.BeginFrame(ctx.frameInput.frameIndex, ctx.timeView.tickIndex);
        return directives;
    }

    void EndSimTick(const world::World& world,
                    time::SimStepView step,
                    const input::TickInput& tickInput,
                    const input::ActionBuffer& actionBuffer) noexcept;

    void EmitOverlays(const DebugEmitOverlayContext& ctx,
                      renderer::RenderList& outList) noexcept;

    [[nodiscard]] constexpr auto&& Shapes(this auto&& self) noexcept { return self.m_shapePool; }
    [[nodiscard]] constexpr auto&& Inspector(this auto&& self) noexcept { return self.m_inspector; }
    [[nodiscard]] constexpr auto&& Profiler(this auto&& self) noexcept { return self.m_profiler; }
    [[nodiscard]] constexpr auto&& Console(this auto&& self) noexcept { return self.m_console; }
    [[nodiscard]] constexpr OverlayFlags ActiveOverlays() const noexcept { return m_overlays; }
    constexpr void SetActiveOverlays(OverlayFlags flags) noexcept { m_overlays = flags; }

    constexpr void RequestSteps(u16 ticks) noexcept {
        m_pendingDirectives.stepTicks = ticks;
        m_pendingDirectives.controlFlags |= DebugLoopDirectives::kFlagForcePause;
    }
    constexpr void RequestPauseToggle() noexcept {
        m_pendingDirectives.controlFlags |= DebugLoopDirectives::kFlagTogglePause;
    }
    constexpr void RequestChannelTimeScale(u8 channel, f32 scale) noexcept {
        m_pendingDirectives.targetTimeChannel = channel;
        m_pendingDirectives.channelTimeScale  = scale;
    }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(
        renderer::Renderer&, const DebugSystemConfig& = {}) noexcept { return Ok; }
    constexpr void Shutdown(renderer::Renderer* = nullptr) noexcept {}
    [[nodiscard]] constexpr platform::EventCapture GetEventCapture() const noexcept {
        return platform::EventCapture::None;
    }
    constexpr bool OnPlatformKeyEvent(u16, bool, bool, bool) noexcept { return false; }
    constexpr bool OnPlatformKeyEvent(u16, bool, bool, bool, input::InputPipeline&) noexcept { return false; }
    constexpr bool OnPlatformTextInput(std::string_view) noexcept { return false; }
    [[nodiscard]] constexpr DebugLoopDirectives BeginFrame(const DebugBeginFrameContext&) noexcept { return {}; }
    constexpr void EndSimTick(const world::World&, time::SimStepView,
                              const input::TickInput&,
                              const input::ActionBuffer&) noexcept {}
    constexpr void EmitOverlays(const DebugEmitOverlayContext&, renderer::RenderList&) noexcept {}
    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept { return 0; }
#endif

private:
#if EN_ENABLE_DEBUG_TOOLS
    void RegisterBuiltInCommands() noexcept;

    u8* m_arenaSlab{nullptr};
    renderer::TextureHandle m_debugAtlas{};
    OverlayFlags m_overlays{OverlayFlags::None};
    u16 m_toggleConsoleScancode{53};
    DebugLoopDirectives m_pendingDirectives{};

    DebugShapePool m_shapePool{};
    DebugInspector m_inspector{};
    DebugProfiler  m_profiler{};
    DebugConsole   m_console{};
#endif
};

} // namespace en::debug

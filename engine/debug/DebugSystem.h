#pragma once

#include "core/time/TimeViews.h"
#include "debug/DebugBatcher.h"
#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"
#include "debug/DebugTypes.h"
#include "input/InputTypes.h"
#include "platform/PlatformTypes.h"

namespace en::assets { class AssetBank; }
namespace en::audio { class AudioSystem; }
namespace en::gameplay { class GameplayPipeline; }
namespace en::input { class ActionBuffer; class InputPipeline; }
namespace en::renderer { class Renderer; class RenderList; }
namespace en::world { class World; }

namespace en::debug {

struct DebugSystemConfig {
    OverlayFlags initialOverlays{OverlayFlags::ProfilerHud | OverlayFlags::TimeChannelHud};
    u16 toggleConsoleScancode{53}; // SDL_SCANCODE_GRAVE (`~`)
};

// Consolidated read-only frame context passed to DebugSystem::BeginFrame().
struct DebugBeginFrameContext {
    const world::World& world;
    time::RenderTimeView timeView{};
    const input::FrameInput& frameInput;
    platform::Extent2i viewportSize{};
    bool isPaused{false};
};

// Consolidated read-only context passed to DebugSystem::EmitOverlays().
struct DebugEmitOverlayContext {
    const world::World& world;
    const gameplay::GameplayPipeline& gameplay;
    const audio::AudioSystem& audio;
    const assets::AssetBank& assetBank;
    time::RenderTimeView timeView{};
    const input::FrameInput& frameInput;
    platform::Extent2i viewportSize{};
};

// Move-only RAII coordinator owning the single 192 KB debug arena and 128x64 unified debug atlas.
// Compiles down to a stateless no-op struct when EN_ENABLE_DEBUG_TOOLS == 0; stores zero dangling subsystem pointers.
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

    bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl,
                            input::InputPipeline& input) noexcept;
    bool OnPlatformTextInput(std::string_view textUtf8) noexcept;

    // Flushes pending console commands, performs mouse entity picking, and returns loop directives.
    [[nodiscard]] DebugLoopDirectives BeginFrame(const DebugBeginFrameContext& ctx) noexcept {
        m_console.FlushPendingCommands(ctx.timeView.tickIndex);

        DebugLoopDirectives directives{};
        if (m_pendingStepTicks > 0) {
            directives.forcePause = true;
            directives.stepTicks  = 1u;
            --m_pendingStepTicks;
        }
        if (m_pendingPauseToggle) {
            directives.togglePause = true;
            m_pendingPauseToggle   = false;
        }
        if (m_pendingTimeScaleChannel <= 8u) {
            directives.targetTimeChannel = m_pendingTimeScaleChannel;
            directives.channelTimeScale  = m_pendingTimeScaleValue;
            m_pendingTimeScaleChannel    = 0xFFu;
        }

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

    [[nodiscard]] constexpr DebugShapePool& Shapes(this auto&& self) noexcept { return self.m_shapePool; }
    [[nodiscard]] constexpr DebugInspector& Inspector(this auto&& self) noexcept { return self.m_inspector; }
    [[nodiscard]] constexpr DebugProfiler& Profiler(this auto&& self) noexcept { return self.m_profiler; }
    [[nodiscard]] constexpr DebugConsole& Console(this auto&& self) noexcept { return self.m_console; }
    [[nodiscard]] constexpr OverlayFlags ActiveOverlays() const noexcept { return m_overlays; }
    constexpr void SetActiveOverlays(OverlayFlags flags) noexcept { m_overlays = flags; }

    constexpr void RequestSteps(u16 ticks) noexcept { m_pendingStepTicks = ticks; }
    constexpr void RequestPauseToggle() noexcept { m_pendingPauseToggle = true; }
    constexpr void RequestChannelTimeScale(u8 channel, f32 scale) noexcept {
        m_pendingTimeScaleChannel = channel;
        m_pendingTimeScaleValue   = scale;
    }
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(
        renderer::Renderer&, const DebugSystemConfig& = {}) noexcept { return Ok; }
    constexpr void Shutdown(renderer::Renderer* = nullptr) noexcept {}
    constexpr bool OnPlatformKeyEvent(u16, bool, bool, bool,
                                      input::InputPipeline&) noexcept { return false; }
    constexpr bool OnPlatformTextInput(std::string_view) noexcept { return false; }
    [[nodiscard]] constexpr DebugLoopDirectives BeginFrame(const DebugBeginFrameContext&) noexcept { return {}; }
    constexpr void EndSimTick(const world::World&, time::SimStepView,
                              const input::TickInput&,
                              const input::ActionBuffer&) noexcept {}
    constexpr void EmitOverlays(const DebugEmitOverlayContext&, renderer::RenderList&) noexcept {}
#endif

private:
#if EN_ENABLE_DEBUG_TOOLS
    void RegisterBuiltInCommands() noexcept;

    u8* m_arenaSlab{nullptr};
    renderer::TextureHandle m_debugAtlas{};
    OverlayFlags m_overlays{OverlayFlags::None};
    u16 m_toggleConsoleScancode{53};
    u16 m_savedActionMask{0xFFFFu};
    u16 m_pendingStepTicks{0};
    f32 m_pendingTimeScaleValue{1.0f};
    u8  m_pendingTimeScaleChannel{0xFFu};
    bool m_pendingPauseToggle{false};

    DebugShapePool m_shapePool{};
    DebugInspector m_inspector{};
    DebugProfiler  m_profiler{};
    DebugConsole   m_console{};
#endif
};

} // namespace en::debug

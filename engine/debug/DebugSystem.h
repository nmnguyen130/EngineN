#pragma once

#include "core/time/TimePipeline.h"
#include "debug/DebugBatcher.h"
#include "debug/DebugInspector.h"
#include "debug/DebugProfilerAndConsole.h"
#include "debug/DebugTypes.h"
#include "input/InputTypes.h"

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

// Move-only RAII coordinator owning the single 192 KB debug arena and 128x64 unified debug atlas.
// Compiles down to a stateless no-op struct when EN_ENABLE_DEBUG_TOOLS == 0.
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

    // Platform event tap: intercepts `~`, F1..F6, `[` and `]` scrub keys, and routes console typing.
    bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl,
                            input::InputPipeline& input) noexcept;
    bool OnPlatformTextInput(std::string_view textUtf8) noexcept;

    // Executes queued console commands and mouse entity picking deterministically before m_time.BeginFrame().
    template <time::ClockSource ClockT>
    void BeginFrame(time::TimePipeline<ClockT>& time,
                    world::World& world,
                    gameplay::GameplayPipeline& gameplay,
                    audio::AudioSystem& audio,
                    const input::FrameInput& frameInput,
                    platform::Extent2i viewportSize) noexcept {
        BindActiveFrameContext(&world, &gameplay, &audio);
        if (m_pendingStepTicks > 0) {
            time.SetPaused(true);
            time.RequestSingleStep();
            --m_pendingStepTicks;
        }
        if (m_pendingPauseToggle) {
            time.SetPaused(!time.IsPaused());
            m_pendingPauseToggle = false;
        }
        m_console.FlushPendingCommands(time.GetRenderView().tickIndex);

        if (m_pendingTimeScaleChannel <= 8u) {
            // Applied on next SimStepView or TimeManager modulator hook.
            m_pendingTimeScaleChannel = 0xFFu;
        }

        // Alt + Left Click (or Left Click while paused) picks and pins an entity in world space.
        if (time.IsPaused() || HasOverlay(m_overlays, OverlayFlags::EntityInspector)) {
            if ((frameInput.pressed & 1u) != 0 && !m_console.IsOpen()) {
                m_inspector.PickEntityAtScreenPos(world, time.GetRenderView(),
                                                  frameInput.mousePos, viewportSize);
            }
        }

        m_profiler.BeginFrame(frameInput.frameIndex, time.GetRenderView().tickIndex);
    }

    void EndSimTick(const world::World& world,
                    time::SimStepView step,
                    const input::TickInput& tickInput,
                    const input::ActionBuffer& actionBuffer) noexcept;

    void EmitOverlays(const world::World& world,
                      const gameplay::GameplayPipeline& gameplay,
                      const audio::AudioSystem& audio,
                      const assets::AssetBank& assetBank,
                      time::RenderTimeView timeView,
                      const input::FrameInput& frameInput,
                      platform::Extent2i viewportSize,
                      renderer::RenderList& outList) noexcept;

    [[nodiscard]] constexpr DebugShapePool& Shapes() noexcept { return m_shapePool; }
    [[nodiscard]] constexpr DebugInspector& Inspector() noexcept { return m_inspector; }
    [[nodiscard]] constexpr DebugProfiler& Profiler() noexcept { return m_profiler; }
    [[nodiscard]] constexpr DebugConsole& Console() noexcept { return m_console; }
    [[nodiscard]] constexpr OverlayFlags ActiveOverlays() const noexcept { return m_overlays; }
    constexpr void SetActiveOverlays(OverlayFlags flags) noexcept { m_overlays = flags; }

    // Internal command helpers invoked by built-in console callbacks.
    constexpr void RequestSteps(u16 ticks) noexcept { m_pendingStepTicks = ticks; }
    constexpr void RequestPauseToggle() noexcept { m_pendingPauseToggle = true; }
    [[nodiscard]] constexpr world::World* ActiveWorld() const noexcept { return m_boundWorld; }
    [[nodiscard]] constexpr gameplay::GameplayPipeline* ActiveGameplay() const noexcept { return m_boundGameplay; }
    [[nodiscard]] constexpr audio::AudioSystem* ActiveAudio() const noexcept { return m_boundAudio; }
#else
    [[nodiscard]] constexpr Result<void, DebugErrc> Init(
        renderer::Renderer&, const DebugSystemConfig& = {}) noexcept { return Ok; }
    constexpr void Shutdown(renderer::Renderer* = nullptr) noexcept {}
    constexpr bool OnPlatformKeyEvent(u16, bool, bool, bool,
                                      input::InputPipeline&) noexcept { return false; }
    constexpr bool OnPlatformTextInput(std::string_view) noexcept { return false; }
    template <time::ClockSource ClockT>
    constexpr void BeginFrame(time::TimePipeline<ClockT>&, world::World&,
                              gameplay::GameplayPipeline&, audio::AudioSystem&,
                              const input::FrameInput&, platform::Extent2i) noexcept {}
    constexpr void EndSimTick(const world::World&, time::SimStepView,
                              const input::TickInput&,
                              const input::ActionBuffer&) noexcept {}
    constexpr void EmitOverlays(const world::World&, const gameplay::GameplayPipeline&,
                                const audio::AudioSystem&, const assets::AssetBank&,
                                time::RenderTimeView, const input::FrameInput&,
                                platform::Extent2i, renderer::RenderList&) noexcept {}
#endif

private:
#if EN_ENABLE_DEBUG_TOOLS
    void BindActiveFrameContext(world::World* w,
                                gameplay::GameplayPipeline* gp,
                                audio::AudioSystem* au) noexcept {
        m_boundWorld    = w;
        m_boundGameplay = gp;
        m_boundAudio    = au;
    }
    void RegisterBuiltInCommands() noexcept;

    u8* m_arenaSlab{nullptr};
    renderer::TextureHandle m_debugAtlas{};
    OverlayFlags m_overlays{OverlayFlags::None};
    u16 m_toggleConsoleScancode{53};
    u16 m_savedActionMask{0xFFFFu};
    u16 m_pendingStepTicks{0};
    u8  m_pendingTimeScaleChannel{0xFFu};
    bool m_pendingPauseToggle{false};

    world::World* m_boundWorld{nullptr};
    gameplay::GameplayPipeline* m_boundGameplay{nullptr};
    audio::AudioSystem* m_boundAudio{nullptr};

    DebugShapePool m_shapePool{};
    DebugInspector m_inspector{};
    DebugProfiler  m_profiler{};
    DebugConsole   m_console{};
#endif
};

} // namespace en::debug

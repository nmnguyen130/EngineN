#pragma once

#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"
#include "audio/AudioSystem.h"
#include "core/time/SdlClock.h"
#include "debug/DebugSystem.h"
#include "framework/FrameworkTypes.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplayPipeline.h"
#include "input/InputPipeline.h"
#include "platform/Window.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"
#include "world/World.h"

#include <string_view>

namespace en::framework {

struct ApplicationConfig {
    std::string_view appName{"EngineN"};
    std::string_view orgName{"EngineN"};
    std::string_view bootPakPath{"base.enpak"};
    bool headless{false};
    assets::PumpBudget assetPumpBudget{};
    platform::WindowConfig window{};
    renderer::RendererConfig renderer{};
    assets::AssetSystemConfig assets{};
    audio::AudioSystemConfig audio{};
    world::WorldConfig world{};
    gameplay::GameplayPipelineConfig gameplay{};
    debug::DebugSystemConfig debug{};
};

// Top-level move-only RAII runtime coordinator for EngineN (Layer 2).
// Owns all concrete subsystems in strict dependency order and drives the 120 Hz fixed-step loop.
// Invariant: Cache Line 0 packs all hot runtime coordinator state; zero heap allocations occur after Init().
class alignas(64) Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) noexcept = default;
    Application& operator=(Application&&) noexcept = default;

    [[nodiscard]] Result<void, FrameworkError> Init(
        const ApplicationConfig& cfg,
        const AppHooks& hooks = {}) noexcept;

    void Shutdown() noexcept;

    // Standard blocking main loop until Platform::ShouldQuit() becomes true.
    [[nodiscard]] Result<void, FrameworkError> Run() noexcept;

    // Single-frame execution entry point; also directly usable inside SDL_AppIterate() callbacks.
    [[nodiscard]] Result<void, FrameworkError> StepFrame() noexcept;

    // Deterministic headless stepper for CI verification, automated fuzzing, and replay verification.
    [[nodiscard]] Result<FrameAuditDigest, FrameworkError> StepHeadless(
        u32 tickCount = 1,
        u32 audioBlocksPerTick = 2) noexcept;

    [[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept;

    template <typename Self>
    [[nodiscard]] constexpr auto& Director(this Self&& self) noexcept { return self.m_director; }
    template <typename Self>
    [[nodiscard]] constexpr auto& World(this Self&& self) noexcept { return self.m_world; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Gameplay(this Self&& self) noexcept { return self.m_gameplay; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Assets(this Self&& self) noexcept { return self.m_assets; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Audio(this Self&& self) noexcept { return self.m_audio; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Renderer(this Self&& self) noexcept { return self.m_renderer; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Platform(this Self&& self) noexcept { return self.m_platform; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Window(this Self&& self) noexcept { return self.m_window; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Input(this Self&& self) noexcept { return self.m_input; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Time(this Self&& self) noexcept { return self.m_time; }
    template <typename Self>
    [[nodiscard]] constexpr auto& ActionBuffer(this Self&& self) noexcept { return self.m_actionBuffer; }
    template <typename Self>
    [[nodiscard]] constexpr auto& Debug(this Self&& self) noexcept { return self.m_debug; }

private:
    [[nodiscard]] constexpr SceneLoadContext MakeLoadContext() noexcept;
    [[nodiscard]] Result<void, FrameworkError> FailInit(
        FrameworkErrc code,
        SubSystemStage stage,
        u8 subErrc = 0) noexcept;
    [[nodiscard]] Result<void, FrameworkError> CommitSafeFrameBarrier() noexcept;
    [[nodiscard]] u32 ExecuteSimTicks() noexcept;
    void RouteCuesAndSyncAudio(u32 ticksExecuted) noexcept;
    [[nodiscard]] renderer::RenderList& ExtractRenderFrame(renderer::ViewportExtent viewportSize) noexcept;

    // Cache Line 0 (offset 0..63, 64B exact): Hot Runtime Coordinator State & Callbacks
    void* m_userAppState{nullptr};
    platform::EventTap m_devToolsTap{};
    AppHooks::ConsumeSimCueFn m_onConsumeSimCue{nullptr};
    AppHooks::PreShutdownFn m_onPreShutdown{nullptr};
    u64 m_lastRenderListAuditHash{0};
    assets::PumpBudget m_assetPumpBudget{};
    u32 m_frameIndex{0};
    bool m_initialized{false};
    bool m_headless{false};
    u16 m_reserved0{0};

    // Strict member declaration order guarantees forward construction and reverse destruction.
    platform::Platform m_platform{};
    platform::Window m_window{};
    input::InputPipeline m_input{};
    input::ActionBuffer m_actionBuffer{};
    time::TimeManager m_time{};
    renderer::Renderer m_renderer{};
    assets::AssetSystem m_assets{};
    audio::AudioSystem m_audio{};
    world::World m_world{};
    gameplay::GameplayPipeline m_gameplay{};
    debug::DebugSystem m_debug{};
    SceneDirector m_director{};
};

} // namespace en::framework

#pragma once

#include "framework/FrameworkTypes.h"
#include "framework/SceneDirector.h"

#include <array>

namespace en::framework {

// Top-level move-only RAII runtime coordinator for EngineN.
// Owns all core subsystems in strict dependency order and drives the 120 Hz fixed-step loop.
// Invariant: Zero heap allocations occur inside StepFrame() or StepHeadless() after Init() succeeds.
class Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) noexcept = default;
    Application& operator=(Application&&) noexcept = default;

    [[nodiscard]] Result<void, FrameworkError> Init(
        const ApplicationConfig& cfg,
        const AppHooks& hooks) noexcept;

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

private:
    [[nodiscard]] constexpr SceneLoadContext MakeLoadContext() noexcept;
    void HandleWindowEvents(platform::WindowFlags flags) noexcept;
    void RunSimTicks(u32& outTicksExecuted) noexcept;
    void RouteCuesAndSyncAudio(u32 ticksExecuted) noexcept;
    void SyncAudioListenerO1() noexcept;

    bool m_initialized{false};
    bool m_headless{false};
    u32 m_frameIndex{0};
    world::EntityId m_cachedCameraEntity{world::kNullEntity};
    u8 m_latchedFrozenMask{0};
    std::array<f32, time::kMaxTimeChannels> m_latchedChannelScales{
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f
    };
    u64 m_lastRenderListAuditHash{0};

    ApplicationConfig m_cfg{};
    AppHooks m_hooks{};

    // Strict member declaration order guarantees forward construction and reverse destruction.
    platform::Platform m_platform{};
    platform::Window m_window{};
    input::InputPipeline m_input{};
    input::ActionBuffer m_actionBuffer{};
    time::TimeManager m_time{};
    renderer::Renderer m_renderer{};
    renderer::RenderList m_headlessRenderList{};
    assets::AssetSystem m_assets{};
    audio::AudioSystem m_audio{};
    world::World m_world{};
    gameplay::GameplayPipeline m_gameplay{};
    SceneDirector m_director{};
};

} // namespace en::framework
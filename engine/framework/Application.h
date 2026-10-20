#pragma once

#include "core/CrashAndCoroutine.h"
#include "core/JobSystem.h"
#include "framework/SceneDirector.h"

namespace en {

struct ApplicationConfig {
    std::string_view appName{"EngineN 2026"};
    std::string_view orgName{"EngineN"};
    bool headless{false};
    u32 workerThreads{0};
    WindowConfig window{};
    RendererConfig renderer{};
    AssetSystemConfig assets{};
    AudioEngineConfig audio{};
    WorldConfig world{};
    GameplayPipelineConfig gameplay{};
};

// Top-level 5-Phase runtime coordinator owning all 10 subsystems in strict dependency order.
// Move-only RAII application host; drives both real-time StepFrame() and deterministic StepHeadless().
// Invariant: client code never invokes World::BeginTick or Renderer2D::Submit directly.
class alignas(64) Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] Result<void, FrameworkErrc> Init(
        const ApplicationConfig& cfg = {}, const AppHooks& hooks = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] Result<void, FrameworkErrc> Run() noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> StepFrame() noexcept;
    [[nodiscard]] Result<FrameAuditDigest, FrameworkErrc> StepHeadless(
        u32 tickCount = 1, u32 audioBlocksPerTick = 2) noexcept;

    [[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept;

    [[nodiscard]] constexpr SceneDirector& Director() noexcept { return m_director; }
    [[nodiscard]] constexpr World& WorldRef() noexcept { return m_world; }
    [[nodiscard]] constexpr GameplayPipeline& Gameplay() noexcept { return m_gameplay; }
    [[nodiscard]] constexpr AssetSystem& Assets() noexcept { return m_assets; }
    [[nodiscard]] constexpr AudioEngine& Audio() noexcept { return m_audio; }
    [[nodiscard]] constexpr Renderer2D& Renderer() noexcept { return m_renderer; }
    [[nodiscard]] constexpr Platform& PlatformRef() noexcept { return m_platform; }
    [[nodiscard]] constexpr Window& WindowRef() noexcept { return m_window; }
    [[nodiscard]] constexpr InputPipeline& Input() noexcept { return m_input; }
    [[nodiscard]] constexpr TimePipeline& Time() noexcept { return m_time; }
    [[nodiscard]] constexpr FramePacer& Pacer() noexcept { return m_framePacer; }
    [[nodiscard]] constexpr CoroutineScheduler& Coroutines() noexcept { return m_coroutines; }
    [[nodiscard]] constexpr ActionBuffer& Actions() noexcept { return m_actionBuffer; }
    [[nodiscard]] constexpr DebugSystem& Debug() noexcept { return m_debug; }
    [[nodiscard]] constexpr JobSystem& Jobs() noexcept { return m_jobs; }

private:
    [[nodiscard]] SceneLoadContext MakeLoadContext() noexcept;
    [[nodiscard]] Result<void, FrameworkErrc> CommitSafeFrameBarrier() noexcept;
    void ExecuteSimTick(const SimStepView& step) noexcept;
    void RouteCuesAndSyncAudio() noexcept;
    void ExtractAndRenderFrame() noexcept;

    AppHooks m_hooks{};
    u64 m_lastRenderListHash{0};
    u64 m_frameIndex{0};
    bool m_initialized{false};
    bool m_headless{false};

    JobSystem m_jobs{};
    Platform m_platform{};
    Window m_window{};
    InputPipeline m_input{};
    ActionBuffer m_actionBuffer{};
    TimePipeline m_time{};
    FramePacer m_framePacer{120};
    CoroutineScheduler m_coroutines{};
    Renderer2D m_renderer{};
    AssetSystem m_assets{};
    AudioEngine m_audio{};
    World m_world{};
    GameplayPipeline m_gameplay{};
    DebugSystem m_debug{};
    SceneDirector m_director{};
};

} // namespace en

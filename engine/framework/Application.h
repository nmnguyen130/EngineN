#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "core/EventBus.h"
#include "core/Jobs.h"
#include "debug/DebugSystem.h"
#include "framework/EnginePipeline.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplaySystem.h"
#include "input/ActionBuffer.h"
#include "input/InputSystem.h"
#include "physics/PhysicsSystem.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"
#include "time/Clock.h"
#include "time/TimeSystem.h"
#include "vfx/VfxSystem.h"
#include "world/World.h"

namespace en {

struct AppConfig {
    std::string_view title{"EngineN"};
    Viewport         viewport{1280, 720};
    f32              pixelsPerMeter{48.0f};
    u32              workerThreads{0U}; // 0U = automatic hardware_concurrency() detection
    AssetBankConfig  assets{};
    AudioConfig      audio{};
    WorldConfig      world{};
    PhysicsConfig    physics{};
    GameplayConfig   gameplay{};
    VfxConfig        vfx{};
    DebugConfig      debug{};
    bool             headless{true};
    bool             enableDebug{true};
};

// Top-level Metroidvania engine lifecycle owner managing subsystem initialization and OS frame pacing.
// Move-only owner of an Arena-allocated Runtime state; delegates simulation and presentation execution to EnginePipeline.
// Invariant: sizeof(Application) == 48.
class Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&& other) noexcept;
    Application& operator=(Application&& other) noexcept;

    [[nodiscard]] Result<void, FwErrc> Init(
        const AppConfig& cfg = {},
        const AppHooks& hooks = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool Ready() const noexcept { return m_ready != 0U; }
    [[nodiscard]] constexpr bool Running() const noexcept { return m_running != 0U; }
    constexpr void Quit() noexcept { m_running = 0U; }

    [[nodiscard]] Result<void, FwErrc> Run(u32 targetFps = 120U) noexcept;
    [[nodiscard]] Result<void, FwErrc> Step(u64 dtNs = kFixedDtNs) noexcept;
    [[nodiscard]] Result<void, FwErrc> StepTicks(u32 ticks) noexcept;

    [[nodiscard]] constexpr u64 FrameCount() const noexcept { return m_frame; }
    [[nodiscard]] constexpr u64 TickCount() const noexcept { return m_ticks; }
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] Platform&       GetPlatform() noexcept;
    [[nodiscard]] Window&         GetWindow() noexcept;
    [[nodiscard]] Renderer&       GetRenderer() noexcept;
    [[nodiscard]] JobSystem&      GetJobs() noexcept;
    [[nodiscard]] EventBus&       GetEvents() noexcept;
    [[nodiscard]] SceneDirector&  GetScenes() noexcept;
    [[nodiscard]] EnginePipeline& GetPipeline() noexcept;
    [[nodiscard]] World&          GetWorld() noexcept;
    [[nodiscard]] PhysicsSystem&  GetPhysics() noexcept;
    [[nodiscard]] GameplaySystem& GetGameplay() noexcept;
    [[nodiscard]] VfxSystem&      GetVfx() noexcept;
    [[nodiscard]] AssetBank&      GetAssets() noexcept;
    [[nodiscard]] AudioSystem&    GetAudio() noexcept;
    [[nodiscard]] DebugSystem&    GetDebug() noexcept;
    [[nodiscard]] TimeSystem&     GetTime() noexcept;
    [[nodiscard]] InputSystem&    GetInput() noexcept;
    [[nodiscard]] ActionBuffer&   GetActions() noexcept;
    [[nodiscard]] RenderList&     GetRenderList() noexcept;

    [[nodiscard]] const Platform&       GetPlatform() const noexcept;
    [[nodiscard]] const Window&         GetWindow() const noexcept;
    [[nodiscard]] const Renderer&       GetRenderer() const noexcept;
    [[nodiscard]] const JobSystem&      GetJobs() const noexcept;
    [[nodiscard]] const EventBus&       GetEvents() const noexcept;
    [[nodiscard]] const SceneDirector&  GetScenes() const noexcept;
    [[nodiscard]] const EnginePipeline& GetPipeline() const noexcept;
    [[nodiscard]] const World&          GetWorld() const noexcept;
    [[nodiscard]] const PhysicsSystem&  GetPhysics() const noexcept;
    [[nodiscard]] const GameplaySystem& GetGameplay() const noexcept;
    [[nodiscard]] const VfxSystem&      GetVfx() const noexcept;
    [[nodiscard]] const AssetBank&      GetAssets() const noexcept;
    [[nodiscard]] const AudioSystem&    GetAudio() const noexcept;
    [[nodiscard]] const DebugSystem&    GetDebug() const noexcept;
    [[nodiscard]] const TimeSystem&     GetTime() const noexcept;
    [[nodiscard]] const InputSystem&    GetInput() const noexcept;
    [[nodiscard]] const ActionBuffer&   GetActions() const noexcept;
    [[nodiscard]] const RenderList&     GetRenderList() const noexcept;

private:
    struct Runtime;

    [[nodiscard]] LoadCtx MakeLoadCtx() const noexcept;
    void ApplyDebugDirectives(const LoopDirectives& dir) noexcept;

    Arena    m_arena{};
    Runtime* m_rt{nullptr};
    u64      m_frame{0};
    u64      m_ticks{0};
    f32      m_ppm{48.0f};
    u8       m_ready{0};
    u8       m_running{0};
    u8       m_headless{1};
    u8       m_debug{1};
};
static_assert(sizeof(Application) == 48);

} // namespace en

#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "core/Jobs.h"
#include "debug/DebugSystem.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "input/InputSystem.h"
#include "physics/PhysicsSystem.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"
#include "time/Clock.h"
#include "time/TimeSystem.h"
#include "world/World.h"

namespace en {

struct alignas(32) AppConfig {
    AssetBankConfig   assets{};
    AudioSystemConfig audio{};
    DebugConfig       debug{};
    WorldConfig       world{};
    Viewport          viewport{1280, 720};
    f32               ppm{48.f};
    u16               maxManifolds{1024};
    u16               maxCues{64};
    u8                velIters{8};
    u8                sleepTicks{60};
    u8                headless{1};
    u8                debugOn{1};
};
static_assert(sizeof(AppConfig) <= 96);

// Owns all subsystems. One Step() drives the whole frame.
class alignas(64) Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) noexcept;
    Application& operator=(Application&&) noexcept;

    [[nodiscard]] Result<void, FwError> Init(const AppConfig& cfg = {}, const Hooks& hooks = {}) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool Ready() const noexcept { return m_ok != 0; }
    [[nodiscard]] constexpr bool Running() const noexcept { return m_run != 0; }
    constexpr void Quit() noexcept { m_run = 0; }

    [[nodiscard]] Result<void, FwError> Run(u32 fps = 120) noexcept;
    [[nodiscard]] Result<void, FwError> Step(u64 dtNs = kFixedDtNs) noexcept;
    [[nodiscard]] Result<void, FwError> StepN(u32 ticks) noexcept;

    [[nodiscard]] u64 Frame() const noexcept { return m_frame; }
    [[nodiscard]] u64 Ticks() const noexcept { return m_ticks; }
    [[nodiscard]] u64 Hash() const noexcept;

    [[nodiscard]] Platform&         Plat() noexcept;
    [[nodiscard]] Window&           Win() noexcept;
    [[nodiscard]] Renderer&         Gfx() noexcept;
    [[nodiscard]] JobSystem&        Jobs() noexcept;
    [[nodiscard]] SceneDirector&    Dir() noexcept;
    [[nodiscard]] World&            Wld() noexcept;
    [[nodiscard]] PhysicsSystem&    Phys() noexcept;
    [[nodiscard]] GameplayPipeline& Game() noexcept;
    [[nodiscard]] AssetBank&        Assets() noexcept;
    [[nodiscard]] AudioSystem&      Sfx() noexcept;
    [[nodiscard]] DebugSystem&      Dbg() noexcept;
    [[nodiscard]] TimeSystem&       Clock() noexcept;
    [[nodiscard]] InputSystem&      In() noexcept;
    [[nodiscard]] ActionBuffer&     Acts() noexcept;
    [[nodiscard]] RenderList&       List() noexcept;

    [[nodiscard]] const Platform&         Plat() const noexcept;
    [[nodiscard]] const Window&           Win() const noexcept;
    [[nodiscard]] const Renderer&         Gfx() const noexcept;
    [[nodiscard]] const JobSystem&        Jobs() const noexcept;
    [[nodiscard]] const SceneDirector&    Dir() const noexcept;
    [[nodiscard]] const World&            Wld() const noexcept;
    [[nodiscard]] const PhysicsSystem&    Phys() const noexcept;
    [[nodiscard]] const GameplayPipeline& Game() const noexcept;
    [[nodiscard]] const AssetBank&        Assets() const noexcept;
    [[nodiscard]] const AudioSystem&      Sfx() const noexcept;
    [[nodiscard]] const DebugSystem&      Dbg() const noexcept;
    [[nodiscard]] const TimeSystem&       Clock() const noexcept;
    [[nodiscard]] const InputSystem&      In() const noexcept;
    [[nodiscard]] const ActionBuffer&     Acts() const noexcept;
    [[nodiscard]] const RenderList&       List() const noexcept;

private:
    struct RT;

    [[nodiscard]] LoadCtx MakeLoad() const noexcept;
    void HandleCues(SimCtx& ctx, const SimStepView& step) noexcept;
    void Draw() noexcept;

    Arena m_arena{};
    RT*   m_rt{};
    void* m_user{};
    u64   m_frame{0};
    u64   m_ticks{0};
    u16   m_ppmQ4{768};
    u8    m_ok{0};
    u8    m_run{0};
    u8    m_headless{1};
    u8    m_debug{1};
    u8    m_freeze{0};
};
static_assert(sizeof(Application) <= 64);

} // namespace en

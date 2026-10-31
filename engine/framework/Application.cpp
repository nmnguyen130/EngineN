#include "framework/Application.h"

namespace en {

struct Application::Runtime {
    TimeSystem     time{};
    InputSystem    input{};
    ActionBuffer   actions{};
    AssetBank      assets{};
    AudioSystem    audio{};
    World          world{};
    PhysicsSystem  physics{};
    GameplaySystem gameplay{};
    VfxSystem      vfx{};
    SceneDirector  scenes{};
    EnginePipeline pipeline{};
    DebugSystem    debug{};
    FramePacer     pacer{};
    JobSystem      jobs{};
    Platform       platform{};
    Window         window{};
    Renderer       renderer{};
    RenderList     renderList{};
    AppHooks       hooks{};
    Viewport       viewport{1280, 720};
    bool           focusPaused{false};
};

Application::Application(Application&& other) noexcept {
    *this = std::move(other);
}

Application& Application::operator=(Application&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arena    = std::move(other.m_arena);
        m_rt       = std::exchange(other.m_rt, nullptr);
        m_frame    = std::exchange(other.m_frame, 0ULL);
        m_ticks    = std::exchange(other.m_ticks, 0ULL);
        m_ppm      = other.m_ppm;
        m_ready    = std::exchange(other.m_ready, 0U);
        m_running  = std::exchange(other.m_running, 0U);
        m_headless = other.m_headless;
        m_debug    = other.m_debug;
    }
    return *this;
}

LoadCtx Application::MakeLoadCtx() const noexcept {
    if (!m_rt) return {};
    return LoadCtx{
        .world           = &m_rt->world,
        .physics         = &m_rt->physics,
        .gameplay        = &m_rt->gameplay,
        .assets          = &m_rt->assets,
        .audio           = &m_rt->audio,
        .scenes          = &m_rt->scenes,
        .jobs            = &m_rt->jobs,
        .userData        = m_rt->hooks.userData,
        .frame           = static_cast<u32>(m_frame),
        .roomScope       = m_rt->scenes.RoomScope(),
        .persistentScope = m_rt->scenes.PersistentScope(),
    };
}

Result<void, FwErrc> Application::Init(const AppConfig& cfg, const AppHooks& hooks) noexcept {
    Shutdown();
    if (!cfg.viewport.Valid() || cfg.pixelsPerMeter <= 0.0f) {
        return Err(FwErrc::InvalidConfig);
    }

    if (!m_arena.Init(sizeof(Runtime) + 1024U)) {
        return Err(FwErrc::InvalidConfig);
    }
    u8* raw = m_arena.AllocBytes(sizeof(Runtime), alignof(Runtime));
    if (!raw) {
        m_arena.Shutdown();
        return Err(FwErrc::InvalidConfig);
    }
    m_rt = std::construct_at(reinterpret_cast<Runtime*>(raw));

    m_rt->hooks    = hooks;
    m_rt->viewport = cfg.viewport;
    m_ppm          = cfg.pixelsPerMeter;
    m_headless     = cfg.headless ? 1U : 0U;
    m_debug        = cfg.enableDebug ? 1U : 0U;
    m_frame        = 0ULL;
    m_ticks        = 0ULL;

    const auto fail = [&](FwErrc err) noexcept -> Result<void, FwErrc> {
        Shutdown();
        return Err(err);
    };

    // 0U automatically queries std::thread::hardware_concurrency() - 1 inside JobSystem::Init.
    (void)m_rt->jobs.Init(cfg.workerThreads);
    if (!m_rt->platform.Init(cfg.title, "EngineN", cfg.headless).has_value()) {
        return fail(FwErrc::PlatformFailed);
    }
    if (!m_rt->window.Create({.title = cfg.title, .size = {cfg.viewport.w, cfg.viewport.h}}, cfg.headless).has_value()) {
        return fail(FwErrc::WindowFailed);
    }
    if (!m_rt->renderer.Init(m_rt->window, {.headless = cfg.headless}).has_value()) {
        return fail(FwErrc::RendererFailed);
    }
    if (!m_rt->assets.Init(cfg.assets).has_value()) {
        return fail(FwErrc::AssetsFailed);
    }
    m_rt->assets.SetGpuDestroy(
        [](TextureHandle t, void* ctx) noexcept {
            if (t != kWhiteTexture) static_cast<Renderer*>(ctx)->DestroyTexture(t);
        },
        &m_rt->renderer);

    AudioConfig audioCfg = cfg.audio;
    audioCfg.headless    = cfg.headless;
    if (!m_rt->audio.Init(audioCfg).has_value()) {
        return fail(FwErrc::AudioFailed);
    }
    if (!m_rt->world.Init(cfg.world).has_value()) {
        return fail(FwErrc::WorldFailed);
    }
    if (hooks.onRegisterComponents) {
        if (auto r = hooks.onRegisterComponents(m_rt->world, hooks.userData); !r) {
            return fail(r.error());
        }
    }
    if (!m_rt->physics.Init(m_rt->world, cfg.physics).has_value()) {
        return fail(FwErrc::PhysicsFailed);
    }
    if (!m_rt->gameplay.Init(m_rt->world, cfg.gameplay, hooks.gameplay).has_value()) {
        return fail(FwErrc::GameplayFailed);
    }
    if (!m_rt->vfx.Init(cfg.vfx).has_value()) {
        return fail(FwErrc::GameplayFailed);
    }
    if (m_debug != 0U) {
        if (!m_rt->debug.Init(cfg.debug).has_value()) {
            return fail(FwErrc::DebugFailed);
        }
    }
    if (auto r = m_rt->scenes.Init(m_rt->assets); !r) {
        return fail(r.error());
    }

    m_rt->pipeline.InitBuiltinStages(PipelineServices{
        .time           = &m_rt->time,
        .platform       = &m_rt->platform,
        .renderer       = &m_rt->renderer,
        .debug          = (m_debug != 0U) ? &m_rt->debug : nullptr,
        .vfx            = &m_rt->vfx,
        .jobs           = &m_rt->jobs,
        .hooks          = &m_rt->hooks,
        .viewport       = &m_rt->viewport,
        .pixelsPerMeter = m_ppm,
        .headless       = m_headless,
        .enableDebug    = m_debug,
    });

    if (hooks.onBoot) {
        LoadCtx bootCtx = MakeLoadCtx();
        if (auto r = hooks.onBoot(bootCtx); !r) {
            return fail(r.error());
        }
    }

    m_ready   = 1U;
    m_running = 1U;
    return {};
}

void Application::Shutdown() noexcept {
    if (!m_rt) {
        m_arena.Shutdown();
        m_ready   = 0U;
        m_running = 0U;
        return;
    }
    if (m_ready != 0U) {
        LoadCtx load = MakeLoadCtx();
        if (m_rt->hooks.onShutdown) m_rt->hooks.onShutdown(load);
        m_rt->pipeline.Clear();
        m_rt->scenes.Shutdown(m_rt->assets, &load);
        if (m_debug != 0U) m_rt->debug.Shutdown();
        m_rt->vfx.Shutdown();
        m_rt->gameplay.Shutdown();
        m_rt->physics.Shutdown();
        m_rt->world.Shutdown();
        m_rt->audio.Shutdown();
        m_rt->assets.Shutdown();
        m_rt->renderer.Shutdown();
        m_rt->window.Destroy();
        m_rt->platform.Shutdown();
        m_rt->jobs.Shutdown();
    }
    std::destroy_at(m_rt);
    m_rt = nullptr;
    m_arena.Shutdown();
    m_ready   = 0U;
    m_running = 0U;
}

Result<void, FwErrc> Application::Run(u32 targetFps) noexcept {
    if (!m_rt || m_ready == 0U) return Err(FwErrc::InvalidConfig);
    if (m_headless != 0U) return StepTicks(120U);

    m_rt->pacer.SetTargetFps(targetFps);
    u64 prevNs = Clock::NowNs();

    while (m_running != 0U && !m_rt->platform.ShouldQuit()) {
        m_rt->pacer.BeginFrame();
        const u64 nowNs = Clock::NowNs();
        const u64 dtNs  = (nowNs > prevNs) ? (nowNs - prevNs) : kFixedDtNs;
        prevNs          = nowNs;

        WindowFlags winFlags = WindowFlags::None;
        {
            ScopeTimer<Clock> pollTimer(m_rt->debug.Profiler(), ProfileScope::Poll);
            winFlags = m_rt->platform.Pump(m_rt->window, m_rt->input);
        }
        if (HasFlag(winFlags, WindowFlags::CloseRequested)) {
            Quit();
            break;
        }
        if (HasFlag(winFlags, WindowFlags::Resized)) {
            const WindowSize s = m_rt->window.State().pixelSize;
            if (s.IsValid()) m_rt->viewport = Viewport{s.w, s.h};
        }
        if (HasFlag(winFlags, WindowFlags::Minimized | WindowFlags::FocusLost)) {
            if (!m_rt->focusPaused) {
                m_rt->focusPaused = true;
                m_rt->time.SetPaused(true);
                m_rt->audio.SetFocusMuted(true);
            }
        } else if (HasFlag(winFlags, WindowFlags::Restored | WindowFlags::FocusGained)) {
            if (m_rt->focusPaused) {
                m_rt->focusPaused = false;
                m_rt->time.OnWindowRefocus();
                m_rt->time.SetPaused(false);
                m_rt->audio.SetFocusMuted(false);
                prevNs = Clock::NowNs();
            }
        }

        if (auto r = Step(dtNs); !r) return r;
        m_rt->pacer.EndFrame();
    }
    return {};
}

void Application::ApplyDebugDirectives(const LoopDirectives& dir) noexcept {
    if (dir.TogglePause()) m_rt->time.SetPaused(!m_rt->time.IsPaused());
    if (dir.ForcePause()) m_rt->time.SetPaused(true);
    if (dir.stepTicks > 0U) m_rt->time.Step(dir.stepTicks);
    if (dir.timeScale >= 0.0f) {
        m_rt->time.SetScale(static_cast<TimeChannel>(dir.timeChannel), dir.timeScale);
    }
}

Result<void, FwErrc> Application::Step(u64 dtNs) noexcept {
    if (!m_rt || m_ready == 0U) return Err(FwErrc::InvalidConfig);

    ++m_frame;
    m_rt->assets.AdvanceFrame(static_cast<u32>(m_frame));

    if (m_debug != 0U) {
        FrameCtx fc{.world = &m_rt->world, .physics = &m_rt->physics, .viewport = m_rt->viewport};
        ApplyDebugDirectives(m_rt->debug.BeginFrame(fc));
    }

    m_rt->time.BeginFrame(dtNs);

    LoadCtx load = MakeLoadCtx();
    if (auto r = m_rt->scenes.Tick(load); !r) return Err(r.error());
    m_rt->input.SetActionMask(m_rt->scenes.InputMask());

    {
        ScopeTimer<Clock> simTimer(m_rt->debug.Profiler(), ProfileScope::Sim);
        while (const TickStep step = m_rt->time.PollTick()) {
            if (m_rt->scenes.IsFrozen()) continue;

            const TickInput tin = m_rt->input.PollTick(step.tick);
            SimCtx sc{
                .world    = &m_rt->world,
                .physics  = &m_rt->physics,
                .gameplay = &m_rt->gameplay,
                .assets   = &m_rt->assets,
                .audio    = &m_rt->audio,
                .actions  = &m_rt->actions,
                .input    = &tin,
                .scenes   = &m_rt->scenes,
            };

            m_rt->pipeline.ExecuteSimTick(sc, step);
            ++m_ticks;

            if (m_debug != 0U) {
                m_rt->debug.EndTick(m_rt->world, step.tick, &tin, &m_rt->actions);
            }
        }
    }

    if (!m_rt->focusPaused) {
        const RenderStep& rv = m_rt->time.RenderView();
        DrawCtx dc{
            .world      = &m_rt->world,
            .physics    = &m_rt->physics,
            .gameplay   = &m_rt->gameplay,
            .assets     = &m_rt->assets,
            .list       = &m_rt->renderList,
            .input      = &m_rt->input.Frame(),
            .renderStep = &rv,
            .scenes     = &m_rt->scenes,
        };
        m_rt->pipeline.ExecuteRenderFrame(dc);
    }

    m_rt->audio.Pump(1U);
    return {};
}

Result<void, FwErrc> Application::StepTicks(u32 ticks) noexcept {
    for (u32 i = 0U; i < ticks; ++i) {
        if (auto r = Step(kFixedDtNs); !r) return r;
    }
    return {};
}

u64 Application::ComputeAuditHash() const noexcept {
    if (!m_rt) return 0ULL;
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_ticks);
    h     = HashCombine(h, m_rt->world.ComputeAuditHash());
    h     = HashCombine(h, m_rt->physics.ComputeAuditHash(m_rt->world));
    h     = HashCombine(h, m_rt->gameplay.ComputeAuditHash(m_rt->world));
    h     = HashCombine(h, m_rt->audio.ComputeAuditHash());
    h     = HashCombine(h, m_rt->scenes.ComputeAuditHash());
    h     = HashCombine(h, m_rt->pipeline.ComputeAuditHash());
    return h;
}

Platform&       Application::GetPlatform() noexcept       { return m_rt->platform; }
Window&         Application::GetWindow() noexcept         { return m_rt->window; }
Renderer&       Application::GetRenderer() noexcept       { return m_rt->renderer; }
JobSystem&      Application::GetJobs() noexcept           { return m_rt->jobs; }
SceneDirector&  Application::GetScenes() noexcept         { return m_rt->scenes; }
EnginePipeline& Application::GetPipeline() noexcept       { return m_rt->pipeline; }
World&          Application::GetWorld() noexcept          { return m_rt->world; }
PhysicsSystem&  Application::GetPhysics() noexcept        { return m_rt->physics; }
GameplaySystem& Application::GetGameplay() noexcept       { return m_rt->gameplay; }
VfxSystem&      Application::GetVfx() noexcept            { return m_rt->vfx; }
AssetBank&      Application::GetAssets() noexcept         { return m_rt->assets; }
AudioSystem&    Application::GetAudio() noexcept          { return m_rt->audio; }
DebugSystem&    Application::GetDebug() noexcept          { return m_rt->debug; }
TimeSystem&     Application::GetTime() noexcept           { return m_rt->time; }
InputSystem&    Application::GetInput() noexcept          { return m_rt->input; }
ActionBuffer&   Application::GetActions() noexcept        { return m_rt->actions; }
RenderList&     Application::GetRenderList() noexcept     { return m_rt->renderList; }

const Platform&       Application::GetPlatform() const noexcept   { return m_rt->platform; }
const Window&         Application::GetWindow() const noexcept     { return m_rt->window; }
const Renderer&       Application::GetRenderer() const noexcept   { return m_rt->renderer; }
const JobSystem&      Application::GetJobs() const noexcept       { return m_rt->jobs; }
const SceneDirector&  Application::GetScenes() const noexcept     { return m_rt->scenes; }
const EnginePipeline& Application::GetPipeline() const noexcept   { return m_rt->pipeline; }
const World&          Application::GetWorld() const noexcept      { return m_rt->world; }
const PhysicsSystem&  Application::GetPhysics() const noexcept    { return m_rt->physics; }
const GameplaySystem& Application::GetGameplay() const noexcept   { return m_rt->gameplay; }
const VfxSystem&      Application::GetVfx() const noexcept        { return m_rt->vfx; }
const AssetBank&      Application::GetAssets() const noexcept     { return m_rt->assets; }
const AudioSystem&    Application::GetAudio() const noexcept      { return m_rt->audio; }
const DebugSystem&    Application::GetDebug() const noexcept      { return m_rt->debug; }
const TimeSystem&     Application::GetTime() const noexcept       { return m_rt->time; }
const InputSystem&    Application::GetInput() const noexcept      { return m_rt->input; }
const ActionBuffer&   Application::GetActions() const noexcept    { return m_rt->actions; }
const RenderList&     Application::GetRenderList() const noexcept { return m_rt->renderList; }

} // namespace en

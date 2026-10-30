#include "framework/Application.h"

#include "world/WorldSystems.h"

namespace en {

struct Application::Runtime {
    TimeSystem                    time{};
    InputSystem                   input{};
    ActionBuffer                  actions{};
    AssetBank                     assets{};
    AudioSystem                   audio{};
    World                         world{};
    PhysicsSystem                 physics{};
    GameplaySystem                gameplay{};
    SceneDirector                 scenes{};
    DebugSystem                   debug{};
    FramePacer                    pacer{};
    JobSystem                     jobs{};
    Platform                      platform{};
    Window                        window{};
    Renderer                      renderer{};
    RenderList                    renderList{};
    AppHooks                      hooks{};
    Viewport                      viewport{1280, 720};
    std::array<AudioTrigger, 64>  audioTriggers{};
    bool                          focusPaused{false};
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

    if (!cfg.headless) {
        (void)m_rt->jobs.Init(1U);
    }
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
    if (m_debug != 0U) {
        if (!m_rt->debug.Init(cfg.debug).has_value()) {
            return fail(FwErrc::DebugFailed);
        }
    }
    if (auto r = m_rt->scenes.Init(m_rt->assets); !r) {
        return fail(r.error());
    }
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
        m_rt->scenes.Shutdown(m_rt->assets, &load);
        if (m_debug != 0U) m_rt->debug.Shutdown();
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

        const WindowFlags winFlags = m_rt->platform.Pump(m_rt->window, m_rt->input);
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
    if (dir.TogglePause()) {
        m_rt->time.SetPaused(!m_rt->time.IsPaused());
    }
    if (dir.ForcePause()) {
        m_rt->time.SetPaused(true);
    }
    if (dir.stepTicks > 0U) {
        m_rt->time.Step(dir.stepTicks);
    }
    if (dir.timeScale >= 0.0f) {
        m_rt->time.SetScale(static_cast<TimeChannel>(dir.timeChannel), dir.timeScale);
    }
}

void Application::RouteGameplayCues(SimCtx& ctx, const TickStep& step) noexcept {
    u32        trigCount = 0U;
    const auto cues      = m_rt->gameplay.DrainCues();

    CameraFollow2D* cam = nullptr;
    if ((m_rt->world.RegisteredPoolsMask() & MakeComponentMask<CameraFollow2D>()) != 0ULL) {
        auto& pool = m_rt->world.GetPool<CameraFollow2D>();
        if (pool.Size() > 0U) cam = &pool.ComponentAtDense(0U);
    }

    for (const GameplayCue& c : cues) {
        switch (c.kind) {
            case CueKind::Audio:
                if (trigCount < m_rt->audioTriggers.size()) {
                    Vec2 pos = c.position;
                    if (pos == Vec2{} && c.emitter.IsValid() && m_rt->world.IsAlive(c.emitter)) {
                        const WorldTransform2D& wt = m_rt->world.GetWorldByIndex(c.emitter.index);
                        pos = {wt.x, wt.y};
                    }
                    m_rt->audioTriggers[trigCount++] = AudioTrigger{
                        .cueId     = c.asset,
                        .position  = pos,
                        .emitter   = c.emitter.Packed(),
                        .gainScale = c.magnitude,
                    };
                }
                break;
            case CueKind::CameraShake:
                if (cam) cam->AddTrauma(c.magnitude);
                break;
            case CueKind::Hitstop:
                m_rt->time.Hitstop(c.channel, c.durationTicks);
                break;
            case CueKind::Rumble:
                if (m_headless == 0U && m_rt->platform.IsInitialized()) {
                    const u16 freq = static_cast<u16>(std::clamp(c.magnitude * 65535.0f, 0.0f, 65535.0f));
                    (void)m_rt->platform.RumbleGamepad(freq, freq, static_cast<u32>(c.durationTicks) * 8U);
                }
                break;
            default:
                break;
        }
        if (m_rt->hooks.onCue) m_rt->hooks.onCue(c, ctx, m_rt->hooks.userData);
    }

    std::array<f32, kGameplayChannels> scales{};
    for (u8 i = 0U; i < kGameplayChannels; ++i) {
        scales[i] = step.Scale(static_cast<TimeChannel>(i));
    }

    AudioUpdateDesc ad{
        .triggers      = {m_rt->audioTriggers.data(), trigCount},
        .assets        = &m_rt->assets,
        .occlusionFn   = [](Vec2 l, Vec2 e, void* p) noexcept -> bool {
            const auto* phys = static_cast<const PhysicsSystem*>(p);
            if (!phys) return false;
            const Vec2 d   = e - l;
            const f32  len = d.Length();
            return len > 1e-4f && static_cast<bool>(phys->Raycast(l, d, len, 0x0001U));
        },
        .occlusionCtx  = &m_rt->physics,
        .channelScales = scales.data(),
        .listenerPos   = cam ? cam->anchor : Vec2{},
        .tick          = step.tick,
        .frozenMask    = static_cast<u8>(step.frozenMask | (m_rt->scenes.IsFrozen() ? kGameplayMask : 0U)),
    };
    m_rt->audio.Update(ad);
}

void Application::RenderFrame() noexcept {
    const RenderStep& rv = m_rt->time.RenderView();
    m_rt->renderList.Reset(m_rt->viewport);
    WorldSystems::ExtractRender(m_rt->world, m_rt->assets, rv, m_rt->renderList, m_ppm);

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
    m_rt->scenes.DrawStates(dc);
    m_rt->scenes.DrawWipe(m_rt->renderList, m_rt->viewport);

    if (m_debug != 0U) {
        std::array<AudioEmitterView, 16> emitters{};
        const u32 emCount = m_rt->audio.ExtractEmitters(emitters);
        FrameCtx fc{
            .world    = &m_rt->world,
            .physics  = &m_rt->physics,
            .emitters = {emitters.data(), emCount},
            .viewport = m_rt->viewport,
            .tick     = static_cast<u32>(m_rt->time.Stats().totalTicks),
        };
        m_rt->debug.Emit(fc, m_rt->renderList);
    }
    m_rt->renderer.Submit(m_rt->renderList);
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

    while (const TickStep step = m_rt->time.PollTick()) {
        if (m_rt->scenes.IsFrozen()) continue;

        const TickInput tin = m_rt->input.PollTick(step.tick);
        m_rt->world.BeginTick(step);

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

        m_rt->scenes.TickStates(sc, step);
        WorldSystems::StepVelocities(m_rt->world, step);
        m_rt->gameplay.Step(m_rt->world, m_rt->physics, step, tin, m_rt->actions);
        WorldSystems::StepVisualsAndLifetimes(m_rt->world, step);
        RouteGameplayCues(sc, step);

        m_rt->world.EndTick(step);
        ++m_ticks;
        if (m_debug != 0U) {
            m_rt->debug.EndTick(m_rt->world, step.tick, &tin, &m_rt->actions);
        }
    }

    if (!m_rt->focusPaused) {
        RenderFrame();
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
    return h;
}

Platform&       Application::GetPlatform() noexcept       { return m_rt->platform; }
Window&         Application::GetWindow() noexcept         { return m_rt->window; }
Renderer&       Application::GetRenderer() noexcept       { return m_rt->renderer; }
JobSystem&      Application::GetJobs() noexcept           { return m_rt->jobs; }
SceneDirector&  Application::GetScenes() noexcept         { return m_rt->scenes; }
World&          Application::GetWorld() noexcept          { return m_rt->world; }
PhysicsSystem&  Application::GetPhysics() noexcept        { return m_rt->physics; }
GameplaySystem& Application::GetGameplay() noexcept       { return m_rt->gameplay; }
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
const World&          Application::GetWorld() const noexcept      { return m_rt->world; }
const PhysicsSystem&  Application::GetPhysics() const noexcept    { return m_rt->physics; }
const GameplaySystem& Application::GetGameplay() const noexcept   { return m_rt->gameplay; }
const AssetBank&      Application::GetAssets() const noexcept     { return m_rt->assets; }
const AudioSystem&    Application::GetAudio() const noexcept      { return m_rt->audio; }
const DebugSystem&    Application::GetDebug() const noexcept      { return m_rt->debug; }
const TimeSystem&     Application::GetTime() const noexcept       { return m_rt->time; }
const InputSystem&    Application::GetInput() const noexcept      { return m_rt->input; }
const ActionBuffer&   Application::GetActions() const noexcept    { return m_rt->actions; }
const RenderList&     Application::GetRenderList() const noexcept { return m_rt->renderList; }

} // namespace en

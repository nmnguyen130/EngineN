#include "framework/Application.h"

#include "time/Clock.h"
#include "world/WorldSystems.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <utility>

namespace en {

struct alignas(64) Application::RT {
    TimeSystem       time{};
    InputSystem      input{};
    ActionBuffer     acts{};
    AssetBank        assets{};
    AudioSystem      audio{};
    World            world{};
    PhysicsSystem    phys{};
    GameplayPipeline game{};
    SceneDirector    dir{};
    DebugSystem      debug{};
    FramePacer       pacer{};
    JobSystem        jobs{};
    Hooks            hooks{};
    Viewport         vp{1280, 720};
    u8               focusOff{0};
    u8               hasGfx{0};
    std::array<AudioTriggerReq, 64> cues{};
    Window           win{};
    Platform         plat{};
    RenderList       list{};
    union {
        u8       gfxPad{};
        Renderer gfx;
    };

    RT() noexcept : gfxPad{0} {}
    ~RT() noexcept { if (hasGfx) std::destroy_at(&gfx); }
};

Application::Application(Application&& o) noexcept { *this = std::move(o); }

Application& Application::operator=(Application&& o) noexcept {
    if (this == &o) return *this;
    Shutdown();
    m_arena = std::move(o.m_arena);
    m_rt = std::exchange(o.m_rt, nullptr);
    m_user = std::exchange(o.m_user, nullptr);
    m_frame = std::exchange(o.m_frame, 0ULL);
    m_ticks = std::exchange(o.m_ticks, 0ULL);
    m_ppmQ4 = o.m_ppmQ4;
    m_ok = std::exchange(o.m_ok, 0);
    m_run = std::exchange(o.m_run, 0);
    m_headless = o.m_headless;
    m_debug = o.m_debug;
    m_freeze = std::exchange(o.m_freeze, 0);
    return *this;
}

LoadCtx Application::MakeLoad() const noexcept {
    LoadCtx c{};
    if (!m_rt) return c;
    c.world = &m_rt->world;
    c.phys = &m_rt->phys;
    c.game = &m_rt->game;
    c.assets = &m_rt->assets;
    c.audio = &m_rt->audio;
    c.dir = &m_rt->dir;
    c.user = m_user;
    c.frame = u32(m_frame);
    c.roomScope = m_rt->dir.RoomScope();
    c.keepScope = m_rt->dir.KeepScope();
    return c;
}

Result<void, FwError> Application::Init(const AppConfig& cfg, const Hooks& hooks) noexcept {
    Shutdown();
    if (!cfg.viewport.Valid() || cfg.ppm <= 0.f) {
        return Err(FwError{FwErr::BadConfig, FwStage::None, 0});
    }

    const bool headless = cfg.headless != 0;
    const usize bytes = headless ? offsetof(RT, gfx) + 64 : sizeof(RT);
    if (!m_arena.Init(bytes)) {
        return Err(FwError{FwErr::BadConfig, FwStage::None, 0});
    }
    auto span = m_arena.Alloc<RT>(1);
    if (span.empty()) {
        m_arena.Shutdown();
        return Err(FwError{FwErr::BadConfig, FwStage::None, 0});
    }
    std::destroy_at(span.data());
    m_rt = ::new (span.data()) RT{};

    m_rt->hooks = hooks;
    m_rt->vp = cfg.viewport;
    m_user = hooks.user;
    m_ppmQ4 = u16(std::clamp(cfg.ppm * 16.f, 16.f, 65535.f));
    m_headless = cfg.headless;
    m_debug = cfg.debugOn;
    m_freeze = 0;
    m_frame = 0;
    m_ticks = 0;

    auto fail = [&](FwErr e, FwStage s, u16 sub = 0) -> Result<void, FwError> {
        Shutdown();
        return Err(FwError{e, s, sub});
    };

    if (!headless) {
        (void)m_rt->jobs.Init(1);
        if (auto r = m_rt->plat.Init("VeilOfTheSilent", "EngineN", false); !r)
            return fail(FwErr::Platform, FwStage::Platform, u16(r.error()));
        WindowConfig wc{};
        wc.title = "Veil of the Silent";
        wc.size = WindowSize{cfg.viewport.w, cfg.viewport.h};
        if (auto r = m_rt->win.Create(wc, false); !r)
            return fail(FwErr::Window, FwStage::Window, u16(r.error()));
        std::construct_at(&m_rt->gfx);
        m_rt->hasGfx = 1;
        if (auto r = m_rt->gfx.Init(m_rt->win); !r)
            return fail(FwErr::Renderer, FwStage::Renderer, u16(r.error()));
    }

    if (auto r = m_rt->assets.Init(cfg.assets); !r)
        return fail(FwErr::Assets, FwStage::Assets, u16(r.error()));
    if (!headless && m_rt->gfx.IsValid()) {
        m_rt->assets.SetGpuDestroy(
            [](TextureHandle t, void* ctx) noexcept {
                if (t != kWhiteTexture) static_cast<Renderer*>(ctx)->DestroyTexture(t);
            },
            &m_rt->gfx);
    }

    AudioSystemConfig ac = cfg.audio;
    ac.headless = headless;
    if (auto r = m_rt->audio.Init(ac); !r)
        return fail(FwErr::Audio, FwStage::Audio, u16(r.error()));
    if (auto r = m_rt->world.Init(cfg.world); !r)
        return fail(FwErr::World, FwStage::World, u16(r.error()));
    if (hooks.registerComponents) {
        if (auto r = hooks.registerComponents(m_rt->world, m_user); !r)
            return fail(r.error(), FwStage::Client);
    }

    PhysicsConfig pc{};
    pc.maxManifolds = cfg.maxManifolds;
    pc.velocityIterations = cfg.velIters;
    pc.sleepTicksThreshold = cfg.sleepTicks;
    if (auto r = m_rt->phys.Init(m_rt->world, pc); !r)
        return fail(FwErr::Physics, FwStage::Physics, u16(r.error()));

    GameplayConfig gc{};
    gc.maxCuesPerTick = cfg.maxCues;
    if (auto r = m_rt->game.Init(m_rt->world, hooks.policies, gc); !r)
        return fail(FwErr::Gameplay, FwStage::Gameplay, u16(r.error()));

    if (m_debug) {
        if (auto r = m_rt->debug.Init(cfg.debug); !r)
            return fail(FwErr::Debug, FwStage::Debug, u16(r.error()));
    }
    if (auto r = m_rt->dir.Init(m_rt->assets); !r)
        return fail(r.error(), FwStage::Director);
    if (hooks.boot) {
        LoadCtx boot = MakeLoad();
        if (auto r = hooks.boot(boot); !r)
            return fail(r.error(), FwStage::Client);
    }

    m_ok = 1;
    m_run = 1;
    return {};
}

void Application::Shutdown() noexcept {
    if (!m_rt) {
        m_arena.Shutdown();
        m_ok = m_run = 0;
        return;
    }
    if (m_ok) {
        LoadCtx load = MakeLoad();
        if (m_rt->hooks.preShutdown) m_rt->hooks.preShutdown(load);
        m_rt->dir.Shutdown(m_rt->assets, m_rt->audio, &load);
        if (m_debug) m_rt->debug.Shutdown();
        m_rt->game.Shutdown();
        m_rt->phys.Shutdown();
        m_rt->world.Shutdown();
        m_rt->audio.Shutdown();
        m_rt->assets.Shutdown();
        if (!m_headless) {
            m_rt->jobs.Shutdown();
            if (m_rt->hasGfx) m_rt->gfx.Shutdown();
            if (m_rt->plat.IsInitialized()) m_rt->plat.Shutdown();
        }
    }
    m_rt->~RT();
    m_rt = nullptr;
    m_arena.Shutdown();
    m_ok = m_run = 0;
    m_user = nullptr;
}

Result<void, FwError> Application::Run(u32 fps) noexcept {
    if (!m_rt || !m_ok) return Err(FwError{FwErr::BadConfig, FwStage::None, 0});
    if (m_headless) return StepN(120);

    m_rt->pacer.SetTargetFps(fps);
    u64 prev = Clock::NowNs();

    while (m_run && !m_rt->plat.ShouldQuit()) {
        m_rt->pacer.BeginFrame();
        const u64 now = Clock::NowNs();
        const u64 dt = now > prev ? now - prev : kFixedDtNs;
        prev = now;

        const WindowFlags f = m_rt->plat.Pump(m_rt->win, m_rt->input);
        if (HasFlag(f, WindowFlags::CloseRequested)) {
            Quit();
            break;
        }
        if (HasFlag(f, WindowFlags::Resized)) {
            const WindowSize s = m_rt->win.State().pixelSize;
            if (s.IsValid()) m_rt->vp = Viewport{s.w, s.h};
        }
        if (HasFlag(f, WindowFlags::Minimized | WindowFlags::FocusLost)) {
            if (!m_rt->focusOff) {
                m_rt->focusOff = 1;
                m_rt->time.SetPaused(true);
                m_rt->audio.SetFocusMuted(true);
            }
        } else if (HasFlag(f, WindowFlags::Restored | WindowFlags::FocusGained)) {
            if (m_rt->focusOff) {
                m_rt->focusOff = 0;
                m_rt->time.OnWindowRefocus();
                m_rt->time.SetPaused(false);
                m_rt->audio.SetFocusMuted(false);
                prev = Clock::NowNs();
            }
        }

        if (auto r = Step(dt); !r) return r;
        m_rt->pacer.EndFrame();
    }
    return {};
}

void Application::HandleCues(SimCtx& ctx, const SimStepView& step) noexcept {
    u32 n = 0;
    const auto cues = m_rt->game.DrainCues();

    CameraController2D* cam = nullptr;
    if ((m_rt->world.RegisteredPoolsMask() & MakeComponentMask<CameraController2D>()) != 0) {
        auto& pool = m_rt->world.GetPool<CameraController2D>();
        if (pool.Size()) cam = &pool.ComponentAtDense(0);
    }

    for (const SimCue& c : cues) {
        switch (c.kind) {
            case SimCueKind::AudioOneShot:
                if (n < m_rt->cues.size()) {
                    f32 x = c.worldX, y = c.worldY;
                    if (x == 0.f && y == 0.f && c.emitter.IsValid() && m_rt->world.IsAlive(c.emitter)) {
                        const auto& wt = m_rt->world.GetWorldByIndex(c.emitter.index);
                        x = wt.x;
                        y = wt.y;
                    }
                    m_rt->cues[n++] = AudioTriggerReq{
                        SoundCueId::FromAssetId(c.assetId), x, y, c.emitter.Packed()};
                }
                break;
            case SimCueKind::CameraTraumaPulse:
                if (cam) cam->AddTrauma(c.Magnitude());
                break;
            case SimCueKind::HitstopFreeze:
                if (c.channel == 0xFF)
                    m_rt->time.MaskHitstop(kGameplayMask, c.durationTicks);
                else if (c.channel < kGameplayChannels)
                    m_rt->time.Hitstop(TimeChannel(c.channel), c.durationTicks);
                break;
            case SimCueKind::GamepadRumble:
                if (!m_headless && m_rt->plat.IsInitialized()) {
                    const u16 f = u16(std::clamp(c.Magnitude() * 65535.f, 0.f, 65535.f));
                    (void)m_rt->plat.RumbleGamepad(f, f, u32(c.durationTicks) * 8);
                }
                break;
            default: break;
        }
        if (m_rt->hooks.cue) m_rt->hooks.cue(c, ctx, m_user);
    }

    std::array<f32, kGameplayChannels> scales{};
    for (u8 i = 0; i < kGameplayChannels; ++i)
        scales[i] = step.Scale(TimeChannel(i));

    AudioUpdateDesc ad{};
    ad.triggers = {m_rt->cues.data(), n};
    ad.assets = &m_rt->assets;
    ad.occlusion = [](f32 lx, f32 ly, f32 ex, f32 ey, void* p) noexcept -> bool {
        const auto* phys = static_cast<const PhysicsSystem*>(p);
        if (!phys) return false;
        const f32 dx = ex - lx, dy = ey - ly;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 <= 1e-6f) return false;
        const f32 d = std::sqrt(d2);
        return phys->Raycast(lx, ly, dx / d, dy / d, d, 0x0001).Hit();
    };
    ad.occlusionCtx = &m_rt->phys;
    ad.channelScales = scales.data();
    ad.tick = step.tick;
    ad.listenerX = cam ? cam->anchorX : 0.f;
    ad.listenerY = cam ? cam->anchorY : 0.f;
    ad.frozenMask = u8(step.frozenMask | (m_freeze ? kGameplayMask : 0));
    m_rt->audio.Update(ad);

    for (u32 rel = m_rt->audio.PollReleasableScopes(); rel; rel &= rel - 1)
        m_rt->assets.EndScope(u8(std::countr_zero(rel)));
}

void Application::Draw() noexcept {
    const RenderStep& rv = m_rt->time.RenderView();
    const f32 ppm = f32(m_ppmQ4) * (1.f / 16.f);
    m_rt->list.Reset(m_rt->vp);
    WorldSystems::ExtractRender(m_rt->world, m_rt->assets, rv, m_rt->list, ppm);

    DrawCtx dc{};
    dc.world = &m_rt->world;
    dc.phys = &m_rt->phys;
    dc.game = &m_rt->game;
    dc.assets = &m_rt->assets;
    dc.list = &m_rt->list;
    dc.frame = &m_rt->input.Frame();
    dc.time = &rv;
    dc.dir = &m_rt->dir;
    m_rt->dir.DrawStates(dc);
    m_rt->dir.DrawWipe(m_rt->list, m_rt->vp);

    if (m_debug) {
        FrameCtx fc{};
        fc.world = &m_rt->world;
        fc.physics = &m_rt->phys;
        fc.viewport = m_rt->vp;
        fc.tick = u32(m_rt->time.Stats().totalTicks);
        m_rt->debug.Emit(fc, m_rt->list);
    }
    m_rt->list.Compile();
}

Result<void, FwError> Application::Step(u64 dtNs) noexcept {
    if (!m_rt || !m_ok) return Err(FwError{FwErr::BadConfig, FwStage::None, 0});

    m_rt->time.BeginFrame(dtNs);
    ++m_frame;

    LoadCtx load = MakeLoad();
    (void)m_rt->dir.Tick(load);
    m_freeze = m_rt->dir.Frozen() ? 1 : 0;

    while (const TickStep step = m_rt->time.PollTick()) {
        if (m_freeze) continue;

        const TickInput tin = m_rt->input.PollTick(step.tick);
        m_rt->world.BeginTick(step);

        if (!step.IsFrozen(TimeChannel::World)) {
            SimCtx sc{};
            sc.world = &m_rt->world;
            sc.phys = &m_rt->phys;
            sc.game = &m_rt->game;
            sc.assets = &m_rt->assets;
            sc.audio = &m_rt->audio;
            sc.acts = &m_rt->acts;
            sc.input = &tin;
            sc.dir = &m_rt->dir;

            m_rt->dir.TickStates(sc, step);
            m_rt->game.Tick(m_rt->world, m_rt->phys, m_rt->assets, step, tin, m_rt->acts);
            m_rt->phys.Step(m_rt->world, step);
            HandleCues(sc, step);
        }

        m_rt->world.EndTick(step);
        ++m_ticks;
        if (m_debug) m_rt->debug.EndTick(m_rt->world, step.tick, &tin, &m_rt->acts);
    }

    if (!m_headless && !m_rt->focusOff && m_rt->hasGfx && m_rt->gfx.IsValid()) {
        Draw();
        m_rt->gfx.Submit(m_rt->list);
    }
    m_rt->audio.Pump(2);
    return {};
}

Result<void, FwError> Application::StepN(u32 ticks) noexcept {
    for (u32 i = 0; i < ticks; ++i)
        if (auto r = Step(kFixedDtNs); !r) return r;
    return {};
}

u64 Application::Hash() const noexcept {
    if (!m_rt) return 0;
    return SceneDirector::Mix(
        m_ticks,
        m_rt->world.ComputeAuditHash(),
        m_rt->phys.ComputeAuditHash(m_rt->world),
        m_rt->game.ComputeAuditHash(m_rt->world),
        m_rt->audio.AuditHash(),
        0,
        m_rt->dir.Hash());
}

Platform& Application::Plat() noexcept { return m_rt->plat; }
Window& Application::Win() noexcept { return m_rt->win; }
Renderer& Application::Gfx() noexcept { return m_rt->gfx; }
JobSystem& Application::Jobs() noexcept { return m_rt->jobs; }
SceneDirector& Application::Dir() noexcept { return m_rt->dir; }
World& Application::Wld() noexcept { return m_rt->world; }
PhysicsSystem& Application::Phys() noexcept { return m_rt->phys; }
GameplayPipeline& Application::Game() noexcept { return m_rt->game; }
AssetBank& Application::Assets() noexcept { return m_rt->assets; }
AudioSystem& Application::Sfx() noexcept { return m_rt->audio; }
DebugSystem& Application::Dbg() noexcept { return m_rt->debug; }
TimeSystem& Application::Clock() noexcept { return m_rt->time; }
InputSystem& Application::In() noexcept { return m_rt->input; }
ActionBuffer& Application::Acts() noexcept { return m_rt->acts; }
RenderList& Application::List() noexcept { return m_rt->list; }

const Platform& Application::Plat() const noexcept { return m_rt->plat; }
const Window& Application::Win() const noexcept { return m_rt->win; }
const Renderer& Application::Gfx() const noexcept { return m_rt->gfx; }
const JobSystem& Application::Jobs() const noexcept { return m_rt->jobs; }
const SceneDirector& Application::Dir() const noexcept { return m_rt->dir; }
const World& Application::Wld() const noexcept { return m_rt->world; }
const PhysicsSystem& Application::Phys() const noexcept { return m_rt->phys; }
const GameplayPipeline& Application::Game() const noexcept { return m_rt->game; }
const AssetBank& Application::Assets() const noexcept { return m_rt->assets; }
const AudioSystem& Application::Sfx() const noexcept { return m_rt->audio; }
const DebugSystem& Application::Dbg() const noexcept { return m_rt->debug; }
const TimeSystem& Application::Clock() const noexcept { return m_rt->time; }
const InputSystem& Application::In() const noexcept { return m_rt->input; }
const ActionBuffer& Application::Acts() const noexcept { return m_rt->acts; }
const RenderList& Application::List() const noexcept { return m_rt->list; }

} // namespace en

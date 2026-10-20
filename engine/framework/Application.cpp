#include "framework/Application.h"

namespace en {

namespace {
f32 QuerySpatialOcclusionCallback(f32 lx, f32 ly, f32 ex, f32 ey, void* userCtx) noexcept {
    if (!userCtx) return 0.0f;
    const auto* spatial = static_cast<const SpatialIndex2D*>(userCtx);
    const Vec2 delta{ex - lx, ey - ly};
    const f32 dist = delta.Length();
    if (dist < 0.25f) return 0.0f;
    const RayHit2D hit = spatial->Raycast({lx, ly}, delta / dist, dist, 1U);
    return (hit.hit && hit.distance < dist - 0.1f) ? 0.75f : 0.0f;
}
} // namespace

SceneLoadContext Application::MakeLoadContext() noexcept {
    return SceneLoadContext{
        m_world,
        m_gameplay,
        m_assets,
        m_audio,
        m_renderer,
        m_hooks.userAppState,
        &m_director,
        m_director.ActiveSceneScope(),
        m_director.PersistentScope(),
        0,
        0,
    };
}

Result<void, FrameworkErrc> Application::Init(
    const ApplicationConfig& cfg, const AppHooks& hooks) noexcept {
    Shutdown();
    m_hooks = hooks;
    m_headless = cfg.headless;
    m_frameIndex = 0;

    (void)m_jobs.Init(cfg.workerThreads);
    if (!m_platform.Init(cfg.appName, cfg.orgName, cfg.headless)) {
        return Err(FrameworkErrc::PlatformInitFailed);
    }
    if (!cfg.headless) {
        if (!m_window.Create(cfg.window)) return Err(FrameworkErrc::WindowCreateFailed);
    }
    RendererConfig rcfg = cfg.renderer;
    rcfg.headless = cfg.headless;
    if (!m_renderer.Init(m_window, rcfg)) return Err(FrameworkErrc::RendererInitFailed);
    if (!m_assets.Init(cfg.assets)) return Err(FrameworkErrc::AssetInitFailed);
    if (!m_audio.Init(cfg.audio)) return Err(FrameworkErrc::AudioInitFailed);
    if (!m_world.Init(cfg.world)) return Err(FrameworkErrc::WorldInitFailed);
    if (!m_gameplay.Init(m_world, hooks.gameplayPolicies, cfg.gameplay)) {
        return Err(FrameworkErrc::GameplayInitFailed);
    }
    if (!m_debug.Init()) return Err(FrameworkErrc::DebugInitFailed);
    if (!m_director.Init(m_assets)) return Err(FrameworkErrc::AssetInitFailed);

    m_input.Reset();
    m_actionBuffer.Clear();
    m_time.Reset();

    if (m_hooks.onRegisterComponents) {
        if (!m_hooks.onRegisterComponents(m_world)) return Err(FrameworkErrc::WorldInitFailed);
    }

    m_initialized = true;
    if (m_hooks.onBoot) {
        SceneLoadContext loadCtx = MakeLoadContext();
        if (auto r = m_hooks.onBoot(loadCtx); !r) return r;
    }
    if (m_director.HasPendingWork()) {
        if (auto r = CommitSafeFrameBarrier(); !r) return r;
    }
    m_assets.Pump(&m_renderer, 64);
    return {};
}

void Application::Shutdown() noexcept {
    if (!m_initialized) return;
    SceneLoadContext loadCtx = MakeLoadContext();
    if (m_hooks.onPreShutdown) {
        m_hooks.onPreShutdown(loadCtx);
    }
    m_director.Shutdown(m_assets, m_audio, &loadCtx);
    m_debug.Shutdown();
    m_gameplay.Shutdown();
    m_world.Shutdown();
    m_audio.Shutdown();
    m_assets.Shutdown();
    m_renderer.Shutdown();
    m_window.Destroy();
    m_platform.Shutdown();
    m_jobs.Shutdown();
    m_initialized = false;
}

Result<void, FrameworkErrc> Application::CommitSafeFrameBarrier() noexcept {
    SceneLoadContext loadCtx = MakeLoadContext();
    auto res = m_director.CommitSafeFramePoint(loadCtx);
    if (!res) return Err(res.error());
    if (*res) {
        m_input.SetActionMask(m_director.TopInputMask());
        m_time.SetPaused(m_director.IsClockPausedByState());
    }
    return {};
}

void Application::ExecuteSimTick(const SimStepView& step) noexcept {
    const TickInput tickIn = m_input.PollTick(step.tickIndex);
    m_actionBuffer.Feed(tickIn, step.IsFrozen(TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickIn, m_actionBuffer);

    SimContext simCtx{
        m_world,
        m_gameplay,
        m_assets.Bank(),
        m_audio,
        m_platform,
        m_actionBuffer,
        &tickIn,
        m_hooks.userAppState,
        &m_director,
        step,
    };
    m_director.DispatchSimTick(simCtx);

    m_coroutines.Step(step.Dt(TimeChannel::World), m_gameplay.RawWorldFlags());
    m_world.StepAmbientVelocities(step);
    m_world.StepAnimationsAndLifetimes(step);
    m_world.EndTick(step);
    m_debug.EndSimTick(m_world, step, tickIn, m_actionBuffer);
}

void Application::RouteCuesAndSyncAudio() noexcept {
    u32 relMask = m_audio.PollReleasableScopeMask();
    while (relMask != 0U) {
        const auto s = static_cast<ScopeId>(std::countr_zero(relMask));
        relMask &= (relMask - 1U);
        m_assets.EndScope(s);
    }

    Vec2 listenerPos{0.0f, 0.0f};
    m_world.Each<CameraRig2D>([&](EntityId id, const CameraRig2D& rig) noexcept {
        if (rig.active) {
            const WorldTransform2D& wt = m_world.GetWorldByIndex(id.index);
            listenerPos = {wt.x, wt.y};
        }
    });

    SmallVector<AudioTriggerReq, 64> audioTriggers{};
    for (const SimCue& cue : m_gameplay.DrainCues()) {
        switch (cue.kind) {
            case SimCueKind::AudioOneShot:
                (void)audioTriggers.PushBack(
                    AudioTriggerReq{cue.assetId, cue.worldX, cue.worldY, cue.emitter.Packed(), 0});
                break;
            case SimCueKind::CameraTraumaPulse: {
                const f32 amt = static_cast<f32>(cue.magnitude_q16) * (1.0f / 65535.0f);
                m_world.Each<CameraController2D>([&](EntityId, CameraController2D& ctrl) noexcept {
                    ctrl.AddTrauma(amt);
                });
                break;
            }
            case SimCueKind::GamepadRumblePulse:
                (void)m_platform.RumbleGamepad(0, cue.magnitude_q16, cue.magnitude_q16, cue.durationTicks * 8U);
                break;
            case SimCueKind::SpawnPrefabEffect:
                if (cue.assetId.IsValid() && m_director.ActiveSceneScope() != kInvalidScope) {
                    const BlobHandle ph = m_assets.PinBlob(
                        m_director.ActiveSceneScope(), cue.assetId, AssetResidency::Sim);
                    LocalTransform2D loc{};
                    loc.x = cue.worldX;
                    loc.y = cue.worldY;
                    (void)m_world.InstantiatePrefab(m_assets.Bank(), ph, loc);
                }
                break;
            case SimCueKind::CustomGameEvent:
                if (m_hooks.onConsumeSimCue) {
                    SimContext simCtx{
                        m_world, m_gameplay, m_assets.Bank(), m_audio, m_platform,
                        m_actionBuffer, nullptr, m_hooks.userAppState, &m_director, {}};
                    m_hooks.onConsumeSimCue(cue, simCtx);
                }
                break;
        }
    }

    AudioUpdateDesc audDesc{};
    audDesc.tickIndex = m_time.Telemetry().totalSimTicks;
    audDesc.listenerX = listenerPos.x;
    audDesc.listenerY = listenerPos.y;
    audDesc.channelScales = m_time.EffectiveScales();
    audDesc.triggers = audioTriggers.Span();
    audDesc.assetBank = &m_assets.Bank();
    audDesc.occlusionQuery = &QuerySpatialOcclusionCallback;
    audDesc.occlusionUserCtx = &m_gameplay.Spatial();
    audDesc.frozenChannelMask = m_time.FrozenMask();
    audDesc.focusMuted = !m_window.State().focused;
    m_audio.Update(audDesc);
}

void Application::ExtractAndRenderFrame() noexcept {
    RenderList& list = m_renderer.BeginFrame(m_window.State().pixelSize);
    const RenderTimeView rtv = m_time.GetRenderView();

    Renderer2D::ExtractWorld(m_world, m_assets.Bank(), m_gameplay.Particles(), rtv, list);

    RenderContext renderCtx{
        m_world,
        m_gameplay,
        m_assets.Bank(),
        list,
        m_input.GetFrameView(),
        m_window.State(),
        m_hooks.userAppState,
        &m_director,
        rtv,
    };
    m_director.DispatchRenderExtract(renderCtx);

    std::array<AudioEmitterView, 32> emitters{};
    const u32 emCount = m_audio.ExtractActiveEmitters(emitters);
    DebugFrameContext dbgCtx{
        &m_world,
        &m_gameplay,
        rtv,
        m_input.GetFrameView(),
        m_window.State().pixelSize,
        std::span<const AudioEmitterView>(emitters.data(), emCount),
        m_time.IsPaused(),
    };
    m_debug.EmitOverlays(dbgCtx, list);

    m_renderer.Submit(list);
    m_lastRenderListHash = list.ComputeAuditHash();
    m_debug.Profiler().EndFrame(
        static_cast<u16>(m_world.AliveCount()), list.Stats(), m_audio.Telemetry());
}

Result<void, FrameworkErrc> Application::StepFrame() noexcept {
    ++m_frameIndex;
    m_assets.Bank().AdvanceFrame(static_cast<u32>(m_frameIndex));

    const WindowFlags winFlags = m_platform.Pump(m_window, m_input);
    if (HasFlag(winFlags, WindowFlags::FocusGained)) {
        m_time.OnWindowRefocus();
    }
    m_assets.Pump(&m_renderer);

    DebugFrameContext dbgBegin{
        &m_world, &m_gameplay, m_time.GetRenderView(), m_input.GetFrameView(),
        m_window.State().pixelSize, {}, m_time.IsPaused()};
    const DebugLoopDirectives dirs = m_debug.BeginFrame(dbgBegin);
    if (dirs.TogglePause()) m_time.SetPaused(!m_time.IsPaused());
    if (dirs.stepTicks > 0) m_time.RequestStep(dirs.stepTicks);

    if (m_director.HasPendingWork()) {
        if (auto r = CommitSafeFrameBarrier(); !r) return r;
    }

    m_time.BeginFrame();
    while (const SimStepView step = m_time.PollTick()) {
        ExecuteSimTick(step);
    }

    RouteCuesAndSyncAudio();
    ExtractAndRenderFrame();
    return {};
}

Result<FrameAuditDigest, FrameworkErrc> Application::StepHeadless(
    u32 tickCount, u32 audioBlocksPerTick) noexcept {
    const u32 steps = std::max(1U, tickCount);
    for (u32 t = 0; t < steps; ++t) {
        ++m_frameIndex;
        m_assets.Bank().AdvanceFrame(static_cast<u32>(m_frameIndex));
        m_assets.Pump(&m_renderer, 64);
        if (m_director.HasPendingWork()) {
            if (auto r = CommitSafeFrameBarrier(); !r) return Err(r.error());
        }
        (void)m_input.EndFrame();
        m_time.BeginFrameManual(kFixedDtNs);
        while (const SimStepView step = m_time.PollTick()) {
            ExecuteSimTick(step);
        }
        RouteCuesAndSyncAudio();
        if (audioBlocksPerTick > 0) {
            m_audio.PumpBlocks(audioBlocksPerTick);
        }
    }
    ExtractAndRenderFrame();
    return ComputeFrameAuditDigest();
}

Result<void, FrameworkErrc> Application::Run() noexcept {
    while (!m_platform.ShouldQuit()) {
        if (auto r = StepFrame(); !r) return r;
    }
    return {};
}

FrameAuditDigest Application::ComputeFrameAuditDigest() const noexcept {
    FrameAuditDigest d{};
    d.tickIndex = m_time.Telemetry().totalSimTicks;
    d.worldHash = m_world.ComputeAuditHash();
    d.gameplayHash = m_gameplay.ComputeAuditHash(m_world);
    d.assetBankHash = m_assets.Bank().ComputeAuditHash();
    d.audioHash = m_audio.ComputeAuditHash();
    d.renderListHash = m_lastRenderListHash;
    d.directorHash = m_director.ComputeAuditHash();
    d.combinedHash = SceneDirector::CombineAuditHashes(
        d.tickIndex, d.worldHash, d.gameplayHash, d.assetBankHash,
        d.audioHash, d.renderListHash, d.directorHash);
    return d;
}

} // namespace en

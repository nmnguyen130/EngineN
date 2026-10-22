#include "framework/Application.h"

#include "world/WorldExtractor.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace en {

// Heap-allocated runtime slab housed inside Application::m_arena so Application itself is 64B (1 cache line exact).
struct alignas(64) Application::AppRuntimeState {
    TimePipeline time{};
    InputPipeline input{};
    ActionBuffer actions{};
    AssetBank assetBank{};
    AudioSystem audio{};
    World world{};
    PhysicsSystem physics{};
    GameplayPipeline gameplay{};
    DebugSystem debug{};
    SceneDirector director{};
    RenderList renderList{};
    AppHooks hooks{};
    InputReplayStream* replayStream{nullptr};
    std::array<AudioTriggerReq, 64> audioTriggerScratch{};
    std::array<AudioEmitterView, 32> audioEmitterScratch{};
    ViewportExtent viewportSize{1280, 720};
    u16 activeEmitterCount{0U};
    u16 replayPrevHeld{0U};
};

Application::Application(Application&& other) noexcept
    : m_arena(std::move(other.m_arena)),
      m_rt(other.m_rt),
      m_userAppState(other.m_userAppState),
      m_lastCombinedAuditHash(other.m_lastCombinedAuditHash),
      m_frameIndex(other.m_frameIndex),
      m_totalSimTicks(other.m_totalSimTicks),
      m_pixelsPerMeterQ4(other.m_pixelsPerMeterQ4),
      m_initialized(other.m_initialized),
      m_running(other.m_running),
      m_headless(other.m_headless),
      m_debugEnabled(other.m_debugEnabled),
      m_statePausedClock(other.m_statePausedClock),
      m_replayMode(other.m_replayMode) {
    other.m_rt = nullptr;
    other.m_userAppState = nullptr;
    other.m_initialized = 0U;
    other.m_running = 0U;
    other.m_statePausedClock = 0U;
    other.m_replayMode = ReplayMode::None;
}

Application& Application::operator=(Application&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arena = std::move(other.m_arena);
        m_rt = other.m_rt;
        m_userAppState = other.m_userAppState;
        m_lastCombinedAuditHash = other.m_lastCombinedAuditHash;
        m_frameIndex = other.m_frameIndex;
        m_totalSimTicks = other.m_totalSimTicks;
        m_pixelsPerMeterQ4 = other.m_pixelsPerMeterQ4;
        m_initialized = other.m_initialized;
        m_running = other.m_running;
        m_headless = other.m_headless;
        m_debugEnabled = other.m_debugEnabled;
        m_statePausedClock = other.m_statePausedClock;
        m_replayMode = other.m_replayMode;

        other.m_rt = nullptr;
        other.m_userAppState = nullptr;
        other.m_initialized = 0U;
        other.m_running = 0U;
        other.m_statePausedClock = 0U;
        other.m_replayMode = ReplayMode::None;
    }
    return *this;
}

SceneLoadContext Application::MakeLoadContext() const noexcept {
    SceneLoadContext ctx{};
    if (m_rt) {
        ctx.world = &m_rt->world;
        ctx.physics = &m_rt->physics;
        ctx.gameplay = &m_rt->gameplay;
        ctx.assetBank = &m_rt->assetBank;
        ctx.audio = &m_rt->audio;
        ctx.director = &m_rt->director;
        ctx.userAppState = m_userAppState;
        ctx.frameIndex = static_cast<u32>(m_frameIndex);
        ctx.sceneScope = m_rt->director.ActiveSceneScope();
        ctx.persistentScope = m_rt->director.PersistentScope();
    }
    return ctx;
}

void Application::BindReplayStream(InputReplayStream* stream, ReplayMode mode) noexcept {
    if (!m_rt) return;
    m_rt->replayStream = stream;
    m_rt->replayPrevHeld = 0U;
    m_replayMode = (stream != nullptr) ? mode : ReplayMode::None;
    if (stream && mode == ReplayMode::Playback) {
        stream->RewindPlayback();
    }
}

Result<void, FrameworkError> Application::Init(
    const ApplicationConfig& cfg,
    const AppHooks& hooks) noexcept {
    Shutdown();

    if (!cfg.viewport.IsValid() || cfg.defaultPixelsPerMeter <= 0.0f) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    if (!m_arena.Init(sizeof(AppRuntimeState) + 256U)) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    u8* mem = m_arena.CarveRawBytes(sizeof(AppRuntimeState));
    if (!mem) {
        m_arena.Shutdown();
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }
    m_rt = ::new (mem) AppRuntimeState{};
    m_rt->hooks = hooks;
    m_rt->viewportSize = cfg.viewport;
    m_userAppState = hooks.userAppState;
    m_pixelsPerMeterQ4 = static_cast<u16>(std::clamp(cfg.defaultPixelsPerMeter * 16.0f, 16.0f, 65535.0f));
    m_headless = cfg.headless;
    m_debugEnabled = cfg.enableDebugTools;
    m_statePausedClock = 0U;
    m_replayMode = ReplayMode::None;
    m_frameIndex = 0U;
    m_totalSimTicks = 0U;
    m_lastCombinedAuditHash = 0U;

    // 1. Initialize AssetBank
    if (auto r = m_rt->assetBank.Init(cfg.assets); !r) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::AssetInitFailed, SubSystemStage::Assets, static_cast<u16>(r.error())});
    }

    // 2. Initialize AudioSystem
    if (auto r = m_rt->audio.Init(cfg.audio); !r) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::AudioInitFailed, SubSystemStage::Audio, static_cast<u16>(r.error())});
    }

    // 3. Initialize World (automatically registers built-in 2D component pools)
    if (auto r = m_rt->world.Init(cfg.world); !r) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::WorldInitFailed, SubSystemStage::World, static_cast<u16>(r.error())});
    }

    if (hooks.onRegisterComponents) {
        if (auto r = hooks.onRegisterComponents(m_rt->world, m_userAppState); !r) {
            Shutdown();
            return Err(FrameworkError{r.error(), SubSystemStage::ClientApp, 0U});
        }
    }

    // 4. Initialize PhysicsSystem
    PhysicsConfig physCfg{};
    physCfg.maxManifolds = cfg.maxManifolds;
    physCfg.velocityIterations = cfg.velocityIterations;
    physCfg.sleepTicksThreshold = cfg.sleepTicksThreshold;
    if (auto r = m_rt->physics.Init(m_rt->world, physCfg); !r) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::PhysicsInitFailed, SubSystemStage::Physics, static_cast<u16>(r.error())});
    }

    // 5. Initialize GameplayPipeline
    GameplayConfig gpCfg{};
    gpCfg.maxCuesPerTick = cfg.maxCuesPerTick;
    if (auto r = m_rt->gameplay.Init(m_rt->world, hooks.gameplayPolicies, gpCfg); !r) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::GameplayInitFailed, SubSystemStage::Gameplay, static_cast<u16>(r.error())});
    }

    // 6. Initialize DebugSystem
    if (cfg.enableDebugTools != 0U) {
        if (auto r = m_rt->debug.Init(cfg.debug); !r) {
            Shutdown();
            return Err(FrameworkError{
                FrameworkErrc::DebugInitFailed, SubSystemStage::Debug, static_cast<u16>(r.error())});
        }
    }

    // 7. Initialize SceneDirector
    if (auto r = m_rt->director.Init(m_rt->assetBank); !r) {
        Shutdown();
        return Err(FrameworkError{r.error(), SubSystemStage::SceneDirector, 0U});
    }

    m_initialized = 1U;
    m_running = 1U;

    // 8. Invoke client OnBoot hook and commit initial safe frame barrier
    SceneLoadContext loadCtx = MakeLoadContext();
    if (hooks.onBoot) {
        if (auto r = hooks.onBoot(loadCtx); !r) {
            Shutdown();
            return Err(FrameworkError{r.error(), SubSystemStage::ClientApp, 0U});
        }
    }

    if (auto r = m_rt->director.CommitSafeFramePoint(loadCtx); !r) {
        Shutdown();
        return Err(FrameworkError{r.error(), SubSystemStage::SceneDirector, 0U});
    }

    return {};
}

void Application::Shutdown() noexcept {
    if (m_rt) {
        SceneLoadContext loadCtx = MakeLoadContext();
        if (m_initialized != 0U && m_rt->hooks.onPreShutdown) {
            m_rt->hooks.onPreShutdown(loadCtx);
        }

        m_rt->director.Shutdown(m_rt->assetBank, m_rt->audio, &loadCtx);
        m_rt->debug.Shutdown();
        m_rt->gameplay.Shutdown();
        m_rt->physics.Shutdown();
        m_rt->world.Shutdown();
        m_rt->audio.Shutdown();
        m_rt->assetBank.Shutdown();
        m_rt->~AppRuntimeState();
        m_rt = nullptr;
    }
    m_arena.Shutdown();
    m_initialized = 0U;
    m_running = 0U;
    m_statePausedClock = 0U;
    m_replayMode = ReplayMode::None;
}

void Application::RouteSimCuesAndSyncAudio(SimContext& simCtx, const SimStepView& step) noexcept {
    if (!m_rt) return;

    u32 audioTrigCount = 0U;
    const std::span<const SimCue> cues = m_rt->gameplay.DrainCues();
    const u64 regMask = m_rt->world.RegisteredPoolsMask();
    const bool hasCamCtrl = (regMask & MakeComponentMask<CameraController2D>()) != 0ULL;

    for (const SimCue& cue : cues) {
        switch (cue.kind) {
            case SimCueKind::AudioOneShot: {
                if (audioTrigCount < m_rt->audioTriggerScratch.size()) {
                    m_rt->audioTriggerScratch[audioTrigCount++] = AudioTriggerReq{
                        SoundCueId::FromAssetId(cue.assetId),
                        cue.worldX,
                        cue.worldY,
                        cue.emitter.Packed(),
                    };
                }
                break;
            }
            case SimCueKind::SpawnPrefabEffect: {
                if (cue.assetId.IsValid()) {
                    const BlobHandle bh = m_rt->assetBank.FindBlobHandle(cue.assetId);
                    if (bh.IsValid()) {
                        (void)WorldExtractor::InstantiatePrefab(
                            m_rt->world, m_rt->assetBank, bh, LocalTransform2D{cue.worldX, cue.worldY});
                    }
                }
                break;
            }
            case SimCueKind::CameraTraumaPulse: {
                if (hasCamCtrl) {
                    auto& camPool = m_rt->world.GetPool<CameraController2D>();
                    for (u16 i = 0U; i < camPool.Size(); ++i) {
                        camPool.ComponentAtDense(i).AddTrauma(cue.Magnitude());
                    }
                }
                break;
            }
            case SimCueKind::HitstopFreeze: {
                if (cue.channel == 0xFFU) {
                    m_rt->time.RequestGameplayFreeze(cue.durationTicks);
                } else {
                    m_rt->time.RequestHitstop(static_cast<TimeChannel>(cue.channel & 7U), cue.durationTicks);
                }
                break;
            }
            case SimCueKind::GamepadRumble:
            case SimCueKind::CustomGameEvent:
                break;
        }

        if (m_rt->hooks.onConsumeSimCue) {
            m_rt->hooks.onConsumeSimCue(cue, simCtx, m_userAppState);
        }
    }

    // Resolve listener position from active CameraController2D (or world origin)
    f32 listenerX = 0.0f;
    f32 listenerY = 0.0f;
    if (hasCamCtrl) {
        const auto& camPool = m_rt->world.GetPool<CameraController2D>();
        if (camPool.Size() > 0U) {
            listenerX = camPool.ComponentAtDense(0U).anchorX;
            listenerY = camPool.ComponentAtDense(0U).anchorY;
        }
    }

    AudioUpdateDesc audioDesc{};
    audioDesc.triggers = {m_rt->audioTriggerScratch.data(), audioTrigCount};
    audioDesc.assetBank = &m_rt->assetBank;
    audioDesc.occlusionQuery = [](f32 lx, f32 ly, f32 ex, f32 ey, void* userCtx) noexcept -> bool {
        const auto* phys = static_cast<const PhysicsSystem*>(userCtx);
        if (!phys) return false;
        const f32 dx = ex - lx;
        const f32 dy = ey - ly;
        const f32 dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= 1e-3f) return false;
        const RayHit2D hit = phys->Raycast(lx, ly, dx / dist, dy / dist, dist, 0x0001U);
        return hit.Hit();
    };
    audioDesc.occlusionUserCtx = &m_rt->physics;
    audioDesc.channelScales = m_rt->time.EffectiveScales().data();
    audioDesc.tickIndex = static_cast<u32>(step.tickIndex);
    audioDesc.listenerX = listenerX;
    audioDesc.listenerY = listenerY;
    // When a modal state layer pauses the sim clock, freeze non-UI audio channels (bitmask 0x7E)
    audioDesc.frozenChannelMask = static_cast<u8>(
        m_rt->time.FrozenMask() | (m_statePausedClock != 0U ? 0x7EU : 0U));
    m_rt->audio.Update(audioDesc);

    // Drain any asset scopes confirmed unused by AudioMixer back into AssetBank
    u32 releasableMask = m_rt->audio.PollReleasableScopeMask();
    while (releasableMask != 0U) {
        const u8 sc = static_cast<u8>(std::countr_zero(releasableMask));
        releasableMask &= (releasableMask - 1U);
        m_rt->assetBank.EndScope(sc);
    }
}

void Application::ExtractAndCompileRenderFrame() noexcept {
    if (!m_rt) return;

    const RenderTimeView renderView = m_rt->time.GetRenderView();
    const f32 defaultPpm = static_cast<f32>(m_pixelsPerMeterQ4) * (1.0f / 16.0f);

    m_rt->renderList.Reset(m_rt->viewportSize);
    WorldExtractor::Extract(
        m_rt->world, m_rt->assetBank, renderView, m_rt->renderList, defaultPpm);

    RenderContext renderCtx{};
    renderCtx.world = &m_rt->world;
    renderCtx.physics = &m_rt->physics;
    renderCtx.gameplay = &m_rt->gameplay;
    renderCtx.assetBank = &m_rt->assetBank;
    renderCtx.renderList = &m_rt->renderList;
    renderCtx.frameInput = &m_rt->input.GetFrameView();
    renderCtx.timeView = &renderView;
    renderCtx.director = &m_rt->director;
    m_rt->director.DispatchRenderExtract(renderCtx);
    m_rt->director.EmitTransitionOverlay(m_rt->renderList, m_rt->viewportSize);

    m_rt->activeEmitterCount = m_rt->audio.ExtractActiveEmitters(m_rt->audioEmitterScratch);

    DebugFrameContext dbgCtx{};
    dbgCtx.world = &m_rt->world;
    dbgCtx.physics = &m_rt->physics;
    dbgCtx.gameplay = &m_rt->gameplay;
    dbgCtx.timeView = &renderView;
    dbgCtx.audioEmitters = std::span<const AudioEmitterView>{
        m_rt->audioEmitterScratch.data(),
        std::min<usize>(m_rt->activeEmitterCount, m_rt->audioEmitterScratch.size())};
    dbgCtx.viewportSize = m_rt->viewportSize;
    dbgCtx.currentTickIndex = static_cast<u32>(m_rt->time.Telemetry().totalSimTicks);
    dbgCtx.isPaused = (m_rt->time.IsPaused() || m_statePausedClock != 0U) ? 1U : 0U;

    m_rt->debug.EmitOverlays(dbgCtx, m_rt->renderList);
    m_rt->renderList.Compile();

    const RenderStats& rStats = m_rt->renderList.Stats();
    SubsystemTelemetryDesc tele{};
    tele.channelScales = m_rt->time.EffectiveScales().data();
    tele.aliveEntities = static_cast<u16>(std::min<u32>(65535U, m_rt->world.AliveCount()));
    tele.submittedQuads = static_cast<u16>(std::min<u32>(65535U, rStats.submittedQuads));
    tele.culledQuads = static_cast<u16>(std::min<u32>(65535U, rStats.culledQuads));
    tele.drawBatches = static_cast<u16>(std::min<u32>(65535U, rStats.drawBatches));
    tele.activeContacts = static_cast<u16>(std::min<usize>(65535U, m_rt->physics.ActiveManifolds().size()));
    tele.catchupTicks = m_rt->time.Telemetry().ticksLastFrame;
    tele.audioActiveVoices = m_rt->audio.Telemetry().activeVoiceCount;
    tele.audioVirtualVoices = m_rt->audio.Telemetry().virtualVoiceCount;
    tele.audioStolenVoices = m_rt->audio.Telemetry().stolenVoiceCount;
    tele.frozenChannelMask = static_cast<u8>(
        m_rt->time.FrozenMask() | (m_statePausedClock != 0U ? 0x7EU : 0U));
    tele.radixPasses = rStats.radixPassesRun;
    tele.audioMusicStems = static_cast<u8>(m_rt->audio.Telemetry().activeMusicStems);
    m_rt->debug.Profiler().RecordSubsystemTelemetry(tele);
    m_rt->debug.Profiler().EndFrame();
}

Result<void, FrameworkError> Application::StepFrame(u64 rawDeltaNs) noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    m_rt->assetBank.AdvanceFrame(static_cast<u32>(++m_frameIndex));
    if (m_rt->assetBank.IsOverVramBudget()) {
        (void)m_rt->assetBank.EvictLruToVramBudget({});
    }

    SceneLoadContext loadCtx = MakeLoadContext();
    if (auto r = m_rt->director.CommitSafeFramePoint(loadCtx); !r) {
        return Err(FrameworkError{r.error(), SubSystemStage::SceneDirector, 0U});
    }

    DebugFrameContext dbgBeginCtx{};
    dbgBeginCtx.world = &m_rt->world;
    dbgBeginCtx.physics = &m_rt->physics;
    dbgBeginCtx.gameplay = &m_rt->gameplay;
    dbgBeginCtx.viewportSize = m_rt->viewportSize;
    dbgBeginCtx.currentTickIndex = static_cast<u32>(m_rt->time.Telemetry().totalSimTicks);
    dbgBeginCtx.isPaused = m_rt->time.IsPaused() ? 1U : 0U;

    const DebugLoopDirectives dir = m_rt->debug.BeginFrame(dbgBeginCtx);
    if (dir.TogglePause()) {
        m_rt->time.SetPaused(!m_rt->time.IsPaused());
    }
    if (dir.ForcePause()) {
        m_rt->time.SetPaused(true);
    }
    if (dir.stepTicks > 0U) {
        m_rt->time.RequestStep(dir.stepTicks);
    }
    if (dir.channelTimeScale >= 0.0f) {
        m_rt->time.SetBaseScale(static_cast<TimeChannel>(dir.targetTimeChannel & 7U), dir.channelTimeScale);
    }
    if (dir.ResetRequested() && m_rt->director.ActiveSceneId().IsValid()) {
        (void)m_rt->director.ReloadActiveScene(m_rt->assetBank);
    }

    // Track modal StateLayerFlags::PauseSimClock edge without permanently locking TimePipeline::IsPaused().
    // While m_statePausedClock != 0, UI & unblocked top modal state layers continue ticking at 120 Hz while World/Gameplay/Physics freeze.
    m_statePausedClock = m_rt->director.IsClockPausedByState() ? 1U : 0U;

    m_rt->time.BeginFrameManual(rawDeltaNs);

    SimContext simCtx{};
    simCtx.world = &m_rt->world;
    simCtx.physics = &m_rt->physics;
    simCtx.gameplay = &m_rt->gameplay;
    simCtx.assetBank = &m_rt->assetBank;
    simCtx.audio = &m_rt->audio;
    simCtx.actionBuffer = &m_rt->actions;
    simCtx.director = &m_rt->director;

    while (SimStepView step = m_rt->time.PollTick()) {
        ++m_totalSimTicks;
        m_rt->input.SetActionMask(m_rt->director.TopInputMask());

        TickInput tickIn{};
        if (m_replayMode == ReplayMode::Playback && m_rt->replayStream != nullptr) {
            if (!m_rt->replayStream->SamplePlaybackTick(step.tickIndex, tickIn, m_rt->replayPrevHeld)) {
                tickIn = m_rt->input.PollTick(step.tickIndex);
            }
        } else {
            tickIn = m_rt->input.PollTick(step.tickIndex);
            if (m_replayMode == ReplayMode::Record && m_rt->replayStream != nullptr) {
                m_rt->replayStream->RecordTick(tickIn);
            }
        }

        m_rt->actions.Feed(tickIn, m_statePausedClock != 0U);
        simCtx.tickInput = &tickIn;

        if (m_statePausedClock == 0U) {
            m_rt->world.BeginTick(step);
            m_rt->director.DispatchSimTick(simCtx, step);
            m_rt->gameplay.Tick(m_rt->world, m_rt->physics, m_rt->assetBank, step, tickIn, m_rt->actions);
            m_rt->world.EndTick(step);
        } else {
            // Modal pause active: dispatch unblocked top modal overlay layer(s) while freezing World/Physics/Gameplay
            m_rt->director.DispatchSimTick(simCtx, step);
        }

        RouteSimCuesAndSyncAudio(simCtx, step);
        m_rt->director.StepTransitionWipeTick();
        m_rt->debug.EndSimTick(m_rt->world, static_cast<u32>(step.tickIndex), &tickIn, &m_rt->actions);
    }

    ExtractAndCompileRenderFrame();
    m_lastCombinedAuditHash = ComputeFrameAuditDigest().combinedHash;
    return {};
}

Result<FrameAuditDigest, FrameworkError> Application::StepHeadless(
    u32 tickCount,
    u32 audioBlocksPerTick) noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    const u32 steps = std::max<u32>(1U, tickCount);
    for (u32 i = 0U; i < steps; ++i) {
        if (auto r = StepFrame(kFixedDtNs); !r) {
            return Err(r.error());
        }
        if (audioBlocksPerTick > 0U) {
            m_rt->audio.PumpHeadlessBlocks(audioBlocksPerTick);
        }
    }
    return ComputeFrameAuditDigest();
}

FrameAuditDigest Application::ComputeFrameAuditDigest() const noexcept {
    if (!m_rt) return {};
    FrameAuditDigest d{};
    d.tickIndex = m_rt->time.Telemetry().totalSimTicks;
    d.worldHash = m_rt->world.ComputeAuditHash();
    d.physicsHash = m_rt->physics.ComputeAuditHash(m_rt->world);
    d.gameplayHash = m_rt->gameplay.ComputeAuditHash(m_rt->world);
    d.audioHash = m_rt->audio.ComputeAuditHash();
    d.renderListHash = m_rt->renderList.ComputeAuditHash();
    d.directorHash = m_rt->director.ComputeAuditHash();
    d.combinedHash = SceneDirector::CombineAuditHashes(
        d.tickIndex,
        d.worldHash,
        d.physicsHash,
        d.gameplayHash,
        d.audioHash,
        d.renderListHash,
        d.directorHash);
    return d;
}

SceneDirector& Application::Director() noexcept { return m_rt->director; }
const SceneDirector& Application::Director() const noexcept { return m_rt->director; }

World& Application::GetWorld() noexcept { return m_rt->world; }
const World& Application::GetWorld() const noexcept { return m_rt->world; }

PhysicsSystem& Application::Physics() noexcept { return m_rt->physics; }
const PhysicsSystem& Application::Physics() const noexcept { return m_rt->physics; }

GameplayPipeline& Application::Gameplay() noexcept { return m_rt->gameplay; }
const GameplayPipeline& Application::Gameplay() const noexcept { return m_rt->gameplay; }

AssetBank& Application::Assets() noexcept { return m_rt->assetBank; }
const AssetBank& Application::Assets() const noexcept { return m_rt->assetBank; }

AudioSystem& Application::Audio() noexcept { return m_rt->audio; }
const AudioSystem& Application::Audio() const noexcept { return m_rt->audio; }

DebugSystem& Application::Debug() noexcept { return m_rt->debug; }
const DebugSystem& Application::Debug() const noexcept { return m_rt->debug; }

TimePipeline& Application::Time() noexcept { return m_rt->time; }
const TimePipeline& Application::Time() const noexcept { return m_rt->time; }

InputPipeline& Application::Input() noexcept { return m_rt->input; }
const InputPipeline& Application::Input() const noexcept { return m_rt->input; }

ActionBuffer& Application::Actions() noexcept { return m_rt->actions; }
const ActionBuffer& Application::Actions() const noexcept { return m_rt->actions; }

RenderList& Application::GetRenderList() noexcept { return m_rt->renderList; }
const RenderList& Application::GetRenderList() const noexcept { return m_rt->renderList; }

} // namespace en

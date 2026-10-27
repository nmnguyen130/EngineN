#include "framework/Application.h"

#include "core/Diagnostics.h"
#include "time/SdlClock.h"
#include "world/WorldExtractor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace en {

// Hot 120 Hz simulation subsystems sit at the front of the slab.
// Renderer (1.39 MB) is placed at the tail inside an union so headless runs skip allocating and zeroing it.
struct alignas(64) Application::AppRuntimeState {
    TimePipeline time{};
    InputPipeline input{};
    ActionBuffer actions{};
    AssetBank assetBank{};
    AudioSystem audio{};
    World world{};
    PhysicsSystem physics{};
    GameplayPipeline gameplay{};
    SceneDirector director{};
    DebugSystem debug{};
    FramePacer framePacer{};
    JobSystem jobs{};
    AppHooks hooks{};
    InputReplayStream* replayStream{nullptr};
    ViewportExtent viewportSize{1280, 720};
    u16 replayPrevHeld{0U};
    u8 windowFocusPaused{0U};
    u8 rendererConstructed{0U};
    std::array<AudioTriggerReq, 64> audioTriggerScratch;
    std::array<AudioEmitterView, 32> audioEmitterScratch;
    Window window{};
    Platform platform{};
    RenderList renderList{};
    union {
        u8 rendererPlaceholder{};
        Renderer renderer;
    };

    AppRuntimeState() noexcept : rendererPlaceholder{0U} {}
    ~AppRuntimeState() noexcept {
        if (rendererConstructed != 0U) {
            std::destroy_at(&renderer);
        }
    }
};

Application::Application(Application&& other) noexcept {
    *this = std::move(other);
}

Application& Application::operator=(Application&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arena = std::move(other.m_arena);
        m_rt = std::exchange(other.m_rt, nullptr);
        m_userAppState = std::exchange(other.m_userAppState, nullptr);
        m_lastCombinedAuditHash = std::exchange(other.m_lastCombinedAuditHash, 0ULL);
        m_frameIndex = std::exchange(other.m_frameIndex, 0ULL);
        m_totalSimTicks = std::exchange(other.m_totalSimTicks, 0ULL);
        m_pixelsPerMeterQ4 = other.m_pixelsPerMeterQ4;
        m_initialized = std::exchange(other.m_initialized, 0U);
        m_running = std::exchange(other.m_running, 0U);
        m_headless = other.m_headless;
        m_debugEnabled = other.m_debugEnabled;
        m_statePausedClock = std::exchange(other.m_statePausedClock, 0U);
        m_replayMode = std::exchange(other.m_replayMode, ReplayMode::None);
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
    if (!m_rt) {
        return;
    }
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

    const bool isHeadless = (cfg.headless != 0U);
    const usize slabBytes = isHeadless ? (offsetof(AppRuntimeState, renderer) + 64U) : sizeof(AppRuntimeState);
    if (!m_arena.Init(slabBytes)) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    u8* mem = m_arena.CarveRawBytes(slabBytes);
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

    const auto fail = [&](FrameworkErrc code, SubSystemStage stage, u16 sub = 0U) noexcept
        -> Result<void, FrameworkError> {
        Shutdown();
        return Err(FrameworkError{code, stage, sub});
    };

    if (!isHeadless) {
        CrashHandler::Install("enginen_crash.log");
        (void)m_rt->jobs.Init(1U);

        if (auto r = m_rt->platform.Init("VeilOfTheSilent", "EngineN", false); !r) {
            return fail(FrameworkErrc::PlatformInitFailed, SubSystemStage::Platform, static_cast<u16>(r.error()));
        }
        WindowConfig wcfg{};
        wcfg.title = "Veil of the Silent";
        wcfg.size = Extent2i{cfg.viewport.w, cfg.viewport.h};
        if (auto r = m_rt->window.Create(wcfg, false); !r) {
            return fail(FrameworkErrc::WindowCreateFailed, SubSystemStage::Window, static_cast<u16>(r.error()));
        }
        std::construct_at(&m_rt->renderer);
        m_rt->rendererConstructed = 1U;
        if (auto r = m_rt->renderer.Init(m_rt->window); !r) {
            return fail(FrameworkErrc::RendererInitFailed, SubSystemStage::Renderer, static_cast<u16>(r.error()));
        }
    }

    if (auto r = m_rt->assetBank.Init(cfg.assets); !r) {
        return fail(FrameworkErrc::AssetInitFailed, SubSystemStage::Assets, static_cast<u16>(r.error()));
    }

    if (!isHeadless && m_rt->renderer.IsValid()) {
        m_rt->assetBank.SetGpuTextureDestroyCallback(
            [](TextureHandle gpuTex, void* userCtx) noexcept {
                if (gpuTex != kBuiltinFontTexture) {
                    if (auto* ren = static_cast<Renderer*>(userCtx)) {
                        ren->DestroyTexture(gpuTex);
                    }
                }
            },
            &m_rt->renderer);
    }

    AudioSystemConfig audioCfg = cfg.audio;
    audioCfg.SetHeadless(isHeadless);
    if (auto r = m_rt->audio.Init(audioCfg); !r) {
        return fail(FrameworkErrc::AudioInitFailed, SubSystemStage::Audio, static_cast<u16>(r.error()));
    }

    if (auto r = m_rt->world.Init(cfg.world); !r) {
        return fail(FrameworkErrc::WorldInitFailed, SubSystemStage::World, static_cast<u16>(r.error()));
    }

    if (hooks.onRegisterComponents) {
        if (auto r = hooks.onRegisterComponents(m_rt->world, m_userAppState); !r) {
            return fail(r.error(), SubSystemStage::ClientApp);
        }
    }

    PhysicsConfig physCfg{};
    physCfg.maxManifolds = cfg.maxManifolds;
    physCfg.velocityIterations = cfg.velocityIterations;
    physCfg.sleepTicksThreshold = cfg.sleepTicksThreshold;
    if (auto r = m_rt->physics.Init(m_rt->world, physCfg); !r) {
        return fail(FrameworkErrc::PhysicsInitFailed, SubSystemStage::Physics, static_cast<u16>(r.error()));
    }

    GameplayConfig gpCfg{};
    gpCfg.maxCuesPerTick = cfg.maxCuesPerTick;
    if (auto r = m_rt->gameplay.Init(m_rt->world, hooks.gameplayPolicies, gpCfg); !r) {
        return fail(FrameworkErrc::GameplayInitFailed, SubSystemStage::Gameplay, static_cast<u16>(r.error()));
    }

    if (m_debugEnabled != 0U) {
        DebugSystemConfig dbgCfg = cfg.debug;
        if (isHeadless) {
            dbgCfg.initialOverlays = OverlayFlags::None;
        }
        if (auto r = m_rt->debug.Init(dbgCfg); !r) {
            return fail(FrameworkErrc::DebugInitFailed, SubSystemStage::Debug, static_cast<u16>(r.error()));
        }
    }

    if (auto r = m_rt->director.Init(m_rt->assetBank); !r) {
        return fail(r.error(), SubSystemStage::SceneDirector);
    }

    m_initialized = 1U;
    m_running = 1U;

    SceneLoadContext loadCtx = MakeLoadContext();
    if (hooks.onBoot) {
        if (auto r = hooks.onBoot(loadCtx); !r) {
            return fail(r.error(), SubSystemStage::ClientApp);
        }
    }

    if (auto r = m_rt->director.CommitSafeFramePoint(loadCtx); !r) {
        return fail(r.error(), SubSystemStage::SceneDirector);
    }

    return {};
}

void Application::Shutdown() noexcept {
    if (m_rt) {
        if (m_initialized != 0U) {
            SceneLoadContext loadCtx = MakeLoadContext();
            if (m_rt->hooks.onPreShutdown) {
                m_rt->hooks.onPreShutdown(loadCtx);
            }
            m_rt->director.Shutdown(m_rt->assetBank, m_rt->audio, &loadCtx);
        } else {
            m_rt->director.Shutdown(m_rt->assetBank, m_rt->audio, nullptr);
        }

        if (m_debugEnabled != 0U) {
            m_rt->debug.Shutdown();
        }
        m_rt->gameplay.Shutdown();
        m_rt->physics.Shutdown();
        m_rt->world.Shutdown();
        m_rt->audio.Shutdown();
        m_rt->assetBank.Shutdown();
        if (m_headless == 0U) {
            m_rt->jobs.Shutdown();
            if (m_rt->rendererConstructed != 0U && m_rt->renderer.IsValid()) {
                m_rt->renderer.Shutdown();
            }
            m_rt->window.Destroy();
            if (m_rt->platform.IsInitialized()) {
                m_rt->platform.Shutdown();
            }
        }
        m_rt->~AppRuntimeState();
        m_rt = nullptr;
    }
    m_arena.Shutdown();
    m_initialized = 0U;
    m_running = 0U;
    m_statePausedClock = 0U;
    m_replayMode = ReplayMode::None;
}

void Application::UploadCommittedTexturesToGpu(u16 customFontTexSlot) noexcept {
    if (!m_rt || m_headless != 0U || m_rt->rendererConstructed == 0U || !m_rt->renderer.IsValid()) {
        return;
    }

    AssetBank& bank = m_rt->assetBank;
    if (customFontTexSlot != 0xFFFFU && customFontTexSlot < bank.HighWaterTex()) {
        const TextureRecord& fontRec = bank.GetTextureBySlot(customFontTexSlot);
        if (fontRec.pixels != nullptr && fontRec.width > 0U && fontRec.height > 0U) {
            if (m_rt->renderer.UpdateTexture(
                    kBuiltinFontTexture,
                    Rect{0.0f, 0.0f, static_cast<f32>(fontRec.width), static_cast<f32>(fontRec.height)},
                    std::span<const u8>{fontRec.pixels, static_cast<usize>(fontRec.width) * fontRec.height * 4U})) {
                bank.BindGpuTexture(customFontTexSlot, kBuiltinFontTexture);
            }
        }
    }

    for (u16 slot = 1U; slot < bank.HighWaterTex(); ++slot) {
        if (slot == customFontTexSlot) {
            continue;
        }
        const TextureRecord& rec = bank.GetTextureBySlot(slot);
        if (rec.pixels != nullptr && rec.width > 0U && rec.height > 0U &&
            (rec.gpu == kWhiteTexture || !rec.gpu.IsValid())) {
            TextureDesc tdesc{};
            tdesc.width = rec.width;
            tdesc.height = rec.height;
            tdesc.filter = TextureFilter::PixelArt;
            tdesc.pixels = std::span<const u8>{rec.pixels, static_cast<usize>(rec.width) * rec.height * 4U};
            if (auto gpuRes = m_rt->renderer.CreateTexture(tdesc); gpuRes.has_value()) {
                bank.BindGpuTexture(slot, *gpuRes);
            }
        }
    }
}

Result<void, FrameworkError> Application::Run(u32 targetFps) noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }
    if (m_headless != 0U) {
        auto res = StepHeadless(120U, 2U);
        if (!res) {
            return Err(res.error());
        }
        return {};
    }

    m_rt->framePacer.SetTargetFps(targetFps);
    u64 prevNs = SdlClock::NowNs();

    while (m_running != 0U && !m_rt->platform.ShouldQuit()) {
        m_rt->framePacer.BeginFrame();
        const u64 nowNs = SdlClock::NowNs();
        const u64 deltaNs = (nowNs > prevNs) ? (nowNs - prevNs) : kFixedDtNs;
        prevNs = nowNs;

        const WindowFlags winFlags = m_rt->platform.Pump(m_rt->window, m_rt->input);
        if (HasFlag(winFlags, WindowFlags::CloseRequested)) {
            RequestQuit();
            break;
        }

        if (HasFlag(winFlags, WindowFlags::Resized)) {
            const ViewportExtent newVp = m_rt->window.State().pixelSize;
            if (newVp.IsValid()) {
                m_rt->viewportSize = newVp;
                if (m_rt->rendererConstructed != 0U && m_rt->renderer.IsValid()) {
                    m_rt->renderer.ResizeViewport(newVp);
                }
            }
        }

        if (HasFlag(winFlags, WindowFlags::Minimized | WindowFlags::FocusLost)) {
            if (m_rt->windowFocusPaused == 0U) {
                m_rt->windowFocusPaused = 1U;
                m_rt->time.SetPaused(true);
                m_rt->audio.SetFocusMuted(true);
            }
        } else if (HasFlag(winFlags, WindowFlags::Restored | WindowFlags::FocusGained)) {
            if (m_rt->windowFocusPaused != 0U) {
                m_rt->windowFocusPaused = 0U;
                m_rt->time.OnWindowRefocus();
                m_rt->time.SetPaused(false);
                m_rt->audio.SetFocusMuted(false);
                prevNs = SdlClock::NowNs();
            }
        }

        if (m_debugEnabled != 0U) {
            const bool wasConsoleOpen = m_rt->debug.Console().IsOpen();
            const PlatformFrameEvents& pEvents = m_rt->platform.FrameEvents();
            for (const PlatformInputEvent& ev : pEvents.Events()) {
                if (ev.Kind() == InputEventKind::Key) {
                    (void)m_rt->debug.OnPlatformKeyEvent(ev.Code(), ev.Down(), false, false, m_rt->input);
                }
            }
            const bool isConsoleOpen = m_rt->debug.Console().IsOpen();
            if (isConsoleOpen != wasConsoleOpen) {
                m_rt->window.SetTextInput(isConsoleOpen);
            }
            const std::string_view text = pEvents.TextView();
            if (!text.empty()) {
                (void)m_rt->debug.OnPlatformTextInput(text);
            }
        }

        if (auto r = StepFrame(deltaNs); !r) {
            return r;
        }

        m_rt->framePacer.EndFrameAndPace();
    }

    return {};
}

void Application::RouteSimCuesAndSyncAudio(SimContext& simCtx, const SimStepView& step) noexcept {
    if (!m_rt) {
        return;
    }

    u32 audioTrigCount = 0U;
    const std::span<const SimCue> cues = m_rt->gameplay.DrainCues();

    CameraController2D* cam = nullptr;
    if ((m_rt->world.RegisteredPoolsMask() & MakeComponentMask<CameraController2D>()) != 0ULL) {
        auto& camPool = m_rt->world.GetPool<CameraController2D>();
        if (camPool.Size() > 0U) {
            cam = &camPool.ComponentAtDense(0U);
        }
    }

    for (const SimCue& cue : cues) {
        switch (cue.kind) {
            case SimCueKind::AudioOneShot: {
                if (audioTrigCount < m_rt->audioTriggerScratch.size()) {
                    f32 cueX = cue.worldX;
                    f32 cueY = cue.worldY;
                    if (cueX == 0.0f && cueY == 0.0f &&
                        cue.emitter.IsValid() && m_rt->world.IsAlive(cue.emitter)) {
                        const WorldTransform2D& wt = m_rt->world.GetWorldByIndex(cue.emitter.index);
                        cueX = wt.x;
                        cueY = wt.y;
                    }
                    m_rt->audioTriggerScratch[audioTrigCount++] = AudioTriggerReq{
                        SoundCueId::FromAssetId(cue.assetId),
                        cueX,
                        cueY,
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
                if (cam != nullptr) {
                    cam->AddTrauma(cue.Magnitude());
                }
                break;
            }
            case SimCueKind::HitstopFreeze: {
                if (cue.channel == 0xFFU) {
                    m_rt->time.RequestGameplayFreeze(cue.durationTicks);
                } else if (cue.userTag == 0U && cue.channel != static_cast<u8>(TimeChannel::World)) {
                    m_rt->time.RequestHitstop(static_cast<TimeChannel>(cue.channel & 7U), cue.durationTicks);
                }
                break;
            }
            case SimCueKind::GamepadRumble: {
                if (m_headless == 0U && m_rt->platform.IsInitialized()) {
                    const f32 mag = cue.Magnitude();
                    (void)m_rt->platform.RumbleGamepad(
                        0U, mag, mag, static_cast<u32>(cue.durationTicks) * 8U);
                }
                break;
            }
            case SimCueKind::CustomGameEvent:
                break;
        }

        if (m_rt->hooks.onConsumeSimCue) {
            m_rt->hooks.onConsumeSimCue(cue, simCtx, m_userAppState);
        }
    }

    AudioUpdateDesc audioDesc{};
    audioDesc.triggers = {m_rt->audioTriggerScratch.data(), audioTrigCount};
    audioDesc.assetBank = &m_rt->assetBank;
    audioDesc.occlusionQuery = [](f32 lx, f32 ly, f32 ex, f32 ey, void* userCtx) noexcept -> bool {
        const auto* phys = static_cast<const PhysicsSystem*>(userCtx);
        if (!phys) {
            return false;
        }
        const f32 dx = ex - lx;
        const f32 dy = ey - ly;
        const f32 distSq = dx * dx + dy * dy;
        if (distSq <= 1e-6f) {
            return false;
        }
        const f32 dist = std::sqrt(distSq);
        const f32 invDist = 1.0f / dist;
        return phys->Raycast(lx, ly, dx * invDist, dy * invDist, dist, 0x0001U).Hit();
    };
    audioDesc.occlusionUserCtx = &m_rt->physics;
    audioDesc.channelScales = m_rt->time.EffectiveScales().data();
    audioDesc.tickIndex = static_cast<u32>(step.tickIndex);
    audioDesc.listenerX = (cam != nullptr) ? cam->anchorX : 0.0f;
    audioDesc.listenerY = (cam != nullptr) ? cam->anchorY : 0.0f;
    audioDesc.frozenChannelMask = static_cast<u8>(
        m_rt->time.FrozenMask() | (m_statePausedClock != 0U ? kGameplayChannelsMask : 0U));
    m_rt->audio.Update(audioDesc);

    for (u32 releasable = m_rt->audio.PollReleasableScopeMask(); releasable != 0U; releasable &= (releasable - 1U)) {
        m_rt->assetBank.EndScope(static_cast<u8>(std::countr_zero(releasable)));
    }
}

void Application::ExtractAndCompileRenderFrame() noexcept {
    if (!m_rt) {
        return;
    }

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

    if (m_debugEnabled == 0U) {
        m_rt->renderList.Compile();
        return;
    }

    const u16 emitterCount = HasFlag(m_rt->debug.ActiveOverlays(), OverlayFlags::AudioEmitters)
                                 ? m_rt->audio.ExtractActiveEmitters(m_rt->audioEmitterScratch)
                                 : 0U;

    const TimeTelemetry& timeTele = m_rt->time.Telemetry();

    DebugFrameContext dbgCtx{};
    dbgCtx.world = &m_rt->world;
    dbgCtx.physics = &m_rt->physics;
    dbgCtx.gameplay = &m_rt->gameplay;
    dbgCtx.timeView = &renderView;
    dbgCtx.audioEmitters = std::span<const AudioEmitterView>{
        m_rt->audioEmitterScratch.data(), std::min<usize>(emitterCount, m_rt->audioEmitterScratch.size())};
    dbgCtx.viewportSize = m_rt->viewportSize;
    dbgCtx.currentTickIndex = static_cast<u32>(timeTele.totalSimTicks);
    dbgCtx.isPaused = (m_rt->time.IsPaused() || m_statePausedClock != 0U) ? 1U : 0U;

    m_rt->debug.EmitOverlays(dbgCtx, m_rt->renderList);
    m_rt->renderList.Compile();

    u32 activeMotorContacts = 0U;
    if ((m_rt->world.RegisteredPoolsMask() & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
        for (const CharacterMotor2D& m : m_rt->world.GetPool<CharacterMotor2D>().Components()) {
            activeMotorContacts += static_cast<u32>(m.contacts != ContactFlags::None);
        }
    }

    const RenderStats& rStats = m_rt->renderList.Stats();
    const AudioTelemetrySnapshot& audioTele = m_rt->audio.Telemetry();

    SubsystemTelemetryDesc tele{};
    tele.channelScales = m_rt->time.EffectiveScales().data();
    tele.aliveEntities = static_cast<u16>(std::min<u32>(65535U, m_rt->world.AliveCount()));
    tele.submittedQuads = static_cast<u16>(std::min<u32>(65535U, rStats.submittedQuads));
    tele.culledQuads = static_cast<u16>(std::min<u32>(65535U, rStats.culledQuads));
    tele.drawBatches = static_cast<u16>(std::min<u32>(65535U, rStats.drawBatches));
    tele.activeContacts = static_cast<u16>(
        std::min<usize>(65535U, m_rt->physics.ActiveManifolds().size() + activeMotorContacts));
    tele.catchupTicks = timeTele.ticksLastFrame;
    tele.audioActiveVoices = audioTele.activeVoiceCount;
    tele.audioVirtualVoices = audioTele.virtualVoiceCount;
    tele.audioStolenVoices = audioTele.stolenVoiceCount;
    tele.frozenChannelMask = static_cast<u8>(
        m_rt->time.FrozenMask() | (m_statePausedClock != 0U ? kGameplayChannelsMask : 0U));
    tele.radixPasses = rStats.radixPassesRun;
    tele.audioMusicStems = static_cast<u8>(audioTele.activeMusicStems);
    m_rt->debug.Profiler().RecordSubsystemTelemetry(tele);
    m_rt->debug.Profiler().EndFrame();
}

Result<void, FrameworkError> Application::StepFrame(u64 rawDeltaNs) noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(FrameworkError{FrameworkErrc::InvalidConfig, SubSystemStage::None, 0U});
    }

    m_lastCombinedAuditHash = 0ULL;
    m_rt->assetBank.AdvanceFrame(static_cast<u32>(++m_frameIndex));
    if (m_rt->assetBank.IsOverVramBudget()) {
        (void)m_rt->assetBank.EvictLruToVramBudget({});
    }
    if (m_headless == 0U && m_rt->rendererConstructed != 0U && m_rt->renderer.IsValid() &&
        m_rt->assetBank.PendingGpuTextureCount() > 0U) {
        UploadCommittedTexturesToGpu();
    }

    if (m_rt->director.HasPendingWork()) {
        SceneLoadContext loadCtx = MakeLoadContext();
        if (auto r = m_rt->director.CommitSafeFramePoint(loadCtx); !r) {
            return Err(FrameworkError{r.error(), SubSystemStage::SceneDirector, 0U});
        }
    }

    if (m_debugEnabled != 0U) {
        DebugFrameContext dbgBeginCtx{};
        dbgBeginCtx.world = &m_rt->world;
        dbgBeginCtx.physics = &m_rt->physics;
        dbgBeginCtx.gameplay = &m_rt->gameplay;
        dbgBeginCtx.viewportSize = m_rt->viewportSize;
        dbgBeginCtx.currentTickIndex = static_cast<u32>(m_rt->time.Telemetry().totalSimTicks);
        dbgBeginCtx.isPaused = m_rt->time.IsPaused() ? 1U : 0U;

        const DebugLoopDirectives dir = m_rt->debug.BeginFrame(dbgBeginCtx);
        m_rt->debug.Profiler().RecordScopeUs(
            ProfileScopeId::FrameTotal,
            static_cast<u32>(std::min<u64>(65535ULL, rawDeltaNs / 1000ULL)));

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
    }

    m_statePausedClock = m_rt->director.IsClockPausedByState() ? 1U : 0U;
    if (m_statePausedClock != 0U) {
        m_rt->time.RequestGameplayFreeze(1U);
    }

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
        if (m_statePausedClock != 0U) {
            m_rt->time.RequestGameplayFreeze(1U);
        }
        ++m_totalSimTicks;
        m_rt->input.SetActionMask(m_rt->director.TopInputMask());

        TickInput tickIn{};
        if (m_replayMode == ReplayMode::Playback && m_rt->replayStream != nullptr) {
            if (m_rt->replayStream->IsCorrupted() ||
                !m_rt->replayStream->SamplePlaybackTick(step.tickIndex, tickIn, m_rt->replayPrevHeld)) {
                return Err(FrameworkError{
                    FrameworkErrc::ReplayDesyncOrExhausted, SubSystemStage::None, 0U});
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
            m_rt->director.DispatchSimTick(simCtx, step);
        }

        RouteSimCuesAndSyncAudio(simCtx, step);
        m_rt->director.StepTransitionWipeTick();
        if (m_debugEnabled != 0U) {
            m_rt->debug.EndSimTick(m_rt->world, static_cast<u32>(step.tickIndex), &tickIn, &m_rt->actions);
        }
    }

    const bool canPresent = (m_headless == 0U) &&
                            (!m_rt->window.IsValid() || m_rt->window.State().IsPresentable());
    if (canPresent || m_headless != 0U) {
        ExtractAndCompileRenderFrame();
    }
    if (canPresent && m_rt->rendererConstructed != 0U && m_rt->renderer.IsValid()) {
        m_rt->renderer.Submit(m_rt->renderList);
    }
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

Result<usize, WorldErrc> Application::SaveWorldSnapshot(std::span<u8> outBuffer) const noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(WorldErrc::InvalidConfig);
    }
    return m_rt->world.SerializeWorldSnapshot(outBuffer);
}

Result<void, WorldErrc> Application::LoadWorldSnapshot(std::span<const u8> inBuffer) noexcept {
    if (!m_rt || m_initialized == 0U) {
        return Err(WorldErrc::InvalidConfig);
    }
    return m_rt->world.DeserializeWorldSnapshot(inBuffer);
}

FrameAuditDigest Application::ComputeFrameAuditDigest() const noexcept {
    if (!m_rt) {
        return {};
    }
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
    m_lastCombinedAuditHash = d.combinedHash;
    return d;
}

Platform& Application::GetPlatform() noexcept { return m_rt->platform; }
const Platform& Application::GetPlatform() const noexcept { return m_rt->platform; }
Window& Application::GetWindow() noexcept { return m_rt->window; }
const Window& Application::GetWindow() const noexcept { return m_rt->window; }
Renderer& Application::GetRenderer() noexcept { return m_rt->renderer; }
const Renderer& Application::GetRenderer() const noexcept { return m_rt->renderer; }
JobSystem& Application::Jobs() noexcept { return m_rt->jobs; }
const JobSystem& Application::Jobs() const noexcept { return m_rt->jobs; }
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

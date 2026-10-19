#include "framework/Application.h"

#include "world/WorldExtractor.h"

#include <algorithm>
#include <array>
#include <bit>

namespace en::framework {

namespace {

constexpr u32 kMaxAudioTriggersPerFrame = 64;

[[nodiscard]] bool QuerySpatialOcclusion(
    const void* userCtx,
    f32 startX,
    f32 startY,
    f32 dirX,
    f32 dirY,
    f32 maxDist,
    u32 ignoreEmitterId) noexcept {
    const auto* spatial = static_cast<const gameplay::SpatialIndex2D*>(userCtx);
    if (spatial == nullptr) {
        return false;
    }
    const world::EntityId ignoreId = (ignoreEmitterId != audio::kNullEmitter)
        ? world::EntityId{
              .index = static_cast<u16>(ignoreEmitterId & 0xFFFFu),
              .generation = static_cast<u16>(ignoreEmitterId >> 16u)}
        : world::kNullEntity;
    return spatial->Raycast(startX, startY, dirX, dirY, maxDist, 0u, ignoreId).hit;
}

} // namespace

constexpr SceneLoadContext Application::MakeLoadContext() noexcept {
    return SceneLoadContext{
        .world = m_world,
        .gameplay = m_gameplay,
        .assets = m_assets,
        .audio = m_audio,
        .renderer = m_headless ? nullptr : &m_renderer,
        .userAppState = m_userAppState,
        .frameIndex = m_frameIndex,
        .sceneScope = m_director.ActiveSceneScope(),
        .persistentScope = m_director.PersistentScope(),
        .reserved = 0,
        .director = &m_director
    };
}

Result<void, FrameworkError> Application::FailInit(
    FrameworkErrc code,
    SubSystemStage stage,
    u8 subErrc) noexcept {
    Shutdown();
    return Err(FrameworkError{code, stage, subErrc, 0});
}

Result<void, FrameworkError> Application::Init(
    const ApplicationConfig& cfg,
    const AppHooks& hooks) noexcept {
    if (m_initialized) {
        Shutdown();
    }

    m_userAppState = hooks.userAppState;
    m_devToolsTap = hooks.devToolsTap;
    m_onConsumeSimCue = hooks.OnConsumeSimCue;
    m_onPreShutdown = hooks.OnPreShutdown;
    m_lastRenderListAuditHash = 0;
    m_assetPumpBudget = cfg.assetPumpBudget;
    m_frameIndex = 0;
    m_headless = cfg.headless;
    m_initialized = true;

    if (!m_headless) {
        if (auto r = m_platform.Init(cfg.appName, cfg.orgName); !r) {
            return FailInit(FrameworkErrc::PlatformInitFailed, SubSystemStage::Platform, static_cast<u8>(r.error()));
        }
        if (auto r = m_window.Create(cfg.window); !r) {
            return FailInit(FrameworkErrc::WindowCreateFailed, SubSystemStage::Window, static_cast<u8>(r.error()));
        }
        if (auto r = m_renderer.Init(m_window, cfg.renderer); !r) {
            return FailInit(FrameworkErrc::RendererInitFailed, SubSystemStage::Renderer, static_cast<u8>(r.error()));
        }
    } else {
        m_renderer.ResizeViewport(cfg.window.size);
    }

    if (auto r = m_assets.Init(cfg.assets); !r) {
        return FailInit(FrameworkErrc::AssetInitFailed, SubSystemStage::Assets, static_cast<u8>(r.error()));
    }

    if (!cfg.bootPakPath.empty() && !m_headless) {
        if (auto pathRes = m_platform.ResolveAsset(cfg.bootPakPath); pathRes) {
            if (auto m = m_assets.MountPackFile(pathRes->View(), 0); !m) {
                return FailInit(FrameworkErrc::AssetInitFailed, SubSystemStage::Assets, static_cast<u8>(m.error()));
            }
        }
    }

    audio::AudioSystemConfig audioCfg = cfg.audio;
    if (m_headless) {
        audioCfg.headlessMode = true;
    }
    if (auto r = m_audio.Init(audioCfg); !r) {
        return FailInit(FrameworkErrc::AudioInitFailed, SubSystemStage::Audio, static_cast<u8>(r.error()));
    }
    if (auto r = m_world.Init(cfg.world); !r) {
        return FailInit(FrameworkErrc::WorldInitFailed, SubSystemStage::World, static_cast<u8>(r.error()));
    }
    if (auto r = m_gameplay.Init(m_world, hooks.gameplayPolicies, cfg.gameplay); !r) {
        return FailInit(FrameworkErrc::GameplayInitFailed, SubSystemStage::Gameplay, static_cast<u8>(r.error()));
    }
    if (!m_headless) {
        if (auto r = m_debug.Init(m_renderer, cfg.debug); !r) {
            return FailInit(FrameworkErrc::DebugInitFailed, SubSystemStage::Debug, static_cast<u8>(r.error()));
        }
    }
    if (hooks.OnRegisterComponents) {
        if (auto r = hooks.OnRegisterComponents(m_world, m_userAppState); !r) {
            return FailInit(FrameworkErrc::ClientBootFailed, SubSystemStage::ClientApp, r.error());
        }
    }
    if (auto r = m_director.Init(m_assets); !r) {
        return FailInit(r.error(), SubSystemStage::SceneDirector);
    }
    if (hooks.OnBoot) {
        SceneLoadContext loadCtx = MakeLoadContext();
        if (auto r = hooks.OnBoot(loadCtx, m_director); !r) {
            return FailInit(FrameworkErrc::ClientBootFailed, SubSystemStage::ClientApp, r.error());
        }
    }

    return {};
}

void Application::Shutdown() noexcept {
    if (!m_initialized) {
        return;
    }

    SceneLoadContext loadCtx = MakeLoadContext();
    if (m_onPreShutdown) {
        m_onPreShutdown(loadCtx);
    }

    m_director.Shutdown(m_assets, m_audio, &loadCtx);
    m_debug.Shutdown(m_headless ? nullptr : &m_renderer);
    m_gameplay.Shutdown();
    m_world.Shutdown();
    m_audio.Shutdown();
    m_assets.Shutdown();

    if (!m_headless) {
        m_renderer.Shutdown();
        m_window.Destroy();
        m_platform.Shutdown();
    }

    m_initialized = false;
}

Result<void, FrameworkError> Application::Run() noexcept {
    while (!m_platform.ShouldQuit()) {
        if (auto res = StepFrame(); !res) {
            return res;
        }
    }
    return {};
}

Result<void, FrameworkError> Application::CommitSafeFrameBarrier() noexcept {
    SceneLoadContext loadCtx = MakeLoadContext();
    auto commitRes = m_director.CommitSafeFramePoint(loadCtx);
    if (!commitRes) {
        return Err(FrameworkError{
            commitRes.error(), SubSystemStage::SceneDirector, 0});
    }

    m_input.SetActionMask(m_director.TopInputMask());
    m_time.SetPaused(m_director.IsClockPausedByState());

    if (*commitRes) {
        m_time.OnWindowRefocus();
        m_actionBuffer.Clear();
    }
    return {};
}

Result<void, FrameworkError> Application::StepFrame() noexcept {
    ++m_frameIndex;

    const platform::WindowFlags winFlags =
        m_platform.Pump(m_window, m_input, m_devToolsTap);
    if (m_platform.ShouldQuit()) {
        return {};
    }

    if (platform::HasFlag(winFlags, platform::WindowFlags::FocusGained)) {
        m_time.OnWindowRefocus();
    }
    if (platform::HasFlag(winFlags, platform::WindowFlags::FocusLost)) {
        m_actionBuffer.Clear();
    }

    m_assets.Pump(m_renderer, m_assetPumpBudget);

    const debug::DebugFrameContext dbgBeginCtx{
        .world = m_world,
        .timeView = m_time.GetRenderView(),
        .frameInput = m_input.GetFrameView(),
        .viewportSize = m_window.State().pixelSize,
        .audioEmitters = {},
        .isPaused = m_time.IsPaused()
    };
    const debug::DebugLoopDirectives dbgDir = m_debug.BeginFrame(dbgBeginCtx);

    if (m_director.HasPendingWork()) {
        if (auto res = CommitSafeFrameBarrier(); !res) {
            return res;
        }
    }

    if (dbgDir.ForcePause()) {
        m_time.SetPaused(true);
    } else if (dbgDir.TogglePause()) {
        m_time.SetPaused(!m_time.IsPaused());
    }
    if (dbgDir.stepTicks > 0) {
        m_time.RequestStep(dbgDir.stepTicks);
    }
    if (dbgDir.targetTimeChannel < time::kMaxTimeChannels) {
        m_time.Modulator().SetBaseScale(dbgDir.targetTimeChannel, dbgDir.channelTimeScale);
    }

    m_time.BeginFrame();
    const u32 ticksExecuted = ExecuteSimTicks();

    RouteCuesAndSyncAudio(ticksExecuted);

    if (m_window.State().IsPresentable()) {
        renderer::RenderList& list = ExtractRenderFrame(m_window.State().pixelSize);

#if EN_ENABLE_DEBUG_TOOLS
        if (m_debug.ActiveOverlays() != debug::OverlayFlags::None) {
            std::array<debug::DebugAudioEmitter, 64> emitters{};
            u32 emitterCount = 0;
            if (debug::HasOverlay(m_debug.ActiveOverlays(), debug::OverlayFlags::AudioEmitters)) {
                std::span<audio::AudioEmitterView> emitterView{
                    reinterpret_cast<audio::AudioEmitterView*>(emitters.data()),
                    emitters.size()
                };
                emitterCount = m_audio.ExtractActiveEmitters(emitterView);
            }

            const auto& renderStats = list.Stats();
            const auto audioTel = m_audio.Telemetry();
            m_debug.Profiler().RecordSubsystemTelemetry(debug::SubsystemTelemetryDesc{
                .channelScales = m_time.EffectiveScales(),
                .audioCpuLoadRatio = audioTel.callbackCpuLoadRatio,
                .audioPeakL = audioTel.masterPeakL,
                .audioPeakR = audioTel.masterPeakR,
                .aliveEntities = static_cast<u16>(m_world.AliveCount()),
                .submittedQuads = static_cast<u16>(renderStats.submittedQuads),
                .culledQuads = static_cast<u16>(renderStats.culledQuads),
                .drawBatches = static_cast<u16>(renderStats.drawBatches),
                .catchupTicks = static_cast<u8>(ticksExecuted),
                .radixPasses = static_cast<u8>(renderStats.radixPassesRun),
                .frozenChannelMask = m_time.FrozenMask(),
                .activePhysVoices = static_cast<u8>(std::popcount(audioTel.activeVoiceMask)),
                .activeVirtVoices = audioTel.activeVirtualVoices,
                .audioUnderruns = static_cast<u8>(audioTel.underrunCount)
            });

            const debug::DebugFrameContext dbgOverlayCtx{
                .world = m_world,
                .timeView = m_time.GetRenderView(),
                .frameInput = m_input.GetFrameView(),
                .viewportSize = m_window.State().pixelSize,
                .audioEmitters = std::span<const debug::DebugAudioEmitter>{emitters.data(), emitterCount},
                .isPaused = m_time.IsPaused()
            };
            m_debug.EmitOverlays(dbgOverlayCtx, list);
        }
#endif

        m_renderer.Submit(list);
    }

    return {};
}

Result<FrameAuditDigest, FrameworkError> Application::StepHeadless(
    u32 tickCount,
    u32 audioBlocksPerTick) noexcept {
    for (u32 t = 0; t < tickCount; ++t) {
        ++m_frameIndex;
        m_assets.PumpHeadless(m_assetPumpBudget);

        if (m_director.HasPendingWork()) {
            if (auto res = CommitSafeFrameBarrier(); !res) {
                return Err(res.error());
            }
        }

        m_time.BeginFrameManual(time::kFixedDtNs);
        const u32 ticksExecuted = ExecuteSimTicks();

        RouteCuesAndSyncAudio(ticksExecuted);

        if (audioBlocksPerTick > 0) {
            m_audio.PumpHeadlessBlocks(audioBlocksPerTick);
        }
    }

    renderer::RenderList& list = ExtractRenderFrame({});
    list.Compile();
    m_lastRenderListAuditHash = list.ComputeAuditHash();

    return ComputeFrameAuditDigest();
}

u32 Application::ExecuteSimTicks() noexcept {
    SimContext simCtx{
        .world = m_world,
        .gameplay = m_gameplay,
        .assetBank = m_assets.Bank(),
        .audio = m_audio,
        .platform = m_platform,
        .actionBuffer = m_actionBuffer,
        .tickInput = nullptr,
        .userState = nullptr,
        .step = {},
        .director = &m_director
    };

    u32 count = 0;
    while (const auto step = m_time.PollTick()) {
        ++count;
        const input::TickInput tickInput = m_input.PollTick(step.TickIndex());
        m_actionBuffer.Feed(tickInput, step.IsFrozen(time::TimeChannel::Player));

        m_world.BeginTick(step);
        m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);

        if (m_director.ActiveSimMask() != 0) {
            simCtx.step = step;
            simCtx.tickInput = &tickInput;
            m_director.DispatchSimTick(simCtx);
        }

        m_world.EndTick(step);
        m_debug.EndSimTick(m_world, step, tickInput, m_actionBuffer);
    }

    return count;
}

void Application::RouteCuesAndSyncAudio(u32 ticksExecuted) noexcept {
    for (u32 relMask = m_audio.PollReleasableScopeMask(); relMask != 0u; relMask &= (relMask - 1u)) {
        m_assets.EndScope(static_cast<assets::ScopeId>(std::countr_zero(relMask)));
    }

    world::EntityId activeCam = world::kNullEntity;
    f32 listenerX = 0.0f;
    f32 listenerY = 0.0f;
    const auto& camPool = m_world.GetPool<world::CameraRig2D>();
    const auto camEntities = camPool.Entities();
    const auto camRigs = camPool.Components();
    for (usize i = 0; i < camEntities.size(); ++i) {
        if (camRigs[i].active && camRigs[i].viewSlot == 0 && !camRigs[i].screenSpace) {
            activeCam = camEntities[i];
            const auto& wt = m_world.GetWorldByIndex(activeCam.index);
            listenerX = wt.x;
            listenerY = wt.y;
            break;
        }
    }

    std::array<audio::AudioTriggerReq, kMaxAudioTriggersPerFrame> audioTriggers{};
    u32 triggerCount = 0;

    if (ticksExecuted > 0) {
        const auto cues = m_gameplay.DrainCues();
        if (!cues.empty()) {
            const assets::AssetBank& bank = m_assets.Bank();
            for (const auto& cue : cues) {
                switch (cue.kind) {
                    case gameplay::SimCueKind::AudioOneShot:
                        if (triggerCount < kMaxAudioTriggersPerFrame) {
                            audioTriggers[triggerCount++] = audio::AudioTriggerReq{
                                .cueId = audio::SoundCueId::FromAssetId(cue.assetId),
                                .worldX = cue.worldX,
                                .worldY = cue.worldY,
                                .emitterId = (cue.emitter != world::kNullEntity)
                                                 ? (static_cast<u32>(cue.emitter.generation) << 16u) | cue.emitter.index
                                                 : audio::kNullEmitter
                            };
                        }
                        break;

                    case gameplay::SimCueKind::CameraTraumaPulse:
                        if (activeCam != world::kNullEntity) {
                            if (auto* ctrl = m_world.TryGet<gameplay::CameraController2D>(activeCam)) {
                                const u32 sum = static_cast<u32>(ctrl->trauma_q16) + cue.magnitude_q16;
                                ctrl->trauma_q16 = static_cast<u16>(std::min<u32>(sum, 0xFFFFu));
                            }
                        }
                        break;

                    case gameplay::SimCueKind::GamepadRumblePulse:
                        if (!m_headless && cue.durationTicks > 0) {
                            const u32 durationMs = (static_cast<u32>(cue.durationTicks) * 1000u) / time::kSimHz;
                            const u8 padSlot = static_cast<u8>(cue.channel & 0x03u);
                            (void)m_platform.RumbleGamepad(
                                padSlot, cue.magnitude_q16, cue.magnitude_q16, std::max<u32>(durationMs, 1u));
                        }
                        break;

                    case gameplay::SimCueKind::SpawnPrefabEffect:
                        if (cue.assetId.value != 0) {
                            const assets::ScopeId scope =
                                (m_director.ActiveSceneScope() != assets::kInvalidScope)
                                    ? m_director.ActiveSceneScope()
                                    : m_director.PersistentScope();
                            if (auto h = m_assets.PinBlob(scope, cue.assetId, assets::Residency::Sim)) {
                                (void)world::WorldExtractor::InstantiatePrefab(
                                    m_world,
                                    bank,
                                    *h,
                                    world::LocalTransform2D{.x = cue.worldX, .y = cue.worldY});
                            }
                        }
                        break;

                    case gameplay::SimCueKind::CustomGameEvent:
                        break;
                }

                if (m_onConsumeSimCue) {
                    m_onConsumeSimCue(cue, m_world, bank, m_userAppState);
                }
            }
        }
    }

    m_audio.Update(audio::AudioUpdateDesc{
        .tickIndex = m_time.GetRenderView().tickIndex,
        .listenerX = listenerX,
        .listenerY = listenerY,
        .channelScales = m_time.EffectiveScales(),
        .triggers = std::span<const audio::AudioTriggerReq>{audioTriggers.data(), triggerCount},
        .assetBank = &m_assets.Bank(),
        .occlusionQuery = &QuerySpatialOcclusion,
        .occlusionUserCtx = &m_gameplay.Spatial(),
        .frozenChannelMask = m_time.FrozenMask(),
        .focusMuteOverride = m_headless ? i8(0) : (m_window.State().focused ? i8(0) : i8(1))
    });
}

renderer::RenderList& Application::ExtractRenderFrame(
    renderer::ViewportExtent viewportSize) noexcept {
    renderer::RenderList& list = m_renderer.BeginFrame(viewportSize);
    const time::RenderTimeView renderTime = m_time.GetRenderView();

    world::WorldExtractor::Extract(m_world, m_assets.Bank(), renderTime, list);

    if (m_director.ActiveRenderMask() != 0) {
        RenderContext renderCtx{
            .world = m_world,
            .gameplay = m_gameplay,
            .assetBank = m_assets.Bank(),
            .renderList = list,
            .frameInput = m_input.GetFrameView(),
            .windowState = m_window.State(),
            .userState = nullptr,
            .director = &m_director,
            .timeView = renderTime
        };
        m_director.DispatchRenderExtract(renderCtx);
    }

    return list;
}

FrameAuditDigest Application::ComputeFrameAuditDigest() const noexcept {
    FrameAuditDigest d{};
    d.tickIndex = m_time.GetRenderView().tickIndex;
    d.worldHash = m_world.ComputeAuditHash();
    d.gameplayHash = m_gameplay.ComputeAuditHash(m_world);
    d.assetBankHash = m_assets.Bank().ComputeAuditHash();
    d.audioHash = m_audio.ComputeAuditHash();
    d.renderListHash = m_lastRenderListAuditHash;
    d.directorHash = m_director.ComputeAuditHash();
    d.combinedHash = SceneDirector::CombineAuditHashes(
        d.tickIndex,
        d.worldHash,
        d.gameplayHash,
        d.assetBankHash,
        d.audioHash,
        d.renderListHash,
        d.directorHash);
    return d;
}

} // namespace en::framework

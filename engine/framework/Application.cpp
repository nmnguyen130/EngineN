#include "framework/Application.h"

#include "world/WorldExtractor.h"

namespace en::framework {

constexpr SceneLoadContext Application::MakeLoadContext() noexcept {
    return SceneLoadContext{
        .world = m_world,
        .gameplay = m_gameplay,
        .assets = m_assets,
        .audio = m_audio,
        .renderer = m_headless ? nullptr : &m_renderer,
        .userAppState = m_hooks.userAppState,
        .frameIndex = m_frameIndex,
        .sceneScope = m_director.ActiveSceneScope(),
        .persistentScope = m_director.PersistentScope()
    };
}

Result<void, FrameworkError> Application::Init(
    const ApplicationConfig& cfg,
    const AppHooks& hooks) noexcept {
    if (m_initialized) {
        Shutdown();
    }

    m_cfg = cfg;
    m_hooks = hooks;
    m_headless = cfg.headless;
    if (m_headless) {
        m_cfg.audio.headlessMode = true;
    }
    m_frameIndex = 0;
    m_cachedCameraEntity = world::kNullEntity;

    if (!m_headless) {
        if (auto res = m_platform.Init(cfg.appName, cfg.orgName); !res) {
            return Err(FrameworkError{
                FrameworkErrc::PlatformInitFailed, SubSystemStage::Platform, 0});
        }
        if (auto res = m_window.Create(cfg.window); !res) {
            Shutdown();
            return Err(FrameworkError{
                FrameworkErrc::WindowCreateFailed, SubSystemStage::Window, 0});
        }

        if (auto res = m_renderer.Init(m_window, cfg.renderer); !res) {
            Shutdown();
            return Err(FrameworkError{
                FrameworkErrc::RendererInitFailed, SubSystemStage::Renderer,
                static_cast<u8>(res.error())});
        }
    } else {
        m_headlessRenderList.Reset(cfg.window.size);
    }

    if (auto res = m_assets.Init(cfg.assets); !res) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::AssetInitFailed, SubSystemStage::Assets,
            static_cast<u8>(res.error())});
    }

    if (!cfg.bootPakPath.empty() && !m_headless) {
        if (auto pathRes = m_platform.ResolveAsset(cfg.bootPakPath); pathRes) {
            if (auto mountRes = m_assets.MountPackFile(pathRes->View(), 0); !mountRes) {
                Shutdown();
                return Err(FrameworkError{
                    FrameworkErrc::AssetInitFailed, SubSystemStage::Assets,
                    static_cast<u8>(mountRes.error())});
            }
        }
    }

    if (auto res = m_audio.Init(m_cfg.audio); !res) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::AudioInitFailed, SubSystemStage::Audio,
            static_cast<u8>(res.error())});
    }

    if (auto res = m_world.Init(cfg.world); !res) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::WorldInitFailed, SubSystemStage::World,
            static_cast<u8>(res.error())});
    }

    if (auto res = m_gameplay.Init(m_world, hooks.gameplayPolicies, cfg.gameplay); !res) {
        Shutdown();
        return Err(FrameworkError{
            FrameworkErrc::GameplayInitFailed, SubSystemStage::Gameplay,
            static_cast<u8>(res.error())});
    }

    if (hooks.OnRegisterComponents) {
        if (auto res = hooks.OnRegisterComponents(m_world, hooks.userAppState); !res) {
            Shutdown();
            return Err(FrameworkError{
                FrameworkErrc::ClientBootFailed, SubSystemStage::ClientApp, res.error()});
        }
    }

    if (auto res = m_director.Init(m_assets); !res) {
        Shutdown();
        return Err(FrameworkError{
            res.error(), SubSystemStage::SceneDirector, 0});
    }

    m_initialized = true;

    if (hooks.OnBoot) {
        SceneLoadContext loadCtx = MakeLoadContext();
        if (auto res = hooks.OnBoot(loadCtx, m_director); !res) {
            Shutdown();
            return Err(FrameworkError{
                FrameworkErrc::ClientBootFailed, SubSystemStage::ClientApp, res.error()});
        }
    }

    return {};
}

void Application::Shutdown() noexcept {
    if (!m_initialized) {
        return;
    }

    if (m_hooks.OnPreShutdown) {
        SceneLoadContext loadCtx = MakeLoadContext();
        m_hooks.OnPreShutdown(loadCtx);
    }

    m_director.Shutdown(m_assets, m_audio);
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

Result<void, FrameworkError> Application::StepFrame() noexcept {
    ++m_frameIndex;

    // 1. Single-Pass OS & Input Hardware Pump
    const platform::WindowFlags winFlags =
        m_platform.Pump(m_window, m_input, m_hooks.devToolsTap);
    if (m_platform.ShouldQuit()) {
        return {};
    }
    HandleWindowEvents(winFlags);

    // 2. Sequence-Ordered Asset Streaming, GPU Upload, and LRU Frame Advance
    m_assets.Pump(m_renderer, m_cfg.assetPumpBudget);

    // 3. Safe Frame Mutation Point (Executed prior to TimeManager::BeginFrame)
    SceneLoadContext loadCtx = MakeLoadContext();
    auto commitRes = m_director.CommitSafeFramePoint(loadCtx, m_input, m_time);
    if (!commitRes) {
        return Err(FrameworkError{
            commitRes.error(), SubSystemStage::SceneDirector, 0});
    }
    if (*commitRes) {
        // Reset cached camera handle and clock delta baseline after scene activation.
        m_cachedCameraEntity = world::kNullEntity;
        m_time.OnWindowRefocus();
        m_actionBuffer.Clear();
    }

    // 4. Deterministic 120 Hz Fixed-Timestep Simulation Loop
    m_time.BeginFrame();
    u32 ticksExecuted = 0;
    RunSimTicks(ticksExecuted);

    // 5. Simulation Cue Routing & Wait-Free Audio Triple-Buffer Publication
    RouteCuesAndSyncAudio(ticksExecuted);

    // 6. Interpolated Presentation Extraction & Hardware Submission
    if (m_window.State().IsPresentable()) {
        renderer::RenderList& list = m_renderer.BeginFrame();
        const time::RenderTimeView renderTime = m_time.GetRenderView();

        world::WorldExtractor::Extract(m_world, m_assets.Bank(), renderTime, list);

        RenderContext renderCtx{
            .world = m_world,
            .gameplay = m_gameplay,
            .assetBank = m_assets.Bank(),
            .renderList = list,
            .frameInput = m_input.GetFrameView(),
            .windowState = m_window.State(),
            .userState = nullptr,
            .timeView = renderTime
        };
        m_director.DispatchRenderExtract(renderCtx);

        m_renderer.Submit(list);
    }

    return {};
}

Result<FrameAuditDigest, FrameworkError> Application::StepHeadless(
    u32 tickCount,
    u32 audioBlocksPerTick) noexcept {
    for (u32 t = 0; t < tickCount; ++t) {
        ++m_frameIndex;
        m_assets.PumpHeadless(m_cfg.assetPumpBudget);

        SceneLoadContext loadCtx = MakeLoadContext();
        auto commitRes = m_director.CommitSafeFramePoint(loadCtx, m_input, m_time);
        if (!commitRes) {
            return Err(FrameworkError{
                commitRes.error(), SubSystemStage::SceneDirector, 0});
        }
        if (*commitRes) {
            m_cachedCameraEntity = world::kNullEntity;
            m_actionBuffer.Clear();
        }

        m_time.BeginFrameManual(time::kFixedDtNs);
        u32 ticksExecuted = 0;
        RunSimTicks(ticksExecuted);

        RouteCuesAndSyncAudio(ticksExecuted);

        if (audioBlocksPerTick > 0) {
            m_audio.PumpHeadlessBlocks(audioBlocksPerTick);
        }
    }

    m_headlessRenderList.Reset(m_cfg.window.size);
    const time::RenderTimeView renderTime = m_time.GetRenderView();
    world::WorldExtractor::Extract(m_world, m_assets.Bank(), renderTime, m_headlessRenderList);

    RenderContext renderCtx{
        .world = m_world,
        .gameplay = m_gameplay,
        .assetBank = m_assets.Bank(),
        .renderList = m_headlessRenderList,
        .frameInput = m_input.GetFrameView(),
        .windowState = m_window.State(),
        .userState = nullptr,
        .timeView = renderTime
    };
    m_director.DispatchRenderExtract(renderCtx);

    m_headlessRenderList.Compile();
    m_lastRenderListAuditHash = m_headlessRenderList.ComputeAuditHash();

    return ComputeFrameAuditDigest();
}

void Application::HandleWindowEvents(platform::WindowFlags flags) noexcept {
    if (flags == platform::WindowFlags::None) {
        return;
    }

    m_audio.OnWindowEvent(flags, m_window.State());

    if (platform::HasFlag(flags, platform::WindowFlags::Resized)) {
        m_renderer.ResizeViewport(m_window.State().pixelSize);
    }
    if (platform::HasFlag(flags, platform::WindowFlags::FocusGained)) {
        m_time.OnWindowRefocus();
    }
    if (platform::HasFlag(flags, platform::WindowFlags::FocusLost)) {
        m_actionBuffer.Clear();
    }
}

void Application::RunSimTicks(u32& outTicksExecuted) noexcept {
    // Hoist SimContext outside the tick loop so pointer references are initialized once per frame.
    SimContext simCtx{
        .world = m_world,
        .gameplay = m_gameplay,
        .assetBank = m_assets.Bank(),
        .audio = m_audio,
        .platform = m_platform,
        .actionBuffer = m_actionBuffer,
        .tickInput = nullptr,
        .userState = nullptr,
        .step = {}
    };

    u32 count = 0;
    while (const auto step = m_time.PollTick()) {
        ++count;
        const input::TickInput tickInput = m_input.PollTick(step.TickIndex());
        m_actionBuffer.Feed(tickInput, step.IsFrozen(time::TimeChannel::Player));

        m_world.BeginTick(step);
        m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);

        simCtx.step = step;
        simCtx.tickInput = &tickInput;
        m_director.DispatchSimTick(simCtx);

        m_world.EndTick(step);

        u8 frozenMask = 0;
        for (u8 ch = 0; ch < time::kMaxTimeChannels; ++ch) {
            if (step.IsFrozen(ch)) {
                frozenMask |= static_cast<u8>(1u << ch);
            }
            m_latchedChannelScales[ch] = step.EffectiveScale(ch);
        }
        m_latchedFrozenMask = frozenMask;
    }

    outTicksExecuted = count;
}

void Application::RouteCuesAndSyncAudio(u32 ticksExecuted) noexcept {
    if (ticksExecuted > 0) {
        const auto cues = m_gameplay.DrainCues();
        if (!cues.empty()) {
            m_audio.ConsumeSimCues(cues, m_world, m_assets.Bank());
            if (m_hooks.OnConsumeSimCue) {
                for (const auto& cue : cues) {
                    m_hooks.OnConsumeSimCue(cue, m_world, m_assets.Bank(), m_hooks.userAppState);
                }
            }
        }
    } else if (m_time.IsPaused()) {
        // When simulation clock is paused by a state overlay, freeze gameplay audio channels cleanly.
        m_latchedFrozenMask = 0x3Fu; // Freeze World, Player, Enemy, Projectile, Vfx, Camera; keep Ui (bit 6) active.
    }

    SyncAudioListenerO1();
    m_audio.Update(
        m_world,
        m_gameplay.Spatial(),
        m_assets.Bank(),
        m_time.GetRenderView(),
        m_latchedFrozenMask,
        m_latchedChannelScales);
}

void Application::SyncAudioListenerO1() noexcept {
    // O(1) fast path using cached camera entity handle.
    if (m_world.IsAlive(m_cachedCameraEntity)) {
        if (const auto* rig = m_world.TryGet<world::CameraRig2D>(m_cachedCameraEntity)) {
            if (rig->active && rig->viewSlot == 0 && !rig->screenSpace) {
                const auto& wt = m_world.GetWorldTransform(m_cachedCameraEntity);
                m_audio.SetListenerPosition(wt.x, wt.y);
                return;
            }
        }
    }

    // Fallback scan only when camera entity spawns or changes.
    for (auto [entity, camRig] : m_world.View<world::CameraRig2D>()) {
        if (camRig.active && camRig.viewSlot == 0 && !camRig.screenSpace) {
            m_cachedCameraEntity = entity;
            const auto& wt = m_world.GetWorldTransform(entity);
            m_audio.SetListenerPosition(wt.x, wt.y);
            return;
        }
    }
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
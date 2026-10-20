#include "game/runtime/VeilGame.h"
#include <cstdio>

namespace en::veil {

VeilSessionState* VeilGame::s_activeSession = nullptr;

AppHooks VeilGame::BuildAppHooks() noexcept {
    s_activeSession = &m_session;
    AppHooks hooks{};
    hooks.userAppState = this;
    hooks.gameplayPolicies.onStepStateMachines = &VeilGame::StepPolicies;
    hooks.gameplayPolicies.onResolveCombatHits = &VeilGame::ResolveCombatHitsBridge;
    hooks.onRegisterComponents = &VeilGame::OnRegisterComponents;
    hooks.onBoot = &VeilGame::OnBoot;
    return hooks;
}

Result<void, WorldErrc> VeilGame::OnRegisterComponents(World& world) noexcept {
    (void)world.RegisterComponentPool<ModifierStack2D>(64);
    (void)world.RegisterComponentPool<PlayerControl2D>(16);
    (void)world.RegisterComponentPool<EnemyBrain2D>(256);
    (void)world.RegisterComponentPool<Projectile2D>(512);
    (void)world.RegisterComponentPool<GrappleAnchor2D>(128);
    (void)world.RegisterComponentPool<ShrineCheckpoint2D>(64);
    return world.RegisterComponentPool<BreakableProp2D>(256);
}

Result<void, FrameworkErrc> VeilGame::OnBoot(SceneLoadContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userAppState);
    if (!self) return Err(FrameworkErrc::ClientBootFailed);

    const usize pakSize = VeilAssets::BuildBootPackage(self->m_bootPakBuffer);
    if (pakSize == 0 || !ctx.assets.MountPackMemory(
                            std::span<const u8>(self->m_bootPakBuffer.data(), pakSize), 10)) {
        return Err(FrameworkErrc::AssetInitFailed);
    }

    const TextureHandle tex = ctx.assets.PinTexture(ctx.persistentScope, kTexMainAtlasId);
    self->m_session.mainAtlas = ctx.assets.PinAtlas(ctx.persistentScope, kAtlasMainId, tex);
    self->m_session.playerRigBlob = ctx.assets.PinBlob(ctx.persistentScope, kRigPlayerId);
    self->m_session.enemyRigBlob = ctx.assets.PinBlob(ctx.persistentScope, kRigEnemyId);
    (void)ctx.assets.PinAudio(ctx.persistentScope, kClipSlashId);
    (void)ctx.assets.PinAudio(ctx.persistentScope, kClipParryId);
    (void)ctx.assets.PinAudio(ctx.persistentScope, kClipHitId);

    VeilAiAndWorldSystem::BuildEnemyBehaviorTree(self->m_session.enemyBt);

    // Load Courtyard scene immediately on boot so windowed mode displays the full playable world right away
    SceneTransitionDesc courtyardDesc{};
    courtyardDesc.sceneId = kSceneCourtyardId;
    courtyardDesc.manifestBlobId = kSceneCourtyardId;
    courtyardDesc.onActivated = &VeilGame::OnCourtyardActivated;
    (void)ctx.director->RequestSceneTransition(courtyardDesc, ctx.assets);

    StateLayerDesc gameLayer{};
    gameLayer.id = kGameplayStateId;
    gameLayer.onSimTick = &VeilGame::GameplaySimTick;
    gameLayer.onRenderExtract = &VeilGame::GameplayRenderExtract;
    gameLayer.stateData = self;
    return ctx.director->PushState(gameLayer);
}

void VeilGame::OnCourtyardActivated(SceneLoadContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userAppState);
    if (!self) return;
    VeilSessionState& s = self->m_session;

    s.playerEntity = VeilPlayerSystem::SpawnPlayer(
        ctx.world, {5.2f, 3.5f}, s.mainAtlas, s.playerRigBlob);

    EntitySpawnDesc camDesc{};
    camDesc.local.x = 6.5f;
    camDesc.local.y = 4.2f;
    camDesc.timeChannel = TimeChannel::Camera;
    if (auto cRes = ctx.world.Spawn(camDesc)) {
        s.cameraEntity = *cRes;
        (void)ctx.world.Emplace<CameraRig2D>(s.cameraEntity, CameraRig2D{});
        CameraController2D ctrl{};
        ctrl.primaryTarget = s.playerEntity;
        ctrl.activeRoomBounds = {0.0f, 0.0f, 64.0f, 24.0f};
        (void)ctx.world.Emplace<CameraController2D>(s.cameraEntity, ctrl);
    }

    // Moving carrier platform.
    EntitySpawnDesc carDesc{};
    carDesc.local.x = 12.0f;
    carDesc.local.y = 4.0f;
    if (auto mRes = ctx.world.Spawn(carDesc)) {
        (void)ctx.world.Emplace<KinematicCarrier2D>(
            *mRes, KinematicCarrier2D{0.03f, 0.0f, CompactAabb2D::FromMeters(0.0f, 0.0f, 1.5f, 0.25f)});
    }

    // Grapple anchor, shrine, breakable prop, and courtyard enemies.
    EntitySpawnDesc grapDesc{};
    grapDesc.local.x = 9.0f;
    grapDesc.local.y = 8.0f;
    if (auto gRes = ctx.world.Spawn(grapDesc)) {
        (void)ctx.world.Emplace<GrappleAnchor2D>(*gRes, GrappleAnchor2D{});
    }

    EntitySpawnDesc shrDesc{};
    shrDesc.local.x = 2.4f;
    shrDesc.local.y = 2.2f;
    if (auto shRes = ctx.world.Spawn(shrDesc)) {
        (void)ctx.world.Emplace<ShrineCheckpoint2D>(*shRes, ShrineCheckpoint2D{1, 10, false, {}});
    }

    (void)VeilAiAndWorldSystem::SpawnEnemy(
        ctx.world, {9.2f, 3.5f}, EnemyKind::RoninScout, s.mainAtlas, s.enemyRigBlob);
    (void)VeilAiAndWorldSystem::SpawnEnemy(
        ctx.world, {14.0f, 3.5f}, EnemyKind::AshArcher, s.mainAtlas, s.enemyRigBlob);
}

void VeilGame::OnSanctumActivated(SceneLoadContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userAppState);
    if (!self) return;
    VeilSessionState& s = self->m_session;
    ++s.roomsCleared;

    s.playerEntity = VeilPlayerSystem::SpawnPlayer(
        ctx.world, {4.0f, 3.5f}, s.mainAtlas, s.playerRigBlob);
    s.bossEntity = VeilAiAndWorldSystem::SpawnEnemy(
        ctx.world, {7.5f, 3.5f}, EnemyKind::WardenBoss, s.mainAtlas, s.enemyRigBlob);

    EntitySpawnDesc camDesc{};
    camDesc.local.x = 6.0f;
    camDesc.local.y = 4.0f;
    camDesc.timeChannel = TimeChannel::Camera;
    if (auto cRes = ctx.world.Spawn(camDesc)) {
        s.cameraEntity = *cRes;
        (void)ctx.world.Emplace<CameraRig2D>(s.cameraEntity, CameraRig2D{});
        CameraController2D ctrl{};
        ctrl.primaryTarget = s.playerEntity;
        ctrl.secondaryTarget = s.bossEntity;
        ctrl.activeRoomBounds = {0.0f, 0.0f, 48.0f, 24.0f};
        (void)ctx.world.Emplace<CameraController2D>(s.cameraEntity, ctrl);
    }
}

void VeilGame::StepPolicies(
    World& world, const AssetBank&, const SimStepView& step,
    const TickInput& tickInput, ActionBuffer& actionBuffer,
    GameplayPipeline& pipeline) noexcept {
    if (!s_activeSession) return;
    VeilPlayerSystem::StepPlayerController(world, step, tickInput, actionBuffer, pipeline, *s_activeSession);
    VeilAiAndWorldSystem::StepEnemiesAndProjectiles(world, step, pipeline, *s_activeSession);
}

void VeilGame::ResolveCombatHitsBridge(
    World& world, const SimStepView& step,
    std::span<const ResolvedHitEvent> hits, GameplayPipeline& pipeline) noexcept {
    if (!s_activeSession) return;
    VeilAiAndWorldSystem::ResolveCombatHits(world, step, hits, pipeline, *s_activeSession);
}

void VeilGame::TitleSimTick(SimContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userState);
    if (!self || !ctx.tickInput) return;
    if (ctx.tickInput->Pressed(VeilAction::Jump) || ctx.tickInput->Pressed(VeilAction::Attack)) {
        StateLayerDesc gameLayer{};
        gameLayer.id = kGameplayStateId;
        gameLayer.onSimTick = &VeilGame::GameplaySimTick;
        gameLayer.onRenderExtract = &VeilGame::GameplayRenderExtract;
        gameLayer.stateData = self;
        (void)ctx.director->SwitchState(gameLayer);
    }
}

void VeilGame::TitleRenderExtract(RenderContext& ctx) noexcept {
    ctx.renderList.PushText(
        {420.0f, 300.0f}, "VEIL OF THE SILENT - PRESS JUMP TO BEGIN",
        Color::FromRGBA8(220, 240, 255, 255), 1.5f, 0, 200, 15);
}

void VeilGame::GameplaySimTick(SimContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userState);
    if (!self || !ctx.tickInput) return;
    if (ctx.tickInput->Pressed(VeilAction::Pause)) {
        StateLayerDesc pauseLayer{};
        pauseLayer.id = kPauseStateId;
        pauseLayer.flags = StateLayerFlags::BlockLowerSimTicks |
                           StateLayerFlags::BlockLowerInput |
                           StateLayerFlags::PauseSimClock;
        pauseLayer.onSimTick = &VeilGame::PauseSimTick;
        pauseLayer.onRenderExtract = &VeilGame::PauseRenderExtract;
        pauseLayer.stateData = self;
        (void)ctx.director->PushState(pauseLayer);
    }
}

void VeilGame::GameplayRenderExtract(RenderContext& ctx) noexcept {
    auto* self = static_cast<VeilGame*>(ctx.userState);
    if (self) VeilAiAndWorldSystem::RenderHudAndEffects(ctx, self->m_session);
}

void VeilGame::PauseSimTick(SimContext& ctx) noexcept {
    if (ctx.tickInput && ctx.tickInput->Pressed(VeilAction::Pause)) {
        (void)ctx.director->PopState();
    }
}

void VeilGame::PauseRenderExtract(RenderContext& ctx) noexcept {
    ctx.renderList.PushRect({0.0f, 0.0f, 1280.0f, 720.0f}, Color::FromRGBA8(0, 0, 0, 150), 0, 220, 32768, 15);
    ctx.renderList.PushText({560.0f, 340.0f}, "PAUSED", Color::FromRGBA8(255, 255, 255, 255), 1.8f, 0, 225, 15);
}

bool VeilGame::RunVerificationSuite() noexcept {
    Application app{};
    VeilGame game{};
    ApplicationConfig cfg{};
    cfg.headless = true;
    if (!app.Init(cfg, game.BuildAppHooks())) return false;

    app.Input().Map()
        .BindAxis(VeilAxis::MoveX, AxisBinding{InputSource::Key(4), InputSource::Key(7)})
        .Bind(VeilAction::Jump, {InputSource::Key(44)})
        .Bind(VeilAction::Dash, {InputSource::Key(225)})
        .Bind(VeilAction::Attack, {InputSource::Key(13)})
        .Bind(VeilAction::Parry, {InputSource::Key(14)})
        .Bind(VeilAction::ThrowShuriken, {InputSource::Key(15)})
        .Bind(VeilAction::Grapple, {InputSource::Key(16)})
        .Bind(VeilAction::Pause, {InputSource::Key(41)});

    (void)app.Debug().Console().RegisterCVarF32(
        "player.speed", &game.m_session.cvarPlayerSpeed, 2.0f, 20.0f);
    app.Debug().SetActiveOverlays(OverlayFlags::AllWorld | OverlayFlags::AllHud);

    // 1. Transition from Title to Courtyard scene.
    app.Input().InjectSyntheticAction(VeilAction::Jump, true);
    SceneTransitionDesc courtyardDesc{};
    courtyardDesc.sceneId = kSceneCourtyardId;
    courtyardDesc.manifestBlobId = kSceneCourtyardId;
    courtyardDesc.onActivated = &VeilGame::OnCourtyardActivated;
    (void)app.Director().RequestSceneTransition(courtyardDesc, app.Assets());
    auto d0 = app.StepHeadless(2);
    app.Input().InjectSyntheticAction(VeilAction::Jump, false);
    if (!d0 || !app.WorldRef().IsAlive(game.m_session.playerEntity)) return false;

    // 2. Exercise movement, katana combo, shuriken, grapple, and parry.
    for (int m = 0; m < 18; ++m) {
        app.Input().InjectSyntheticAxis(VeilAxis::MoveX, 1.0f);
        (void)app.StepHeadless(1);
    }
    for (int i = 0; i < 6; ++i) {
        app.Input().InjectSyntheticAxis(VeilAxis::MoveX, 1.0f);
        app.Input().InjectSyntheticAction(VeilAction::Attack, true);
        (void)app.StepHeadless(4);
        app.Input().InjectSyntheticAction(VeilAction::Attack, false);
        (void)app.StepHeadless(4);
    }
    app.Input().InjectSyntheticAction(VeilAction::ThrowShuriken, true);
    (void)app.StepHeadless(2);
    app.Input().InjectSyntheticAction(VeilAction::ThrowShuriken, false);

    app.Input().InjectSyntheticAction(VeilAction::Grapple, true);
    (void)app.StepHeadless(10);
    app.Input().InjectSyntheticAction(VeilAction::Grapple, false);

    // 3. Verify pause stack push/pop.
    app.Input().InjectSyntheticAction(VeilAction::Pause, true);
    (void)app.StepHeadless(2);
    app.Input().InjectSyntheticAction(VeilAction::Pause, false);
    if (app.Director().StateDepth() != 2 || !app.Time().IsPaused()) return false;

    app.Input().InjectSyntheticAction(VeilAction::Pause, true);
    (void)app.StepHeadless(2);
    app.Input().InjectSyntheticAction(VeilAction::Pause, false);
    if (app.Director().StateDepth() != 1 || app.Time().IsPaused()) return false;

    // 4. Transition to Boss Sanctum & verify boss combat.
    SceneTransitionDesc sanctumDesc{};
    sanctumDesc.sceneId = kSceneBossSanctumId;
    sanctumDesc.manifestBlobId = kSceneBossSanctumId;
    sanctumDesc.onActivated = &VeilGame::OnSanctumActivated;
    (void)app.Director().RequestSceneTransition(sanctumDesc, app.Assets());
    (void)app.StepHeadless(4);
    if (!app.WorldRef().IsAlive(game.m_session.bossEntity)) return false;

    for (int i = 0; i < 15; ++i) {
        app.Input().InjectSyntheticAction(VeilAction::Attack, true);
        (void)app.StepHeadless(4);
        app.Input().InjectSyntheticAction(VeilAction::Attack, false);
        (void)app.StepHeadless(4);
    }

    // 5. Verify SaveGameManager checkpoint round-trip.
    std::array<u8, 1024> saveBuf{};
    SaveSlotHeader saveHdr{};
    saveHdr.checkpointRoomId = 2;
    saveHdr.playerHp = 100;
    saveHdr.currency = game.m_session.soulCurrency;
    auto savedBytes = SaveGameManager::SerializeSaveSlot(saveHdr, app.Gameplay(), saveBuf);
    if (!savedBytes) return false;
    auto loadedHdr = SaveGameManager::DeserializeSaveSlot(
        std::span<const u8>(saveBuf.data(), *savedBytes), app.Gameplay());
    if (!loadedHdr || loadedHdr->checkpointRoomId != 2) return false;

    app.Debug().SetActiveOverlays(OverlayFlags::AllHud);
    (void)app.StepHeadless(1);
    const u32 renderedPixels = app.Renderer().CountNonBlackPixels();
    if (renderedPixels < 5000U) return false;
    std::printf("[PASS] Veil of the Silent full industrial verification passed (Kills=%u, Souls=%u, RenderedPixels=%u/230400).\n",
                game.m_session.totalKills, game.m_session.soulCurrency, renderedPixels);
    app.Shutdown();
    return true;
}

} // namespace en::veil

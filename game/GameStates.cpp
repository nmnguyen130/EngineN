#include "game/GameStates.h"
#include "game/GamePolicies.h"

namespace game {

namespace {

// ============================================================================
// Immediate-Mode Screen-Space HUD & Overlay Helper (Zero Heap, Single Draw Call)
// ============================================================================
void RenderPlayerHud(
    const GameSessionState& session,
    framework::RenderContext& ctx) noexcept
{
    if (!ctx.world.IsAlive(session.playerEntity)) return;
    const auto* dmg  = ctx.world.TryGet<gameplay::DamageReceiver2D>(session.playerEntity);
    const auto* ctrl = ctx.world.TryGet<PlayerController2D>(session.playerEntity);
    if (!dmg || !ctrl) return;

    // 1. Health Masks (Top-Left)
    for (i16 i = 0; i < dmg->maxHp; ++i) {
        const bool filled = (i < dmg->hp);
        const f32 x = 28.0f + static_cast<f32>(i) * 26.0f;
        const f32 y = 28.0f;
        if (session.debugSys != nullptr) {
            session.debugSys->Shapes().AddBox(
                x, y, 9.0f, 9.0f,
                filled ? 0xE63946FFu : 0x3A3A4AFFu,
                filled, 2.0f, 1);
        }
    }

    // 2. Soul Energy Gauge (Below Health Masks)
    if (session.debugSys != nullptr) {
        const f32 soulRatio = static_cast<f32>(ctrl->soulEnergy) / 100.0f;
        session.debugSys->Shapes().AddBox(80.0f, 52.0f, 62.0f, 5.0f, 0x222831FFu, true, 1.0f, 1);
        if (soulRatio > 0.01f) {
            const f32 fillHalfW = 60.0f * soulRatio;
            session.debugSys->Shapes().AddBox(
                20.0f + fillHalfW, 52.0f, fillHalfW, 3.5f, 0x66FCF1FFu, true, 1.0f, 1);
        }

        // 3. Banner Notification
        if (session.bannerTimerTicks > 0) {
            const f32 cx = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.width) - 80.0f;
            session.debugSys->Shapes().AddTextScreen(
                cx, 96.0f,
                std::string_view{session.bannerText.data(), 12},
                0xFFD166FFu, 1);
        }
    }
}

// ============================================================================
// 1. TITLE SCREEN STATE (StateIds::kTitleScreen)
// ============================================================================
void TitleOnEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* session = static_cast<GameSessionState*>(stateData);
    if (session == nullptr) return;
    session->titleCursor = 0;
    ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::Bypass, 20000.0f);
}

void TitleOnSimTick(framework::SimContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr || ctx.tickInput == nullptr) return;

    if (ctx.tickInput->Pressed(GameAction::LookUp) || ctx.tickInput->Pressed(GameAction::Crouch)) {
        session->titleCursor = static_cast<u8>(session->titleCursor ^ 1u);
        (void)ctx.audio.PlayCue(Assets::kCueUiMove, ctx.assetBank);
    }

    if (ctx.tickInput->Pressed(GameAction::MenuConfirm)) {
        (void)ctx.audio.PlayCue(Assets::kCueUiConfirm, ctx.assetBank);
        if (session->titleCursor == 0) {
            // Start Game -> Switch to GameplayState
            if (session->director != nullptr) {
                (void)session->director->SwitchState(MakeGameplayStateDesc(session));
            }
        } else {
            session->quitRequested = true;
        }
    }
}

void TitleOnRenderExtract(framework::RenderContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr || session->debugSys == nullptr) return;

    const f32 cx = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.width);
    const f32 cy = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.height);

    session->debugSys->Shapes().AddBox(cx, cy, 180.0f, 110.0f, 0x0F172AE0u, true, 2.0f, 1);
    session->debugSys->Shapes().AddTextScreen(cx - 48.0f, cy - 60.0f, "AETHELGARD", 0x66FCF1FFu, 1);
    session->debugSys->Shapes().AddTextScreen(
        cx - 44.0f, cy - 10.0f,
        session->titleCursor == 0 ? "> ENTER ABYSS" : "  ENTER ABYSS",
        session->titleCursor == 0 ? 0xFFD166FFu : 0x94A3B8FFu, 1);
    session->debugSys->Shapes().AddTextScreen(
        cx - 44.0f, cy + 20.0f,
        session->titleCursor == 1 ? "> QUIT GAME  " : "  QUIT GAME  ",
        session->titleCursor == 1 ? 0xFFD166FFu : 0x94A3B8FFu, 1);
}

// ============================================================================
// 2. ACTIVE GAMEPLAY STATE (StateIds::kGameplay)
// ============================================================================
void GameplayOnEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* session = static_cast<GameSessionState*>(stateData);
    if (session == nullptr) return;

    // Ensure full-bandwidth audio and active music stems
    ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::Bypass, 20000.0f);
    ctx.audio.SetBusFilter(audio::AudioBusId::Ambience, audio::FilterMode::Bypass, 20000.0f);
    constexpr std::array<f32, 4> kActiveStems{1.0f, 1.0f, 0.85f, 0.0f};
    ctx.audio.SetMusicStemWeights(kActiveStems);
}

void GameplayOnSimTick(framework::SimContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr || ctx.tickInput == nullptr) return;

    if (session->bannerTimerTicks > 0) {
        --session->bannerTimerTicks;
    }

    // Check Modal Pause Input
    if (ctx.tickInput->Pressed(GameAction::Pause)) {
        (void)ctx.audio.PlayCue(Assets::kCueUiConfirm, ctx.assetBank);
        if (session->director != nullptr) {
            (void)session->director->PushState(MakePauseModalStateDesc(session));
        }
        return;
    }

    // Step Player Controller, Enemies, Moving Platforms, Room Triggers, and VFX
    StepPlayerController(
        *session, ctx.world, ctx.gameplay, ctx.audio, ctx.assetBank,
        ctx.step, *ctx.tickInput, ctx.actionBuffer);

    StepEnemiesAndCarriers(
        *session, ctx.world, ctx.gameplay, ctx.audio, ctx.assetBank, ctx.step);

    // Check Player Death Transition to GameOver Overlay
    if (ctx.world.IsAlive(session->playerEntity)) {
        if (const auto* dmg = ctx.world.TryGet<gameplay::DamageReceiver2D>(session->playerEntity);
            dmg != nullptr && dmg->hp <= 0)
        {
            if (const auto* fsm = ctx.world.TryGet<gameplay::StateMachine2D>(session->playerEntity);
                fsm != nullptr && fsm->stateTicks >= 45 && session->director != nullptr)
            {
                (void)session->director->PushState(MakeGameOverStateDesc(session));
            }
        }
    }
}

void GameplayOnRenderExtract(framework::RenderContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr) return;
    RenderPlayerHud(*session, ctx);
}

// ============================================================================
// 3. MODAL PAUSE OVERLAY STATE (StateIds::kPauseModal)
//    Pauses SimClock + blocks lower SimTicks & Input while rendering World below
// ============================================================================
void PauseOnEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* session = static_cast<GameSessionState*>(stateData);
    if (session != nullptr) session->pauseCursor = 0;

    // Underwater low-pass filter on Music & Ambience while paused
    ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::LowPass, 650.0f);
    ctx.audio.SetBusFilter(audio::AudioBusId::Ambience, audio::FilterMode::LowPass, 500.0f);
}

void PauseOnExit(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    (void)stateData;
    ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::Bypass, 20000.0f);
    ctx.audio.SetBusFilter(audio::AudioBusId::Ambience, audio::FilterMode::Bypass, 20000.0f);
}

void PauseOnRenderExtract(framework::RenderContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr) return;

    // Note: Because StateLayerFlags::PauseSimClock pauses the 120Hz tick loop,
    // modal pause menu navigation evaluates deterministic edges from ctx.frameInput!
    if (ctx.frameInput.Pressed(GameAction::Pause) || ctx.frameInput.Pressed(GameAction::MenuCancel)) {
        if (session->director != nullptr) {
            (void)session->director->PopState();
        }
        return;
    }

    if (ctx.frameInput.Pressed(GameAction::LookUp)) {
        session->pauseCursor = static_cast<u8>((session->pauseCursor + 2u) % 3u);
    } else if (ctx.frameInput.Pressed(GameAction::Crouch)) {
        session->pauseCursor = static_cast<u8>((session->pauseCursor + 1u) % 3u);
    }

    if (ctx.frameInput.Pressed(GameAction::MenuConfirm)) {
        if (session->pauseCursor == 0) {
            // Resume
            if (session->director != nullptr) (void)session->director->PopState();
        } else if (session->pauseCursor == 1) {
            // Respawn at Last Shrine
            if (auto* dmg = ctx.world.TryGet<gameplay::DamageReceiver2D>(session->playerEntity)) {
                dmg->hp = dmg->maxHp;
            }
            world::LocalTransform2D loc = ctx.world.GetLocalTransform(session->playerEntity);
            loc.x = session->checkpointX;
            loc.y = session->checkpointY;
            ctx.world.Teleport(session->playerEntity, loc);
            if (session->director != nullptr) (void)session->director->PopState();
        } else {
            // Return to Title Screen
            if (session->director != nullptr) {
                (void)session->director->ClearAndPushState(MakeTitleStateDesc(session));
            }
        }
    }

    if (session->debugSys != nullptr) {
        const f32 cx = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.width);
        const f32 cy = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.height);
        session->debugSys->Shapes().AddBox(cx, cy, 160.0f, 100.0f, 0x0B0C10E8u, true, 2.0f, 1);
        session->debugSys->Shapes().AddTextScreen(cx - 32.0f, cy - 55.0f, "- PAUSED -", 0x66FCF1FFu, 1);
        session->debugSys->Shapes().AddTextScreen(
            cx - 48.0f, cy - 15.0f,
            session->pauseCursor == 0 ? "> RESUME" : "  RESUME",
            session->pauseCursor == 0 ? 0xFFD166FFu : 0xC5C6C7FFu, 1);
        session->debugSys->Shapes().AddTextScreen(
            cx - 48.0f, cy + 10.0f,
            session->pauseCursor == 1 ? "> SHRINE" : "  SHRINE",
            session->pauseCursor == 1 ? 0xFFD166FFu : 0xC5C6C7FFu, 1);
        session->debugSys->Shapes().AddTextScreen(
            cx - 48.0f, cy + 35.0f,
            session->pauseCursor == 2 ? "> TITLE " : "  TITLE ",
            session->pauseCursor == 2 ? 0xFFD166FFu : 0xC5C6C7FFu, 1);
    }
}

// ============================================================================
// 4. GAME OVER STATE (StateIds::kGameOver)
// ============================================================================
void GameOverOnEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* session = static_cast<GameSessionState*>(stateData);
    if (session != nullptr) session->gameOverCursor = 0;
    ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::LowPass, 400.0f);
}

void GameOverOnSimTick(framework::SimContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr || ctx.tickInput == nullptr) return;

    if (ctx.tickInput->Pressed(GameAction::MenuConfirm)) {
        // Restore Player HP & Teleport to Last Shrine Checkpoint
        if (auto* dmg = ctx.world.TryGet<gameplay::DamageReceiver2D>(session->playerEntity)) {
            dmg->hp          = dmg->maxHp;
            dmg->iframeTicks = 60;
        }
        if (auto* fsm = ctx.world.TryGet<gameplay::StateMachine2D>(session->playerEntity)) {
            fsm->stateId    = static_cast<u8>(PlayerState::Idle);
            fsm->stateTicks = 0;
        }
        world::LocalTransform2D loc = ctx.world.GetLocalTransform(session->playerEntity);
        loc.x = session->checkpointX;
        loc.y = session->checkpointY;
        ctx.world.Teleport(session->playerEntity, loc);

        ctx.audio.SetBusFilter(audio::AudioBusId::Music, audio::FilterMode::Bypass, 20000.0f);
        if (session->director != nullptr) {
            (void)session->director->PopState();
        }
    }
}

void GameOverOnRenderExtract(framework::RenderContext& ctx) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userState);
    if (session == nullptr || session->debugSys == nullptr) return;

    const f32 cx = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.width);
    const f32 cy = 0.5f * static_cast<f32>(ctx.windowState.pixelSize.height);
    session->debugSys->Shapes().AddBox(cx, cy, 190.0f, 90.0f, 0x1A0508F0u, true, 2.0f, 1);
    session->debugSys->Shapes().AddTextScreen(cx - 48.0f, cy - 30.0f, "SOUL SHATTERED", 0xE63946FFu, 1);
    session->debugSys->Shapes().AddTextScreen(cx - 48.0f, cy + 15.0f, "> AWAKEN AT SHRINE", 0xFFD166FFu, 1);
}

} // namespace

// ============================================================================
// StateLayerDesc Builders
// ============================================================================
framework::StateLayerDesc MakeTitleStateDesc(GameSessionState* session) noexcept {
    return framework::StateLayerDesc{
        .onSimTick       = &TitleOnSimTick,
        .onRenderExtract = &TitleOnRenderExtract,
        .onEnter         = &TitleOnEnter,
        .onExit          = nullptr,
        .stateData       = session,
        .id              = StateIds::kTitleScreen,
        .inputActionMask = kMenuActionMask,
        .flags           = framework::StateLayerFlags::None
    };
}

framework::StateLayerDesc MakeGameplayStateDesc(GameSessionState* session) noexcept {
    return framework::StateLayerDesc{
        .onSimTick       = &GameplayOnSimTick,
        .onRenderExtract = &GameplayOnRenderExtract,
        .onEnter         = &GameplayOnEnter,
        .onExit          = nullptr,
        .stateData       = session,
        .id              = StateIds::kGameplay,
        .inputActionMask = kGameplayActionMask,
        .flags           = framework::StateLayerFlags::None
    };
}

framework::StateLayerDesc MakePauseModalStateDesc(GameSessionState* session) noexcept {
    return framework::StateLayerDesc{
        .onSimTick       = nullptr,
        .onRenderExtract = &PauseOnRenderExtract,
        .onEnter         = &PauseOnEnter,
        .onExit          = &PauseOnExit,
        .stateData       = session,
        .id              = StateIds::kPauseModal,
        .inputActionMask = kMenuActionMask,
        .flags           = static_cast<framework::StateLayerFlags>(
            static_cast<u8>(framework::StateLayerFlags::BlockLowerSimTicks) |
            static_cast<u8>(framework::StateLayerFlags::BlockLowerInput) |
            static_cast<u8>(framework::StateLayerFlags::PauseSimClock))
    };
}

framework::StateLayerDesc MakeGameOverStateDesc(GameSessionState* session) noexcept {
    return framework::StateLayerDesc{
        .onSimTick       = &GameOverOnSimTick,
        .onRenderExtract = &GameOverOnRenderExtract,
        .onEnter         = &GameOverOnEnter,
        .onExit          = nullptr,
        .stateData       = session,
        .id              = StateIds::kGameOver,
        .inputActionMask = kMenuActionMask,
        .flags           = static_cast<framework::StateLayerFlags>(
            static_cast<u8>(framework::StateLayerFlags::BlockLowerSimTicks) |
            static_cast<u8>(framework::StateLayerFlags::BlockLowerInput))
    };
}

// ============================================================================
// RoomSwap Transition Callbacks (Executed at CommitSafeFramePoint Barrier)
// ============================================================================
void OnRoomDeactivating(framework::SceneLoadContext& ctx, assets::AssetId /*sceneId*/) noexcept {
    // Destroy all room-transient entities (Enemies, Portals, Moving Platforms, VFX)
    // while keeping persistent entities (Player + CameraRig) alive across rooms!
    for (auto [ent, tag] : ctx.world.View<RoomTransientTag>()) {
        (void)tag;
        (void)ctx.world.DeferDestroy(ent);
    }
    (void)ctx.world.FlushCommands();
}

bool OnRoomActivated(framework::SceneLoadContext& ctx, assets::AssetId /*sceneId*/) noexcept {
    auto* session = static_cast<GameSessionState*>(ctx.userAppState);
    if (session == nullptr) return false;

    // 1. Ensure Player & Camera exist and spawn room-local entities
    PopulateRoomEntities(*session, ctx.world, ctx.gameplay, ctx.assets.Bank(), session->activeRoomId);

    // 2. Teleport Player & Camera to target entrance and set portal re-entry cooldown
    if (session->hasPendingRoomTeleport && ctx.world.IsAlive(session->playerEntity)) {
        session->hasPendingRoomTeleport = false;

        world::LocalTransform2D pLoc = ctx.world.GetLocalTransform(session->playerEntity);
        pLoc.x = session->pendingSpawnX;
        pLoc.y = session->pendingSpawnY;
        ctx.world.Teleport(session->playerEntity, pLoc);

        if (auto* ctrl = ctx.world.TryGet<PlayerController2D>(session->playerEntity)) {
            ctrl->portalCooldownTicks = 45; // 375ms portal re-entry immunity
            ctrl->facingSign          = static_cast<u8>(session->pendingSpawnFacing);
        }
        if (auto* mot = ctx.world.TryGet<gameplay::KinematicMotor2D>(session->playerEntity)) {
            mot->vx        = 0.0f;
            mot->vy        = 0.0f;
            mot->carrierId = world::kNullEntity;
        }
        if (auto* crumb = ctx.world.TryGet<gameplay::SafeGroundBreadcrumb2D>(session->playerEntity)) {
            crumb->safeX     = session->pendingSpawnX;
            crumb->safeY     = session->pendingSpawnY;
            crumb->prevSafeX = session->pendingSpawnX;
            crumb->prevSafeY = session->pendingSpawnY;
        }
    }

    // 3. Configure CameraController2D room bounds blend
    if (ctx.world.IsAlive(session->cameraEntity)) {
        const auto* roomDesc = session->FindRoom(session->activeRoomId);
        if (auto* cam = ctx.world.TryGet<gameplay::CameraController2D>(session->cameraEntity);
            cam != nullptr && roomDesc != nullptr)
        {
            cam->prevRoomBounds    = cam->activeRoomBounds;
            cam->activeRoomBounds  = roomDesc->cameraBounds;
            cam->roomBlendTicks    = 0;
            cam->roomBlendDuration = 36;
        }

        world::LocalTransform2D cLoc = ctx.world.GetLocalTransform(session->cameraEntity);
        cLoc.x = session->pendingSpawnX;
        cLoc.y = session->pendingSpawnY;
        ctx.world.Teleport(session->cameraEntity, cLoc);
    }

    ctx.world.PropagateTransforms();
    return true;
}

void RequestRoomLoad(
    GameSessionState& session,
    assets::AssetSystem& assets,
    assets::AssetId targetRoomId,
    f32 spawnX,
    f32 spawnY,
    framework::SceneTransitionMode mode,
    framework::SceneWaitPolicy waitPolicy) noexcept
{
    if (session.director == nullptr) return;
    const auto* roomDesc = session.FindRoom(targetRoomId);
    if (roomDesc == nullptr) return;

    session.activeRoomId           = roomDesc->sceneId;
    session.pendingSpawnX          = spawnX;
    session.pendingSpawnY          = spawnY;
    session.hasPendingRoomTeleport = true;

    (void)session.director->RequestSceneTransition(framework::SceneTransitionDesc{
        .sceneId              = roomDesc->sceneId,
        .manifestBlobId       = roomDesc->manifestId,
        .collisionMapId       = roomDesc->collisionId,
        .rootPrefabId         = roomDesc->prefabId,
        .cueBankId            = roomDesc->audioBankId,
        .onActivated          = &OnRoomActivated,
        .onDeactivating       = &OnRoomDeactivating,
        .musicTrackId         = roomDesc->musicTrackId,
        .musicCrossfadeBlocks = 94,
        .mode                 = mode,
        .waitPolicy           = waitPolicy
    }, assets);
}

// ============================================================================
// In-Game Debug Console Commands & CVars Registration
// ============================================================================
namespace {

void CmdGiveAllAbilities(
    void* userCtx,
    const debug::ConsoleArgs& /*args*/,
    debug::DebugConsole& console) noexcept
{
    auto* session = static_cast<GameSessionState*>(userCtx);
    if (session == nullptr || session->debugSys == nullptr) return;
    world::World* world = session->debugSys->ActiveWorld();
    gameplay::GameplayPipeline* gameplay = session->debugSys->ActiveGameplay();
    if (!world || !gameplay || !world->IsAlive(session->playerEntity)) return;

    if (auto* ctrl = world->TryGet<PlayerController2D>(session->playerEntity)) {
        ctrl->unlockedAbilities =
            AbilityMask::WallCling | AbilityMask::ShadowDash | AbilityMask::DoubleJump;
        ctrl->maxAirJumps = 2;
        ctrl->soulEnergy  = 100;
    }
    gameplay->SetWorldFlag(WorldFlags::kUnlockedWallCling, true);
    gameplay->SetWorldFlag(WorldFlags::kUnlockedShadowDash, true);
    gameplay->SetWorldFlag(WorldFlags::kUnlockedDoubleJump, true);
    console.Print(debug::LogSeverity::Success, "Unlocked WallCling, ShadowDash, and DoubleJump.");
}

void CmdHealPlayer(
    void* userCtx,
    const debug::ConsoleArgs& /*args*/,
    debug::DebugConsole& console) noexcept
{
    auto* session = static_cast<GameSessionState*>(userCtx);
    if (session == nullptr || session->debugSys == nullptr) return;
    world::World* world = session->debugSys->ActiveWorld();
    if (!world || !world->IsAlive(session->playerEntity)) return;

    if (auto* dmg = world->TryGet<gameplay::DamageReceiver2D>(session->playerEntity)) {
        dmg->hp = dmg->maxHp;
    }
    if (auto* ctrl = world->TryGet<PlayerController2D>(session->playerEntity)) {
        ctrl->soulEnergy = 100;
    }
    console.Print(debug::LogSeverity::Success, "Restored full player HP and Soul.");
}

void CmdSlayAllEnemies(
    void* userCtx,
    const debug::ConsoleArgs& /*args*/,
    debug::DebugConsole& console) noexcept
{
    auto* session = static_cast<GameSessionState*>(userCtx);
    if (session == nullptr || session->debugSys == nullptr) return;
    world::World* world = session->debugSys->ActiveWorld();
    gameplay::GameplayPipeline* gameplay = session->debugSys->ActiveGameplay();
    if (!world || !gameplay) return;

    u32 count = 0;
    for (auto [ent, brain, dmg] : world->View<EnemyBrain2D, gameplay::DamageReceiver2D>()) {
        dmg.hp             = 0;
        brain.aiState      = EnemyState::Dead;
        brain.aiTimerTicks = 1;
        if (brain.worldFlagOnDeath != 0) {
            gameplay->SetWorldFlag(brain.worldFlagOnDeath, true);
        }
        ++count;
    }
    if (count > 0) {
        console.Print(debug::LogSeverity::Success, "Slain all active room enemies.");
    }
}

} // namespace

void RegisterClientDebugTools(GameSessionState& session, debug::DebugSystem& debugSys) noexcept {
    session.debugSys = &debugSys;
    auto& console = debugSys.Console();

    // Live Movement & Combat CVars
    (void)console.RegisterCVarF32("g_run_speed",       &session.tuning.runSpeed,          2.0f, 25.0f);
    (void)console.RegisterCVarF32("g_jump_impulse",    &session.tuning.jumpImpulse,       5.0f, 30.0f);
    (void)console.RegisterCVarF32("g_dash_speed",      &session.tuning.dashSpeed,         8.0f, 40.0f);
    (void)console.RegisterCVarI32("g_coyote_ticks",    &session.tuning.coyoteTicks,       0,    30);
    (void)console.RegisterCVarI32("g_jump_buffer",     &session.tuning.jumpBufferTicks,   0,    30);
    (void)console.RegisterCVarI32("g_hitstop_light",   &session.tuning.hitstopLightTicks, 0,    30);
    (void)console.RegisterCVarI32("g_hitstop_heavy",   &session.tuning.hitstopHeavyTicks, 0,    60);
    (void)console.RegisterCVarBool("g_god",            &session.tuning.godMode);
    (void)console.RegisterCVarBool("g_infinite_soul",  &session.tuning.infiniteSoul);

    // Interactive Debug Commands
    (void)console.RegisterCommand("give_all",   &CmdGiveAllAbilities, &session);
    (void)console.RegisterCommand("heal",       &CmdHealPlayer,       &session);
    (void)console.RegisterCommand("kill_all",   &CmdSlayAllEnemies,   &session);

    // Enable key combat/kinematic overlays in development builds by default
    debugSys.SetActiveOverlays(static_cast<debug::OverlayFlags>(
        static_cast<u32>(debug::OverlayFlags::ProfilerHud) |
        static_cast<u32>(debug::OverlayFlags::CombatHitboxes)));
}

} // namespace game
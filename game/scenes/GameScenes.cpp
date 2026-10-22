#include "game/scenes/GameScenes.h"
#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/player/PlayerController.h"
#include "game/ui/HudAndMenuSystem.h"

#include <array>

namespace vots {

namespace {

[[nodiscard]] EntityId SpawnInteractableProp(
    en::World& world,
    Vec2 pos,
    InteractableKind kind,
    u8 paramByte,
    u16 reqFlag,
    u16 grantFlag,
    bool isMainRoute,
    Color tint) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = pos.x;
    sp.local.y = pos.y;
    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return kNullEntity;
    const EntityId id = *idRes;

    en::Sprite2D spr{};
    spr.width = (kind == InteractableKind::RoomExitPortal) ? 1.2f : 1.0f;
    spr.height = (kind == InteractableKind::RoomExitPortal) ? 2.2f : 1.4f;
    spr.layer = 9U;
    spr.tint = tint;
    (void)world.Emplace<en::Sprite2D>(id, spr);

    InteractableComp inter{};
    inter.kind = kind;
    inter.paramByte = paramByte;
    inter.requiredWorldFlag = reqFlag;
    inter.grantedWorldFlag = grantFlag;
    inter.isMainRouteGate = isMainRoute ? 1U : 0U;
    (void)world.Emplace<InteractableComp>(id, inter);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash("RoomProp");
    tag.categoryMask = en::kRoomTransientCategoryBit;
    (void)world.Emplace<en::Tag2D>(id, tag);
    return id;
}

[[nodiscard]] EntityId SpawnDestructibleWall(
    en::World& world,
    Vec2 pos,
    i32 durability,
    u16 unlockFlag) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = pos.x;
    sp.local.y = pos.y;
    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return kNullEntity;
    const EntityId id = *idRes;

    en::Sprite2D spr{};
    spr.width = 1.2f;
    spr.height = 2.6f;
    spr.layer = 10U;
    spr.tint = Color{0.65f, 0.50f, 0.30f, 1.0f};
    (void)world.Emplace<en::Sprite2D>(id, spr);

    en::Collider2D col{};
    col.localBounds = en::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.6f, 1.3f);
    col.filter.categoryBits = 0x0001U;
    (void)world.Emplace<en::Collider2D>(id, col);

    DestructibleTerrainComp ter{};
    ter.durability = durability;
    ter.unlockedWorldFlagOnBreak = unlockFlag;
    (void)world.Emplace<DestructibleTerrainComp>(id, ter);

    en::Tag2D tag{};
    tag.name = en::StringId::Hash("DestructibleWall");
    tag.categoryMask = en::kRoomTransientCategoryBit;
    (void)world.Emplace<en::Tag2D>(id, tag);
    return id;
}

} // namespace

en::StateLayerDesc GameScenes::MakeGameplayLayerDesc(VeilGameApp* app) noexcept {
    en::StateLayerDesc desc{};
    desc.id = static_cast<u8>(GameStateLayerId::ActiveGameplay);
    desc.inputActionMask = 0xFFFFU;
    desc.flags = en::StateLayerFlags::None;
    desc.stateData = app;
    desc.onSimTick = [](en::SimContext& ctx, const en::SimStepView& step, void* data) noexcept {
        if (auto* g = static_cast<VeilGameApp*>(data)) {
            g->OnSimTick(ctx, step);
        }
    };
    desc.onRenderExtract = [](en::RenderContext& ctx, void* data) noexcept {
        if (auto* g = static_cast<VeilGameApp*>(data)) {
            g->OnRenderExtract(ctx);
        }
    };
    return desc;
}

en::StateLayerDesc GameScenes::MakePauseModalLayerDesc(VeilGameApp* app) noexcept {
    en::StateLayerDesc desc{};
    desc.id = static_cast<u8>(GameStateLayerId::PauseModal);
    desc.inputActionMask = static_cast<u16>(
        (1U << static_cast<u8>(GameAction::Pause)) |
        (1U << static_cast<u8>(GameAction::MenuConfirm)) |
        (1U << static_cast<u8>(GameAction::MenuBack)));
    desc.flags = en::StateLayerFlags::BlockLowerSimTicks |
                 en::StateLayerFlags::BlockLowerInput |
                 en::StateLayerFlags::PauseSimClock;
    desc.stateData = app;
    desc.onSimTick = [](en::SimContext& ctx, const en::SimStepView&, void*) noexcept {
        if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Pause), 6U) ||
            ctx.actionBuffer->Consume(static_cast<u8>(GameAction::MenuBack), 6U)) {
            (void)ctx.director->PopState();
        }
    };
    desc.onRenderExtract = [](en::RenderContext& ctx, void*) noexcept {
        if (ctx.renderList) {
            HudAndMenuSystem::EmitPauseModal(*ctx.renderList);
        }
    };
    return desc;
}

en::StateLayerDesc GameScenes::MakeResonatorModalLayerDesc(VeilGameApp* app) noexcept {
    en::StateLayerDesc desc{};
    desc.id = static_cast<u8>(GameStateLayerId::ResonatorModal);
    desc.inputActionMask = 0xFFFFU;
    desc.flags = en::StateLayerFlags::BlockLowerSimTicks |
                 en::StateLayerFlags::BlockLowerInput |
                 en::StateLayerFlags::PauseSimClock;
    desc.stateData = app;
    desc.onSimTick = [](en::SimContext& ctx, const en::SimStepView&, void*) noexcept {
        if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::MenuBack), 6U) ||
            ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Interact), 6U) ||
            ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Pause), 6U)) {
            (void)ctx.director->PopState();
        }
    };
    desc.onRenderExtract = [](en::RenderContext& ctx, void* data) noexcept {
        const auto* g = static_cast<const VeilGameApp*>(data);
        if (g && ctx.renderList && ctx.world) {
            HudAndMenuSystem::EmitResonatorDeckModal(g->PlayerEntity(), *ctx.world, *ctx.renderList);
        }
    };
    return desc;
}

en::StateLayerDesc GameScenes::MakeMapModalLayerDesc(VeilGameApp* app) noexcept {
    en::StateLayerDesc desc{};
    desc.id = static_cast<u8>(GameStateLayerId::MapModal);
    desc.inputActionMask = 0xFFFFU;
    desc.flags = en::StateLayerFlags::BlockLowerSimTicks |
                 en::StateLayerFlags::BlockLowerInput |
                 en::StateLayerFlags::PauseSimClock;
    desc.stateData = app;
    desc.onSimTick = [](en::SimContext& ctx, const en::SimStepView&, void*) noexcept {
        if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::ToggleMap), 6U) ||
            ctx.actionBuffer->Consume(static_cast<u8>(GameAction::MenuBack), 6U) ||
            ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Pause), 6U)) {
            (void)ctx.director->PopState();
        }
    };
    desc.onRenderExtract = [](en::RenderContext& ctx, void* data) noexcept {
        const auto* g = static_cast<const VeilGameApp*>(data);
        if (g && ctx.renderList) {
            HudAndMenuSystem::EmitMapAndPinsModal(g->Metroidvania(), g->ActiveRoomId(), *ctx.renderList);
        }
    };
    return desc;
}

en::StateLayerDesc GameScenes::MakeEndingModalLayerDesc(VeilGameApp* app) noexcept {
    en::StateLayerDesc desc{};
    desc.id = static_cast<u8>(GameStateLayerId::EndingModal);
    desc.inputActionMask = 0xFFFFU;
    desc.flags = en::StateLayerFlags::BlockLowerSimTicks |
                 en::StateLayerFlags::BlockLowerInput |
                 en::StateLayerFlags::PauseSimClock;
    desc.stateData = app;
    desc.onSimTick = [](en::SimContext& ctx, const en::SimStepView&, void*) noexcept {
        if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::MenuBack), 6U)) {
            (void)ctx.director->PopState();
        }
    };
    desc.onRenderExtract = [](en::RenderContext& ctx, void* data) noexcept {
        const auto* g = static_cast<const VeilGameApp*>(data);
        if (g && ctx.renderList) {
            HudAndMenuSystem::EmitEndingModal(g->Metroidvania().AchievedEnding(), *ctx.renderList);
        }
    };
    return desc;
}

en::Result<void, en::FrameworkErrc> GameScenes::RequestRoomTransition(
    VeilGameApp& app,
    u8 targetRoomId,
    en::SceneTransitionMode mode) noexcept {
    const u8 clampedRoom = static_cast<u8>(targetRoomId % kRoomCount);
    const u8 prevRoom = app.ActiveRoomId();

    if (mode == en::SceneTransitionMode::FullReset) {
        app.SetTransitionSpawnX(-4.0f);
    } else if (clampedRoom > prevRoom) {
        app.SetTransitionSpawnX(-7.5f); // Entered from left portal
    } else if (clampedRoom < prevRoom) {
        app.SetTransitionSpawnX(17.5f); // Entered from right portal
    } else {
        app.SetTransitionSpawnX(app.Metroidvania().LastCheckpointPos().x);
    }

    app.SetPendingRoomId(clampedRoom);

    constexpr std::array<std::string_view, kRoomCount> kSceneNames{{
        "room_ash_sump",
        "room_resonator_sanctum",
        "room_echo_vault",
        "room_spire_arena",
    }};

    en::SceneTransitionDesc desc{};
    desc.sceneId = en::AssetId::Hash(kSceneNames[clampedRoom]);
    desc.manifestBlobId = app.AssetHandles().roomManifestIds[clampedRoom];
    desc.musicTrackId = 100U + clampedRoom;
    desc.musicCrossfadeBlocks = 16U;
    desc.transitionDurationTicks = 2U;
    desc.mode = mode;
    desc.wipe = en::TransitionWipeKind::FadeToBlack;
    desc.onActivated = [](en::SceneLoadContext& lctx) noexcept -> en::Result<void, en::FrameworkErrc> {
        auto* g = static_cast<VeilGameApp*>(lctx.userAppState);
        if (!g) return en::Err(en::FrameworkErrc::ClientBootFailed);
        return PopulateRoom(*g, lctx, g->PendingRoomId());
    };
    return app.EngineApp().Director().RequestSceneTransition(desc, app.EngineApp().Assets());
}

void GameScenes::SpawnRoomEnemiesIfNeeded(
    VeilGameApp& app,
    en::World& world,
    u8 roomId) noexcept {
    // Track which enemy slots are already alive in the room
    u8 existingSlots = 0U;
    for (auto [id, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == roomId && !recv.HasFlag(en::ReceiverFlags::Dead) && recv.hp > 0) {
            existingSlots |= ai.spawnSlotBit;
        }
    }

    if (roomId == 0U) {
        if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(0U, 0x01U)) {
            (void)EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{4.0f, 1.0f}, 0U, 0x01U);
        }
        if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(0U, 0x02U)) {
            (void)EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{6.5f, 1.0f}, 0U, 0x02U);
        }
    } else if (roomId == 1U) {
        if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(1U, 0x01U)) {
            (void)EnemyAiSystem::SpawnChoirEnforcer(world, app.AssetHandles(), Vec2{5.0f, 1.0f}, 1U, 0x01U);
        }
        if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(1U, 0x02U)) {
            (void)EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{11.5f, 1.0f}, 1U, 0x02U);
        }
    } else if (roomId == 2U) {
        if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(2U, 0x01U)) {
            (void)EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{3.5f, 1.0f}, 2U, 0x01U);
        }
        if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(2U, 0x02U)) {
            (void)EnemyAiSystem::SpawnChoirEnforcer(world, app.AssetHandles(), Vec2{15.0f, 1.0f}, 2U, 0x02U);
        }
    }
}

en::Result<void, en::FrameworkErrc> GameScenes::PopulateRoom(
    VeilGameApp& app,
    en::SceneLoadContext& ctx,
    u8 roomId) noexcept {
    app.SetActiveRoomId(roomId);
    app.Metroidvania().DiscoverRoom(roomId);
    app.SetPortalCooldownTicks(35U);

    // 1. Build enclosed 36x12 tilemap (world X in [-12, +24], world Y in [-1, +11]):
    //    - Solid floor (ty = 0), solid ceiling (ty = 11), solid left/right boundary walls (tx = 0..1, tx = 34..35)
    //    - Stepped OneWay platforms at ty = 3 (world Y = 2.0) and ty = 6 (world Y = 5.0)
    constexpr u32 kMapW = 36U;
    constexpr u32 kMapH = 12U;
    std::array<en::TileMaterialFlags, kMapW * kMapH> tiles{};
    for (u32 x = 0U; x < kMapW; ++x) {
        tiles[0U * kMapW + x] = en::TileMaterialFlags::Solid;              // Ground floor (y = -1..0)
        tiles[(kMapH - 1U) * kMapW + x] = en::TileMaterialFlags::Solid;    // Ceiling (y = 10..11)
    }
    for (u32 y = 0U; y < kMapH; ++y) {
        tiles[y * kMapW + 0U] = en::TileMaterialFlags::Solid;              // Left outer wall
        tiles[y * kMapW + 1U] = en::TileMaterialFlags::Solid;
        tiles[y * kMapW + (kMapW - 2U)] = en::TileMaterialFlags::Solid;    // Right outer wall
        tiles[y * kMapW + (kMapW - 1U)] = en::TileMaterialFlags::Solid;
    }
    // Lower OneWay platforms (world X = -4..+2 and +8..+15, world Y = 2.0)
    for (u32 x = 8U; x < 15U; ++x) {
        tiles[3U * kMapW + x] = en::TileMaterialFlags::OneWay;
    }
    for (u32 x = 20U; x < 27U; ++x) {
        tiles[3U * kMapW + x] = en::TileMaterialFlags::OneWay;
    }
    // Upper OneWay platform (world X = +1..+9, world Y = 5.0)
    for (u32 x = 13U; x < 21U; ++x) {
        tiles[6U * kMapW + x] = en::TileMaterialFlags::OneWay;
    }
    (void)ctx.physics->BuildTileMap(kMapW, kMapH, 1.0f, -12.0f, -1.0f, tiles);

    // 2. Spawn or reposition Kaelen & Camera rig across RoomSwap / FullReset
    const f32 spawnX = app.TransitionSpawnX();
    if (!ctx.world->IsAlive(app.PlayerEntity())) {
        auto heroRes = PlayerController::SpawnKaelen(*ctx.world, app.AssetHandles(), Vec2{spawnX, 1.0f}, roomId);
        if (!heroRes.has_value()) return en::Err(en::FrameworkErrc::ClientBootFailed);
        app.SetPlayerEntity(*heroRes);
    } else {
        if (auto* ps = ctx.world->TryGet<PlayerStateComp>(app.PlayerEntity())) {
            ps->activeRoomId = roomId;
            ps->airDashCharges = ps->maxAirDashCharges;
        }
        if (auto* motor = ctx.world->TryGet<en::CharacterMotor2D>(app.PlayerEntity())) {
            motor->vx = 0.0f;
            motor->vy = 0.0f;
        }
        if (auto* anchor = ctx.world->TryGet<en::SafeAnchor2D>(app.PlayerEntity())) {
            anchor->safeX = spawnX;
            anchor->safeY = 1.0f;
        }
        en::LocalTransform2D local = ctx.world->GetLocalTransform(app.PlayerEntity());
        local.x = spawnX;
        local.y = 1.0f;
        ctx.world->Teleport(app.PlayerEntity(), local);
    }

    if (!ctx.world->IsAlive(app.CameraEntity())) {
        en::EntitySpawnDesc camSp{};
        camSp.local.x = spawnX;
        camSp.local.y = 3.0f;
        camSp.timeChannel = en::TimeChannel::Camera;
        const EntityId camId = ctx.world->Spawn(camSp).value_or(kNullEntity);
        app.SetCameraEntity(camId);

        en::CameraRig2D rig{};
        rig.pixelsPerMeter = 48.0f;
        rig.active = true;
        (void)ctx.world->Emplace<en::CameraRig2D>(camId, rig);

        en::CameraController2D ctrl{};
        ctrl.primaryTarget = app.PlayerEntity();
        ctrl.activeRoomBounds = en::Rect{-12.0f, -1.0f, 36.0f, 12.0f};
        ctrl.flags = en::CameraCtrlFlags::EnableLookahead | en::CameraCtrlFlags::ConfineToRoom | en::CameraCtrlFlags::SnapNextStep;
        (void)ctx.world->Emplace<en::CameraController2D>(camId, ctrl);
    } else {
        if (auto* ctrl = ctx.world->TryGet<en::CameraController2D>(app.CameraEntity())) {
            ctrl->primaryTarget = app.PlayerEntity();
            ctrl->secondaryTarget = kNullEntity;
            ctrl->secondaryWeight_q8 = 0U;
            ctrl->activeRoomBounds = en::Rect{-12.0f, -1.0f, 36.0f, 12.0f};
            ctrl->flags |= en::CameraCtrlFlags::ConfineToRoom | en::CameraCtrlFlags::SnapNextStep;
        }
    }

    // 3. Populate room-specific props, checkpoints, portals, enemies, gates, and Boss
    app.SetBossEntity(kNullEntity);

    // Every room has a Tuning Resonator checkpoint at x = -6.0f
    (void)SpawnInteractableProp(
        *ctx.world, Vec2{-6.0f, 1.0f}, InteractableKind::TuningResonator, roomId, 0U, 0U, true,
        Color{0.30f, 0.90f, 1.0f, 1.0f});

    // Left & Right Room Exit Portals so players can walk between rooms 0 <-> 1 <-> 2 <-> 3
    if (roomId > 0U) {
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{-9.2f, 1.1f}, InteractableKind::RoomExitPortal,
            static_cast<u8>(roomId - 1U), 0U, 0U, true, Color{0.35f, 0.85f, 1.0f, 1.0f});
    }
    if (roomId + 1U < kRoomCount) {
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{20.5f, 1.1f}, InteractableKind::RoomExitPortal,
            static_cast<u8>(roomId + 1U), 0U, 0U, true, Color{0.35f, 0.85f, 1.0f, 1.0f});
    }

    // Spawn Lost Wick Husk prop if active in this room
    if (app.Metroidvania().ActiveHusk().IsActive() && app.Metroidvania().ActiveHusk().roomId == roomId) {
        (void)SpawnInteractableProp(
            *ctx.world,
            Vec2{app.Metroidvania().ActiveHusk().worldX, app.Metroidvania().ActiveHusk().worldY},
            InteractableKind::LostWickHusk,
            roomId, 0U, 0U, false, Color{1.0f, 0.82f, 0.25f, 1.0f});
    }

    // Spawn non-cleared room enemies
    SpawnRoomEnemiesIfNeeded(app, *ctx.world, roomId);

    if (roomId == 0U) {
        // Room 0: Ash-Sump Slums — Ventus Echo Altar, Frequency Gate to Sanctum
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{9.5f, 1.0f}, InteractableKind::EchoAltar,
            static_cast<u8>(EchoId::Ventus), 0U, 0U, true, GetEchoDef(EchoId::Ventus).primaryColor);
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{14.5f, 1.0f}, InteractableKind::FrequencyGate,
            1U, static_cast<u16>(WorldFlagId::UnlockedGrappleHook),
            static_cast<u16>(WorldFlagId::GateSanctumOpened), true, Color{0.95f, 0.75f, 0.25f, 1.0f});
    } else if (roomId == 1U) {
        // Room 1: Resonator Sanctum — Lore Tablet, Memory Tuning Fork 1
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{9.0f, 1.0f}, InteractableKind::LoreTablet,
            1U, 0U, static_cast<u16>(WorldFlagId::LoreTablet01Read), false, Color{0.85f, 0.85f, 0.95f, 1.0f});
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{14.0f, 1.0f}, InteractableKind::MemoryTuningFork,
            0U, 0U, static_cast<u16>(WorldFlagId::MemoryTuningQuest01Done), false, Color{0.95f, 0.55f, 0.85f, 1.0f});
    } else if (roomId == 2U) {
        // Room 2: Echo Vault — Elemental Cataclysm Destructible Wall, Echo Altar, Memory Tuning Fork 2
        (void)SpawnDestructibleWall(
            *ctx.world, Vec2{6.5f, 1.3f}, 30, static_cast<u16>(WorldFlagId::GateSpireOpened));
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{10.5f, 1.0f}, InteractableKind::EchoAltar,
            static_cast<u8>(EchoId::Aqua), 0U, 0U, false, GetEchoDef(EchoId::Aqua).primaryColor);
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{13.5f, 1.0f}, InteractableKind::MemoryTuningFork,
            1U, 0U, static_cast<u16>(WorldFlagId::MemoryTuningQuest02Done), false, Color{0.95f, 0.55f, 0.85f, 1.0f});
    } else if (roomId == 3U) {
        // Room 3: Spire of Unison Boss Arena — High Conductor 3-Phase Boss & Spire Decision Core
        if (!ctx.gameplay->TestWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated))) {
            auto bossRes = EnemyAiSystem::SpawnHighConductorBoss(*ctx.world, app.AssetHandles(), Vec2{6.0f, 1.35f});
            if (bossRes.has_value()) {
                app.SetBossEntity(*bossRes);
                if (auto* camCtrl = ctx.world->TryGet<en::CameraController2D>(app.CameraEntity())) {
                    camCtrl->secondaryTarget = *bossRes;
                    camCtrl->secondaryWeight_q8 = 96U;
                }
            }
        }
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{13.0f, 1.0f}, InteractableKind::SpireDecisionCore,
            0U, static_cast<u16>(WorldFlagId::HighConductorDefeated), 0U, true, Color{1.0f, 0.95f, 0.60f, 1.0f});
    }

    return {};
}

} // namespace vots

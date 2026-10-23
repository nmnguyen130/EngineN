#include "game/scenes/GameScenes.h"
#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/camera/RoomCameraController.h"
#include "game/player/PlayerController.h"
#include "game/ui/HudAndMenuSystem.h"
#include "game/world/WorldTopology.h"

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
    Color tint,
    u8 priority = 10U) noexcept {
    en::EntitySpawnDesc sp{};
    sp.local.x = pos.x;
    sp.local.y = pos.y;
    auto idRes = world.Spawn(sp);
    if (!idRes.has_value()) return kNullEntity;
    const EntityId id = *idRes;

    en::Sprite2D spr{};
    spr.width = 1.0f;
    spr.height = 1.4f;
    spr.layer = 9U;
    spr.tint = tint;
    (void)world.Emplace<en::Sprite2D>(id, spr);

    InteractableComp inter{};
    inter.kind = kind;
    inter.paramByte = paramByte;
    inter.requiredWorldFlag = reqFlag;
    inter.grantedWorldFlag = grantFlag;
    inter.isMainRouteGate = isMainRoute ? 1U : 0U;
    inter.interactPriority = priority;
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
    const RoomTopologySpec& targetSpec = GetRoomTopology(clampedRoom);

    if (mode == en::SceneTransitionMode::FullReset) {
        app.SetTransitionSpawnX(-4.0f);
    } else if (prevRoom == 7U && clampedRoom == 0U) {
        app.SetTransitionSpawnX(-5.2f);
    } else if (clampedRoom > prevRoom) {
        app.SetTransitionSpawnX(targetSpec.WestEntranceSpawnX());
    } else if (clampedRoom < prevRoom) {
        app.SetTransitionSpawnX(targetSpec.EastEntranceSpawnX());
    } else {
        app.SetTransitionSpawnX(app.Metroidvania().LastCheckpointPos().x);
    }

    app.SetPendingRoomId(clampedRoom);

    constexpr std::array<std::string_view, kRoomCount> kSceneNames{{
        "room_00_ash_sump_slums",
        "room_01_collapsed_mine_junction",
        "room_02_glass_conservatory",
        "room_03_choirworks",
        "room_04_drowned_archive",
        "room_05_gravity_foundry",
        "room_06_warden_spire_arena",
        "room_07_spire_overlook_shortcut",
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
    u8 existingSlots = 0U;
    for (auto [id, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == roomId && !recv.HasFlag(en::ReceiverFlags::Dead) && recv.hp > 0) {
            existingSlots |= ai.spawnSlotBit;
        }
    }

    switch (roomId) {
        case 0U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(0U, 0x01U)) {
                (void)EnemyAiSystem::SpawnDissonantDrifter(world, app.AssetHandles(), Vec2{4.0f, 1.0f}, 0U, 0x01U);
            }
            if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(0U, 0x02U)) {
                (void)EnemyAiSystem::SpawnDissonantDrifter(world, app.AssetHandles(), Vec2{8.5f, 1.0f}, 0U, 0x02U);
            }
            break;
        case 1U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(1U, 0x01U)) {
                (void)EnemyAiSystem::SpawnDissonantDrifter(world, app.AssetHandles(), Vec2{4.5f, 1.0f}, 1U, 0x01U);
            }
            if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(1U, 0x02U)) {
                (void)EnemyAiSystem::SpawnChoirSentry(world, app.AssetHandles(), Vec2{14.0f, 1.0f}, 1U, 0x02U);
            }
            break;
        case 2U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(2U, 0x01U)) {
                (void)EnemyAiSystem::SpawnResonanceBrute(world, app.AssetHandles(), Vec2{4.0f, 1.1f}, 2U, 0x01U);
            }
            if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(2U, 0x02U)) {
                (void)EnemyAiSystem::SpawnDissonantDrifter(world, app.AssetHandles(), Vec2{18.0f, 1.0f}, 2U, 0x02U);
            }
            break;
        case 3U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(3U, 0x01U)) {
                (void)EnemyAiSystem::SpawnChoirSentry(world, app.AssetHandles(), Vec2{5.0f, 1.0f}, 3U, 0x01U);
            }
            if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(3U, 0x02U)) {
                (void)EnemyAiSystem::SpawnTuningHuskElite(world, app.AssetHandles(), Vec2{15.5f, 1.0f}, 3U, 0x02U);
            }
            break;
        case 4U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(4U, 0x01U)) {
                (void)EnemyAiSystem::SpawnResonanceBrute(world, app.AssetHandles(), Vec2{6.0f, 1.1f}, 4U, 0x01U);
            }
            if ((existingSlots & 0x02U) == 0U && !app.Metroidvania().IsEnemyCleared(4U, 0x02U)) {
                (void)EnemyAiSystem::SpawnChoirSentry(world, app.AssetHandles(), Vec2{17.5f, 1.0f}, 4U, 0x02U);
            }
            break;
        case 5U:
            if ((existingSlots & 0x01U) == 0U && !app.Metroidvania().IsEnemyCleared(5U, 0x01U)) {
                (void)EnemyAiSystem::SpawnTuningHuskElite(world, app.AssetHandles(), Vec2{7.5f, 1.0f}, 5U, 0x01U);
            }
            break;
        default:
            break;
    }
}

en::Result<void, en::FrameworkErrc> GameScenes::PopulateRoom(
    VeilGameApp& app,
    en::SceneLoadContext& ctx,
    u8 roomId) noexcept {
    app.SetActiveRoomId(roomId);
    app.Metroidvania().DiscoverRoom(roomId);
    app.SetPortalCooldownTicks(35U);

    const RoomTopologySpec& roomSpec = GetRoomTopology(roomId);
    const u32 mapW = roomSpec.tileWidth;
    const u32 mapH = roomSpec.tileHeight;

    // Max room grid is 50 x 22 = 1100 tiles (zero dynamic heap allocation)
    std::array<en::TileMaterialFlags, 64U * 24U> tiles{};
    for (u32 x = 0U; x < mapW; ++x) {
        tiles[0U * mapW + x] = en::TileMaterialFlags::Solid;
        tiles[(mapH - 1U) * mapW + x] = en::TileMaterialFlags::Solid;
    }
    for (u32 y = 0U; y < mapH; ++y) {
        tiles[y * mapW + 0U] = en::TileMaterialFlags::Solid;
        tiles[y * mapW + 1U] = en::TileMaterialFlags::Solid;
        tiles[y * mapW + (mapW - 2U)] = en::TileMaterialFlags::Solid;
        tiles[y * mapW + (mapW - 1U)] = en::TileMaterialFlags::Solid;
    }

    // Carve open West / East outer boundary passages so Kaelen walks off the edge of the room like Hollow Knight
    if (roomSpec.westNeighborRoomId < kRoomCount) {
        for (u32 y = 1U; y <= 4U; ++y) {
            tiles[y * mapW + 0U] = en::TileMaterialFlags::Empty;
            tiles[y * mapW + 1U] = en::TileMaterialFlags::Empty;
        }
    }
    if (roomSpec.eastNeighborRoomId < kRoomCount) {
        for (u32 y = 1U; y <= 4U; ++y) {
            tiles[y * mapW + (mapW - 2U)] = en::TileMaterialFlags::Empty;
            tiles[y * mapW + (mapW - 1U)] = en::TileMaterialFlags::Empty;
        }
    }

    // Base lower & mid platforms
    for (u32 x = 8U; x < 15U && x + 2U < mapW; ++x) {
        tiles[3U * mapW + x] = en::TileMaterialFlags::OneWay;
    }
    for (u32 x = 20U; x < 27U && x + 2U < mapW; ++x) {
        tiles[3U * mapW + x] = en::TileMaterialFlags::OneWay;
    }
    for (u32 x = 13U; x < 21U && x + 2U < mapW; ++x) {
        tiles[6U * mapW + x] = en::TileMaterialFlags::OneWay;
    }

    // Room-role specific geometry (GDD v0.5 Section 2–4: Long Promenades vs Tall Vertical Shafts)
    if (mapW >= 44U) {
        for (u32 x = 30U; x < mapW - 5U; ++x) {
            tiles[4U * mapW + x] = en::TileMaterialFlags::OneWay;
        }
        for (u32 x = 25U; x < 33U && x + 2U < mapW; ++x) {
            tiles[7U * mapW + x] = en::TileMaterialFlags::OneWay;
        }
    }
    if (roomSpec.role == RoomRole::VerticalShaft && mapH >= 20U) {
        for (u32 x = 6U; x < 13U; ++x)  tiles[9U * mapW + x]  = en::TileMaterialFlags::OneWay;
        for (u32 x = 16U; x < 24U; ++x) tiles[12U * mapW + x] = en::TileMaterialFlags::OneWay;
        for (u32 x = 9U; x < 18U; ++x)  tiles[15U * mapW + x] = en::TileMaterialFlags::OneWay;
    }

    (void)ctx.physics->BuildTileMap(
        static_cast<u16>(mapW),
        static_cast<u16>(mapH),
        1.0f,
        roomSpec.originX,
        roomSpec.originY,
        std::span<const en::TileMaterialFlags>{tiles.data(), static_cast<usize>(mapW) * mapH});

    const f32 spawnX = app.TransitionSpawnX();
    if (!ctx.world->IsAlive(app.PlayerEntity())) {
        auto heroRes = PlayerController::SpawnKaelen(*ctx.world, app.AssetHandles(), Vec2{spawnX, 1.0f}, roomId);
        if (!heroRes.has_value()) return en::Err(en::FrameworkErrc::ClientBootFailed);
        app.SetPlayerEntity(*heroRes);
    } else {
        if (auto* ps = ctx.world->TryGet<PlayerStateComp>(app.PlayerEntity())) {
            ps->activeRoomId = roomId;
            ps->airDashCharges = ps->MaxDashCharges();
            ps->airJumpsRemaining = (ps->doubleJumpEnabled != 0U) ? 1U : 0U;
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

    app.SetBossEntity(kNullEntity);

    if (roomId == 0U || roomId == 5U) {
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{-6.0f, 1.0f}, InteractableKind::TuningResonator, roomId, 0U, 0U, true,
            Color{0.30f, 0.90f, 1.0f, 1.0f}, 2U);
    }

    if (app.Metroidvania().ActiveHusk().IsActive() && app.Metroidvania().ActiveHusk().roomId == roomId) {
        (void)SpawnInteractableProp(
            *ctx.world,
            Vec2{app.Metroidvania().ActiveHusk().worldX, app.Metroidvania().ActiveHusk().worldY},
            InteractableKind::LostWickHusk,
            roomId, 0U, 0U, false, Color{1.0f, 0.82f, 0.25f, 1.0f}, 1U);
    }

    SpawnRoomEnemiesIfNeeded(app, *ctx.world, roomId);

    switch (roomId) {
        case 0U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{-3.5f, 1.0f}, InteractableKind::FrequencyGate,
                7U, static_cast<u16>(WorldFlagId::ShortcutToAshSumpOpened),
                static_cast<u16>(WorldFlagId::ShortcutToAshSumpOpened), false,
                Color{0.45f, 0.95f, 0.65f, 1.0f}, 8U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{11.5f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Ventus), 0U, 0U, true, GetEchoDef(EchoId::Ventus).primaryColor, 5U);
            break;

        case 1U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{8.5f, 1.0f}, InteractableKind::LoreTablet,
                1U, 0U, static_cast<u16>(WorldFlagId::LoreTablet01Read), false,
                Color{0.85f, 0.85f, 0.95f, 1.0f}, 6U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{18.0f, 1.0f}, InteractableKind::MemoryTuningFork,
                0U, 0U, static_cast<u16>(WorldFlagId::MemoryTuningQuest01Done), false,
                Color{0.95f, 0.55f, 0.85f, 1.0f}, 5U);
            break;

        case 2U:
            (void)SpawnDestructibleWall(
                *ctx.world, Vec2{8.5f, 1.3f}, 30, static_cast<u16>(WorldFlagId::GateSpireOpened));
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{14.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Aqua), 0U, 0U, false, GetEchoDef(EchoId::Aqua).primaryColor, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{24.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Lux), 0U, static_cast<u16>(WorldFlagId::PrismKeeperDefeated),
                false, GetEchoDef(EchoId::Lux).primaryColor, 5U);
            break;

        case 3U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{6.5f, 1.0f}, InteractableKind::LoreTablet,
                2U, 0U, static_cast<u16>(WorldFlagId::LoreTablet02Read), false,
                Color{0.85f, 0.85f, 0.95f, 1.0f}, 6U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{11.0f, 1.0f}, InteractableKind::MemoryTuningFork,
                1U, 0U, static_cast<u16>(WorldFlagId::ConductorSealChoirworks), false,
                Color{0.95f, 0.55f, 0.85f, 1.0f}, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{20.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Stridor), 0U, 0U, false, GetEchoDef(EchoId::Stridor).primaryColor, 5U);
            break;

        case 4U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{7.0f, 1.0f}, InteractableKind::LoreTablet,
                3U, 0U, static_cast<u16>(WorldFlagId::LoreTablet03Read), false,
                Color{0.85f, 0.85f, 0.95f, 1.0f}, 6U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{11.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Fulgur), 0U, static_cast<u16>(WorldFlagId::ConductorSealArchive),
                false, GetEchoDef(EchoId::Fulgur).primaryColor, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{15.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Aether), 0U, 0U, false, GetEchoDef(EchoId::Aether).primaryColor, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{22.0f, 1.0f}, InteractableKind::FrequencyGate,
                1U, static_cast<u16>(WorldFlagId::UnlockedGrappleHook),
                static_cast<u16>(WorldFlagId::GateSanctumOpened), true,
                Color{0.95f, 0.75f, 0.25f, 1.0f}, 8U);
            break;

        case 5U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{3.5f, 1.0f}, InteractableKind::LoreTablet,
                4U, 0U, static_cast<u16>(WorldFlagId::LoreTablet04Read), false,
                Color{0.85f, 0.85f, 0.95f, 1.0f}, 6U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{10.5f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Gravitas), 0U, 0U, false, GetEchoDef(EchoId::Gravitas).primaryColor, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{13.0f, 1.0f}, InteractableKind::MemoryTuningFork,
                2U, 0U, static_cast<u16>(WorldFlagId::ConductorSealFoundry), false,
                Color{0.95f, 0.55f, 0.85f, 1.0f}, 5U);
            break;

        case 6U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{-4.5f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Umbra), 0U, 0U, false, GetEchoDef(EchoId::Umbra).primaryColor, 5U);
            if (!ctx.gameplay->TestWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated))) {
                auto bossRes = EnemyAiSystem::SpawnAshSumpWardenBoss(*ctx.world, app.AssetHandles(), Vec2{6.0f, 1.35f}, 6U);
                if (bossRes.has_value()) {
                    app.SetBossEntity(*bossRes);
                }
            }
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{16.5f, 1.0f}, InteractableKind::SpireDecisionCore,
                0U, static_cast<u16>(WorldFlagId::HighConductorDefeated), 0U, true,
                Color{1.0f, 0.95f, 0.60f, 1.0f}, 4U);
            break;

        case 7U:
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{2.0f, 1.0f}, InteractableKind::EchoAltar,
                static_cast<u8>(EchoId::Tempus), 0U, 0U, false, GetEchoDef(EchoId::Tempus).primaryColor, 5U);
            (void)SpawnInteractableProp(
                *ctx.world, Vec2{6.0f, 1.0f}, InteractableKind::ShortcutLever,
                0U, 0U, static_cast<u16>(WorldFlagId::ShortcutToAshSumpOpened), true,
                Color{0.40f, 0.98f, 0.58f, 1.0f}, 3U);
            break;

        default:
            break;
    }

    const EntityId camId = RoomCameraController::ConfigureRoomCamera(
        app.CameraEntity(), app.PlayerEntity(), app.BossEntity(), roomId, Vec2{spawnX, 1.0f}, *ctx.world);
    app.SetCameraEntity(camId);

    return {};
}

} // namespace vots

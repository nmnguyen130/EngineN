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

en::Result<void, en::FrameworkErrc> GameScenes::PopulateRoom(
    VeilGameApp& app,
    en::SceneLoadContext& ctx,
    u8 roomId) noexcept {
    app.SetActiveRoomId(roomId);
    app.Metroidvania().DiscoverRoom(roomId);

    // 1. Build 32x8 static tilemap floor + one-way ledges for the active room
    std::array<en::TileMaterialFlags, 32U * 8U> tiles{};
    for (u32 x = 0U; x < 32U; ++x) {
        tiles[x] = en::TileMaterialFlags::Solid; // y = 0 floor row
    }
    for (u32 x = 8U; x < 16U; ++x) {
        tiles[3U * 32U + x] = en::TileMaterialFlags::OneWay; // elevated one-way platform
    }
    (void)ctx.physics->BuildTileMap(32U, 8U, 1.0f, -10.0f, -1.0f, tiles);

    // 2. Spawn or preserve Kaelen & Camera rig across RoomSwap / FullReset
    if (!ctx.world->IsAlive(app.PlayerEntity())) {
        auto heroRes = PlayerController::SpawnKaelen(*ctx.world, app.AssetHandles(), Vec2{0.0f, 1.0f}, roomId);
        if (!heroRes.has_value()) return en::Err(en::FrameworkErrc::ClientBootFailed);
        app.SetPlayerEntity(*heroRes);
    } else {
        if (auto* ps = ctx.world->TryGet<PlayerStateComp>(app.PlayerEntity())) {
            ps->activeRoomId = roomId;
        }
        en::LocalTransform2D local = ctx.world->GetLocalTransform(app.PlayerEntity());
        local.x = 0.0f;
        local.y = 1.0f;
        ctx.world->Teleport(app.PlayerEntity(), local);
    }

    if (!ctx.world->IsAlive(app.CameraEntity())) {
        en::EntitySpawnDesc camSp{};
        camSp.local.x = 0.0f;
        camSp.local.y = 2.0f;
        camSp.timeChannel = en::TimeChannel::Camera;
        const EntityId camId = ctx.world->Spawn(camSp).value_or(kNullEntity);
        app.SetCameraEntity(camId);

        en::CameraRig2D rig{};
        rig.pixelsPerMeter = 48.0f;
        rig.active = true;
        (void)ctx.world->Emplace<en::CameraRig2D>(camId, rig);

        en::CameraController2D ctrl{};
        ctrl.primaryTarget = app.PlayerEntity();
        ctrl.activeRoomBounds = en::Rect{-10.0f, -2.0f, 32.0f, 14.0f};
        (void)ctx.world->Emplace<en::CameraController2D>(camId, ctrl);
    }

    // 3. Populate room-specific props, checkpoints, enemies, gates, and Boss
    app.SetBossEntity(kNullEntity);

    // Every room has a Tuning Resonator checkpoint at x = -2.0f
    (void)SpawnInteractableProp(
        *ctx.world, Vec2{-2.0f, 1.0f}, InteractableKind::TuningResonator, roomId, 0U, 0U, true,
        Color{0.30f, 0.90f, 1.0f, 1.0f});

    if (roomId == 0U) {
        // Room 0: Ash-Sump Slums — Dissonant Husks, Ventus Echo Altar, Frequency Gate to Sanctum
        (void)EnemyAiSystem::SpawnDissonantHusk(*ctx.world, app.AssetHandles(), Vec2{4.0f, 1.0f});
        (void)EnemyAiSystem::SpawnDissonantHusk(*ctx.world, app.AssetHandles(), Vec2{5.5f, 1.0f});
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{8.0f, 1.0f}, InteractableKind::EchoAltar,
            static_cast<u8>(EchoId::Ventus), 0U, 0U, true, GetEchoDef(EchoId::Ventus).primaryColor);
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{14.0f, 1.0f}, InteractableKind::FrequencyGate,
            1U, static_cast<u16>(WorldFlagId::UnlockedGrappleHook),
            static_cast<u16>(WorldFlagId::GateSanctumOpened), true, Color{0.95f, 0.75f, 0.25f, 1.0f});
    } else if (roomId == 1U) {
        // Room 1: Resonator Sanctum — Heavy Choir Enforcer (BreakableGuard), Lore Tablet, Memory Tuning Fork 1
        (void)EnemyAiSystem::SpawnChoirEnforcer(*ctx.world, app.AssetHandles(), Vec2{5.0f, 1.0f});
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{9.0f, 1.0f}, InteractableKind::LoreTablet,
            1U, 0U, static_cast<u16>(WorldFlagId::LoreTablet01Read), false, Color{0.85f, 0.85f, 0.95f, 1.0f});
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{12.0f, 1.0f}, InteractableKind::MemoryTuningFork,
            0U, 0U, static_cast<u16>(WorldFlagId::MemoryTuningQuest01Done), false, Color{0.95f, 0.55f, 0.85f, 1.0f});
    } else if (roomId == 2U) {
        // Room 2: Echo Vault — Elemental Cataclysm Destructible Wall, Echo Altars, Memory Tuning Fork 2
        (void)SpawnDestructibleWall(
            *ctx.world, Vec2{6.0f, 1.3f}, 30, static_cast<u16>(WorldFlagId::GateSpireOpened));
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{10.0f, 1.0f}, InteractableKind::EchoAltar,
            static_cast<u8>(EchoId::Aqua), 0U, 0U, false, GetEchoDef(EchoId::Aqua).primaryColor);
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{13.0f, 1.0f}, InteractableKind::MemoryTuningFork,
            1U, 0U, static_cast<u16>(WorldFlagId::MemoryTuningQuest02Done), false, Color{0.95f, 0.55f, 0.85f, 1.0f});
    } else if (roomId == 3U) {
        // Room 3: Spire of Unison Boss Arena — High Conductor 3-Phase Boss & Spire Decision Core
        auto bossRes = EnemyAiSystem::SpawnHighConductorBoss(*ctx.world, app.AssetHandles(), Vec2{6.0f, 1.35f});
        if (bossRes.has_value()) {
            app.SetBossEntity(*bossRes);
            if (auto* camCtrl = ctx.world->TryGet<en::CameraController2D>(app.CameraEntity())) {
                camCtrl->secondaryTarget = *bossRes;
                camCtrl->secondaryWeight_q8 = 96U;
            }
        }
        (void)SpawnInteractableProp(
            *ctx.world, Vec2{12.0f, 1.0f}, InteractableKind::SpireDecisionCore,
            0U, static_cast<u16>(WorldFlagId::HighConductorDefeated), 0U, true, Color{1.0f, 0.95f, 0.60f, 1.0f});
    }

    return {};
}

} // namespace vots

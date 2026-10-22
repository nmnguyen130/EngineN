#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/player/PlayerController.h"
#include "game/scenes/GameScenes.h"
#include "game/ui/HudAndMenuSystem.h"

#include <cmath>

namespace vots {

en::Result<void, en::FrameworkErrc> VeilGameApp::OnRegisterComponentsHook(
    en::World& world,
    void*) noexcept {
    if (!world.RegisterComponentPool<PlayerStateComp>(16U).has_value() ||
        !world.RegisterComponentPool<WeaponAttackComp>(256U).has_value() ||
        !world.RegisterComponentPool<ResonanceMarkComp>(256U).has_value() ||
        !world.RegisterComponentPool<BreakAndCcComp>(256U).has_value() ||
        !world.RegisterComponentPool<EnemyAiComp>(256U).has_value() ||
        !world.RegisterComponentPool<ProjectileComp>(256U).has_value() ||
        !world.RegisterComponentPool<InteractableComp>(128U).has_value() ||
        !world.RegisterComponentPool<DestructibleTerrainComp>(64U).has_value()) {
        return en::Err(en::FrameworkErrc::WorldInitFailed);
    }
    return {};
}

en::Result<void, en::FrameworkErrc> VeilGameApp::OnBootHook(
    en::SceneLoadContext& ctx) noexcept {
    auto* self = static_cast<VeilGameApp*>(ctx.userAppState);
    if (!self) return en::Err(en::FrameworkErrc::ClientBootFailed);

    auto assetsRes = GameAssetPipeline::BuildAndCommitPersistentAssets(
        *ctx.assetBank, *ctx.audio, ctx.persistentScope);
    if (!assetsRes.has_value()) {
        return en::Err(assetsRes.error());
    }
    self->m_assets = *assetsRes;

    ctx.gameplay->SetWorldFlag(static_cast<u16>(WorldFlagId::AwakenedIgnisLantern), true);
    ctx.gameplay->SetWorldFlag(static_cast<u16>(WorldFlagId::ResonatorAshSumpActive), true);

    if (auto r = ctx.director->PushState(GameScenes::MakeGameplayLayerDesc(self)); !r) {
        return en::Err(r.error());
    }

    return GameScenes::RequestRoomTransition(*self, 0U, en::SceneTransitionMode::FullReset);
}

void VeilGameApp::OnPreShutdownHook(en::SceneLoadContext&) noexcept {}

void VeilGameApp::OnConsumeSimCueHook(
    const en::SimCue&,
    en::SimContext&,
    void* userAppState) noexcept {
    if (auto* self = static_cast<VeilGameApp*>(userAppState)) {
        ++self->m_cuesConsumedCount;
    }
}

en::Result<void, en::FrameworkError> VeilGameApp::Init(
    bool headless,
    en::ViewportExtent viewport) noexcept {
    Shutdown();

    en::ApplicationConfig cfg{};
    cfg.viewport = viewport;
    cfg.headless = headless ? 1U : 0U;
    cfg.enableDebugTools = 1U;
    cfg.defaultPixelsPerMeter = 48.0f;

    en::AppHooks hooks{};
    hooks.userAppState = this;
    hooks.gameplayPolicies.onCombatHit = &CombatSystem::OnEngineCombatHitCallback;
    hooks.onRegisterComponents = &OnRegisterComponentsHook;
    hooks.onBoot = &OnBootHook;
    hooks.onPreShutdown = &OnPreShutdownHook;
    hooks.onConsumeSimCue = &OnConsumeSimCueHook;

    if (auto r = m_app.Init(cfg, hooks); !r.has_value()) {
        return r;
    }

    PlayerController::ConfigureDefaultInputMap(m_app.Input().Map());
    GameDebugTools::RegisterCVarsAndCommands(m_app.Debug(), this);
    return {};
}

void VeilGameApp::Shutdown() noexcept {
    m_app.Shutdown();
    m_playerEntity = kNullEntity;
    m_cameraEntity = kNullEntity;
    m_bossEntity = kNullEntity;
}

void VeilGameApp::OnSimTick(en::SimContext& ctx, const en::SimStepView& step) noexcept {
    if (!ctx.world || !ctx.gameplay || !ctx.physics || !ctx.tickInput || !ctx.actionBuffer) return;

    if (m_portalCooldownTicks > 0U) {
        --m_portalCooldownTicks;
    }

    // 1. Step mark expirations (420t), Chrono-Overdrive delayed strikes, Break State/Recovery, and CC timers
    CombatSystem::StepCombatTimersAndDelayedStrikes(*ctx.world, *ctx.gameplay);

    // 2. Apply debug CVars if enabled
    if (m_cvars.infiniteHarmony && ctx.world->IsAlive(m_playerEntity)) {
        if (auto* ps = ctx.world->TryGet<PlayerStateComp>(m_playerEntity)) {
            ps->harmony = kMaxHarmonyGauge;
        }
    }
    if (m_cvars.godMode && ctx.world->IsAlive(m_playerEntity)) {
        if (auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity)) {
            recv->hp = recv->maxHp;
        }
    }

    // 3. Step Kaelen controls & attacks
    PlayerController::StepPlayer(
        m_playerEntity,
        *ctx.world,
        *ctx.physics,
        *ctx.gameplay,
        *ctx.tickInput,
        *ctx.actionBuffer,
        m_nextAttackInstanceId);

    // 4. Step Enemies, Boss Phases, and Projectiles (despawns dead enemies and records cleared slots)
    EnemyAiSystem::StepEnemiesAndProjectiles(m_playerEntity, *ctx.world, *ctx.gameplay, &m_metroidvania);

    // 5. Step ambient projectile velocities, sprite frame animations, and transient lifetimes
    ctx.world->StepAmbientVelocities(step);
    ctx.world->StepAnimationsAndLifetimes(step);

    // 6. Check Kaelen death -> spawn / update single Lost Wick Husk, reset room enemies, and snap camera
    if (ctx.world->IsAlive(m_playerEntity)) {
        if (const auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity)) {
            if (recv->HasFlag(en::ReceiverFlags::Dead) || recv->hp <= 0) {
                const en::WorldTransform2D& xf = ctx.world->GetWorldByIndex(m_playerEntity.index);
                m_metroidvania.HandlePlayerDeathAndSpawnHusk(
                    m_playerEntity, Vec2{xf.x, xf.y}, m_activeRoomId, *ctx.world);

                if (auto* camCtrl = ctx.world->TryGet<en::CameraController2D>(m_cameraEntity)) {
                    camCtrl->primaryTarget = m_playerEntity;
                    camCtrl->flags |= en::CameraCtrlFlags::SnapNextStep;
                }

                if (m_metroidvania.LastCheckpointRoom() != m_activeRoomId &&
                    !ctx.director->HasPendingWork()) {
                    (void)GameScenes::RequestRoomTransition(
                        *this, m_metroidvania.LastCheckpointRoom(), en::SceneTransitionMode::RoomSwap);
                } else {
                    GameScenes::SpawnRoomEnemiesIfNeeded(*this, *ctx.world, m_activeRoomId);
                }
            }
        }
    }

    // 7. Automatic Room Exit Portal proximity transition when walking into Left/Right portals
    if (m_portalCooldownTicks == 0U &&
        !ctx.director->HasPendingWork() &&
        ctx.world->IsAlive(m_playerEntity)) {
        const en::WorldTransform2D& pxf = ctx.world->GetWorldByIndex(m_playerEntity.index);
        for (auto [propId, inter] : ctx.world->View<InteractableComp>()) {
            if (inter.kind != InteractableKind::RoomExitPortal) continue;
            const en::WorldTransform2D& ixf = ctx.world->GetWorldByIndex(propId.index);
            if (std::abs(ixf.x - pxf.x) <= 0.75f && std::abs(ixf.y - pxf.y) <= 1.4f) {
                m_portalCooldownTicks = 40U;
                (void)GameScenes::RequestRoomTransition(*this, inter.paramByte, en::SceneTransitionMode::RoomSwap);
                break;
            }
        }
    }

    // 8. Check modal UI triggers (Pause, Map, PlaceMapPin, World Interactables)
    if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Pause), 6U)) {
        (void)ctx.director->PushState(GameScenes::MakePauseModalLayerDesc(this));
    } else if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::ToggleMap), 6U)) {
        (void)ctx.director->PushState(GameScenes::MakeMapModalLayerDesc(this));
    } else if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::PlaceMapPin), 6U)) {
        if (ctx.world->IsAlive(m_playerEntity)) {
            const en::WorldTransform2D& xf = ctx.world->GetWorldByIndex(m_playerEntity.index);
            (void)m_metroidvania.PlaceMapPin(m_activeRoomId, Vec2{xf.x, xf.y}, MapPinKind::FrequencyGate, 1U);
        }
    } else if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Interact), 6U)) {
        if (ctx.world->IsAlive(m_playerEntity)) {
            const en::WorldTransform2D& pxf = ctx.world->GetWorldByIndex(m_playerEntity.index);
            for (auto [propId, inter] : ctx.world->View<InteractableComp>()) {
                const en::WorldTransform2D& ixf = ctx.world->GetWorldByIndex(propId.index);
                const f32 dx = ixf.x - pxf.x;
                const f32 dy = ixf.y - pxf.y;
                if (dx * dx + dy * dy > inter.interactRadius * inter.interactRadius) continue;

                switch (inter.kind) {
                    case InteractableKind::TuningResonator:
                        m_metroidvania.ActivateTuningResonator(
                            m_playerEntity, m_activeRoomId, Vec2{ixf.x, ixf.y}, *ctx.world, *ctx.gameplay);
                        GameScenes::SpawnRoomEnemiesIfNeeded(*this, *ctx.world, m_activeRoomId);
                        break;
                    case InteractableKind::LostWickHusk:
                        if (m_metroidvania.RecoverLostWickHusk(m_playerEntity, *ctx.world, *ctx.gameplay)) {
                            (void)ctx.world->DeferDestroy(propId);
                        }
                        break;
                    case InteractableKind::EchoAltar:
                        m_metroidvania.UnlockEchoAtAltar(
                            m_playerEntity, static_cast<EchoId>(inter.paramByte), *ctx.world);
                        inter.activated = 1U;
                        break;
                    case InteractableKind::MemoryTuningFork:
                        m_metroidvania.CompleteMemoryTuningFork(inter.paramByte, *ctx.gameplay);
                        inter.activated = 1U;
                        break;
                    case InteractableKind::LoreTablet:
                        if (inter.grantedWorldFlag != 0U) {
                            ctx.gameplay->SetWorldFlag(inter.grantedWorldFlag, true);
                        }
                        inter.activated = 1U;
                        break;
                    case InteractableKind::FrequencyGate: {
                        const FrequencyGateDiagnostic diag =
                            MetroidvaniaSystem::EvaluateFrequencyGate(inter, *ctx.gameplay);
                        if (diag.isUnlocked) {
                            if (inter.grantedWorldFlag != 0U) {
                                ctx.gameplay->SetWorldFlag(inter.grantedWorldFlag, true);
                            }
                            inter.activated = 1U;
                        }
                        break;
                    }
                    case InteractableKind::SpireDecisionCore:
                        if (ctx.gameplay->TestWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated))) {
                            (void)m_metroidvania.ResolveSpireEnding(
                                m_playerEntity, false, true, *ctx.world, *ctx.gameplay);
                            (void)ctx.director->PushState(GameScenes::MakeEndingModalLayerDesc(this));
                        }
                        break;
                    case InteractableKind::RoomExitPortal:
                        m_portalCooldownTicks = 40U;
                        (void)GameScenes::RequestRoomTransition(*this, inter.paramByte, en::SceneTransitionMode::RoomSwap);
                        break;
                }
                break;
            }
        }
    }

    GameDebugTools::RecordFrameWatchPlots(m_app.Debug(), *this);
}

void VeilGameApp::OnRenderExtract(en::RenderContext& ctx) noexcept {
    if (!ctx.renderList || !ctx.world) return;
    HudAndMenuSystem::EmitGameplayHudAndWorldIndicators(
        m_playerEntity, *ctx.world, &m_app.Physics(), m_metroidvania, m_activeRoomId, *ctx.renderList);
}

} // namespace vots

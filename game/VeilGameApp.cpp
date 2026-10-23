#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/camera/RoomCameraController.h"
#include "game/combat/CombatSystem.h"
#include "game/player/PlayerController.h"
#include "game/scenes/GameScenes.h"
#include "game/ui/HudAndMenuSystem.h"
#include "game/world/WorldTopology.h"

#include <algorithm>
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
    const en::SimCue& cue,
    en::SimContext& ctx,
    void* userAppState) noexcept {
    if (auto* self = static_cast<VeilGameApp*>(userAppState)) {
        ++self->m_cuesConsumedCount;
        if (cue.kind == en::SimCueKind::CameraTraumaPulse && ctx.world != nullptr) {
            RoomCameraController::TriggerCombatImpulse(
                self->m_cameraEntity, Vec2{0.0f, 2.2f}, cue.Magnitude(), *ctx.world);
        }
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
    cfg.defaultPixelsPerMeter = kCameraPixelsPerMeter;

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

    CombatSystem::StepCombatTimersAndDelayedStrikes(*ctx.world, *ctx.gameplay);

    if (ctx.world->IsAlive(m_playerEntity)) {
        if (auto* ps = ctx.world->TryGet<PlayerStateComp>(m_playerEntity)) {
            if (m_cvars.infiniteHarmony) {
                ps->harmony = kMaxHarmonyGauge;
            }
            if ((ps->doubleJumpEnabled != 0U) != m_cvars.doubleJump) {
                PlayerController::SetDoubleJumpEnabled(m_playerEntity, m_cvars.doubleJump, *ctx.world);
            }
            const u8 targetCharges = static_cast<u8>(std::clamp(m_cvars.maxDashCharges, 1, 2));
            if (ps->MaxDashCharges() != targetCharges) {
                PlayerController::SetMaxDashCharges(m_playerEntity, targetCharges, *ctx.world);
            }
        }
        if (m_cvars.godMode) {
            if (auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity)) {
                recv->hp = recv->maxHp;
            }
        }
    }

    PlayerController::StepPlayer(
        m_playerEntity,
        *ctx.world,
        *ctx.physics,
        *ctx.gameplay,
        *ctx.tickInput,
        *ctx.actionBuffer,
        m_nextAttackInstanceId);

    EnemyAiSystem::StepEnemiesAndProjectiles(m_playerEntity, *ctx.world, *ctx.gameplay, &m_metroidvania);

    ctx.world->StepAmbientVelocities(step);
    ctx.world->StepAnimationsAndLifetimes(step);

    RoomCameraController::StepCameraFraming(m_cameraEntity, m_playerEntity, m_bossEntity, m_activeRoomId, *ctx.world);

    if (ctx.world->IsAlive(m_playerEntity)) {
        if (const auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity)) {
            if (recv->HasFlag(en::ReceiverFlags::Dead) || recv->hp <= 0) {
                const en::WorldTransform2D& xf = ctx.world->GetWorldByIndex(m_playerEntity.index);
                m_metroidvania.HandlePlayerDeathAndSpawnHusk(
                    m_playerEntity, Vec2{xf.x, xf.y}, m_activeRoomId, *ctx.world);

                RoomCameraController::SnapToTarget(
                    m_cameraEntity, m_playerEntity, m_metroidvania.LastCheckpointRoom(), *ctx.world);

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

    // Hollow Knight-style Off-Edge Room Boundary Transition
    if (m_portalCooldownTicks == 0U &&
        !ctx.director->HasPendingWork() &&
        ctx.world->IsAlive(m_playerEntity)) {
        const en::WorldTransform2D& pxf = ctx.world->GetWorldByIndex(m_playerEntity.index);
        const u8 nextRoom = WorldTopology::CheckSeamlessBoundaryExit(
            m_activeRoomId, Vec2{pxf.x, pxf.y}, *ctx.gameplay);
        if (nextRoom < kRoomCount) {
            m_portalCooldownTicks = 40U;
            (void)GameScenes::RequestRoomTransition(*this, nextRoom, en::SceneTransitionMode::RoomSwap);
        }
    }

    if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Pause), 6U)) {
        (void)ctx.director->PushState(GameScenes::MakePauseModalLayerDesc(this));
    } else if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::ToggleMap), 6U)) {
        (void)ctx.director->PushState(GameScenes::MakeMapModalLayerDesc(this));
    } else if (ctx.actionBuffer->Consume(static_cast<u8>(GameAction::PlaceMapPin), 6U)) {
        if (ctx.world->IsAlive(m_playerEntity)) {
            const en::WorldTransform2D& xf = ctx.world->GetWorldByIndex(m_playerEntity.index);
            (void)m_metroidvania.PlaceMapPin(m_activeRoomId, Vec2{xf.x, xf.y}, MapPinKind::FrequencyGate, 1U);
        }
    } else if (ctx.tickInput->Pressed(static_cast<u8>(GameAction::Interact)) ||
               ctx.actionBuffer->Consume(static_cast<u8>(GameAction::Interact), 6U)) {
        if (ctx.world->IsAlive(m_playerEntity)) {
            const auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity);
            const auto* ps = ctx.world->TryGet<PlayerStateComp>(m_playerEntity);
            const bool canInteract = (recv == nullptr || !recv->HasFlag(en::ReceiverFlags::Dead)) &&
                                     (ps == nullptr || ps->unisonState == UnisonActionState::None);
            if (canInteract) {
                const en::WorldTransform2D& pxf = ctx.world->GetWorldByIndex(m_playerEntity.index);

                EntityId bestPropId = kNullEntity;
                InteractableComp* bestInter = nullptr;
                Vec2 bestPos{0.0f, 0.0f};
                f32 bestDistSq = 1.0e9f;
                u8 bestPriority = 0xFFU;

                for (auto [propId, inter] : ctx.world->View<InteractableComp>()) {
                    const en::WorldTransform2D& ixf = ctx.world->GetWorldByIndex(propId.index);
                    const f32 dx = ixf.x - pxf.x;
                    const f32 dy = ixf.y - pxf.y;
                    const f32 dSq = dx * dx + dy * dy;
                    if (dSq > inter.interactRadius * inter.interactRadius) continue;

                    const bool closer = (dSq + 1.0e-4f < bestDistSq);
                    const bool tiedDist = (std::abs(dSq - bestDistSq) <= 1.0e-4f);
                    const bool betterPrio = tiedDist && (inter.interactPriority < bestPriority);
                    const bool betterId = tiedDist && (inter.interactPriority == bestPriority) &&
                                          (propId.index < bestPropId.index);
                    if (!bestPropId.IsValid() || closer || betterPrio || betterId) {
                        bestPropId = propId;
                        bestInter = &inter;
                        bestPos = Vec2{ixf.x, ixf.y};
                        bestDistSq = dSq;
                        bestPriority = inter.interactPriority;
                    }
                }

                if (bestPropId.IsValid() && bestInter != nullptr) {
                    switch (bestInter->kind) {
                        case InteractableKind::TuningResonator:
                            m_metroidvania.ActivateTuningResonator(
                                m_playerEntity, m_activeRoomId, bestPos, *ctx.world, *ctx.gameplay);
                            GameScenes::SpawnRoomEnemiesIfNeeded(*this, *ctx.world, m_activeRoomId);
                            break;
                        case InteractableKind::LostWickHusk:
                            if (m_metroidvania.RecoverLostWickHusk(m_playerEntity, *ctx.world, *ctx.gameplay)) {
                                (void)ctx.world->DeferDestroy(bestPropId);
                            }
                            break;
                        case InteractableKind::EchoAltar:
                            m_metroidvania.UnlockEchoAtAltar(
                                m_playerEntity, static_cast<EchoId>(bestInter->paramByte), *ctx.world);
                            if (bestInter->grantedWorldFlag != 0U) {
                                ctx.gameplay->SetWorldFlag(bestInter->grantedWorldFlag, true);
                            }
                            bestInter->activated = 1U;
                            break;
                        case InteractableKind::MemoryTuningFork:
                            m_metroidvania.CompleteMemoryTuningFork(bestInter->paramByte, *ctx.gameplay);
                            if (bestInter->grantedWorldFlag != 0U) {
                                ctx.gameplay->SetWorldFlag(bestInter->grantedWorldFlag, true);
                            }
                            bestInter->activated = 1U;
                            break;
                        case InteractableKind::LoreTablet:
                            if (bestInter->grantedWorldFlag != 0U) {
                                ctx.gameplay->SetWorldFlag(bestInter->grantedWorldFlag, true);
                            }
                            bestInter->activated = 1U;
                            break;
                        case InteractableKind::FrequencyGate: {
                            const FrequencyGateDiagnostic diag =
                                MetroidvaniaSystem::EvaluateFrequencyGate(*bestInter, *ctx.gameplay);
                            if (diag.isUnlocked) {
                                if (bestInter->grantedWorldFlag != 0U) {
                                    ctx.gameplay->SetWorldFlag(bestInter->grantedWorldFlag, true);
                                }
                                bestInter->activated = 1U;
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
                        case InteractableKind::ShortcutLever:
                            if (bestInter->grantedWorldFlag != 0U) {
                                ctx.gameplay->SetWorldFlag(bestInter->grantedWorldFlag, true);
                            }
                            bestInter->activated = 1U;
                            m_portalCooldownTicks = 40U;
                            (void)GameScenes::RequestRoomTransition(*this, bestInter->paramByte, en::SceneTransitionMode::RoomSwap);
                            break;
                    }
                }
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

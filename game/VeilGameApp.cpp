#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/player/PlayerController.h"
#include "game/scenes/GameScenes.h"
#include "game/ui/HudAndMenuSystem.h"

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

void VeilGameApp::OnSimTick(en::SimContext& ctx, const en::SimStepView&) noexcept {
    if (!ctx.world || !ctx.gameplay || !ctx.physics || !ctx.tickInput || !ctx.actionBuffer) return;

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

    // 4. Step Enemies, Boss Phases, and Projectiles
    EnemyAiSystem::StepEnemiesAndProjectiles(m_playerEntity, *ctx.world, *ctx.gameplay);

    // 5. Check Kaelen death -> spawn / update single Lost Wick Husk & respawn at Tuning Resonator
    if (ctx.world->IsAlive(m_playerEntity)) {
        if (const auto* recv = ctx.world->TryGet<en::DamageReceiver2D>(m_playerEntity)) {
            if (recv->HasFlag(en::ReceiverFlags::Dead) || recv->hp <= 0) {
                const en::WorldTransform2D& xf = ctx.world->GetWorldByIndex(m_playerEntity.index);
                m_metroidvania.HandlePlayerDeathAndSpawnHusk(
                    m_playerEntity, Vec2{xf.x, xf.y}, m_activeRoomId, *ctx.world);
            }
        }
    }

    // 6. Check modal UI triggers (Pause, Map, PlaceMapPin, World Interactables)
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
                        break;
                    case InteractableKind::LostWickHusk:
                        (void)m_metroidvania.RecoverLostWickHusk(m_playerEntity, *ctx.world, *ctx.gameplay);
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
        m_playerEntity, *ctx.world, m_metroidvania, *ctx.renderList);
}

} // namespace vots

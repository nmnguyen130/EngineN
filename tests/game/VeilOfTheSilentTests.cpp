#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/camera/RoomCameraController.h"
#include "game/combat/CombatSystem.h"
#include "game/core/SaveSystem.h"
#include "game/player/PlayerController.h"
#include "game/scenes/GameScenes.h"
#include "game/world/WorldTopology.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace vots::tests {

struct TestSuite {
    u32 passed{0U};
    u32 failed{0U};

    void Expect(bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] Line %d: %s\n", line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, expr) (suite).Expect(static_cast<bool>(expr), #expr, __LINE__)
#define EXPECT_EQ(suite, a, b) (suite).Expect((a) == (b), #a " == " #b, __LINE__)
#define EXPECT_NEAR(suite, a, b, eps) (suite).Expect(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __LINE__)

// 1. Verify GDD v0.3/v0.5 Shared Weapon Frame Data, Recovery Cancel Window, and Attack Instance Echo Capture Invariance
void TestSingleWeaponMovesetAndEchoCaptureInvariance(TestSuite& suite) noexcept {
    const auto& l1 = GetWeaponMoveSpec(WeaponMoveKind::Light1);
    const auto& l2 = GetWeaponMoveSpec(WeaponMoveKind::Light2);
    const auto& l3 = GetWeaponMoveSpec(WeaponMoveKind::Light3);
    EXPECT_EQ(suite, l1.startupTicks, 6U);
    EXPECT_EQ(suite, l1.activeTicks, 6U);
    EXPECT_EQ(suite, l1.recoveryTicks, 14U);
    EXPECT_EQ(suite, l2.startupTicks, 7U);
    EXPECT_EQ(suite, l2.activeTicks, 6U);
    EXPECT_EQ(suite, l2.recoveryTicks, 15U);
    EXPECT_EQ(suite, l3.startupTicks, 9U);
    EXPECT_EQ(suite, l3.activeTicks, 8U);
    EXPECT_EQ(suite, l3.recoveryTicks, 19U);
    EXPECT_EQ(suite, l1.hpDamage + l2.hpDamage + l3.hpDamage, 40);
    EXPECT_EQ(suite, l1.breakDamage + l2.breakDamage + l3.breakDamage, 18U);

    const auto& a1 = GetWeaponMoveSpec(WeaponMoveKind::Air1);
    const auto& a2 = GetWeaponMoveSpec(WeaponMoveKind::Air2);
    EXPECT_EQ(suite, a1.startupTicks, 6U);
    EXPECT_EQ(suite, a1.activeTicks, 6U);
    EXPECT_EQ(suite, a1.recoveryTicks, 12U);
    EXPECT_EQ(suite, a2.startupTicks, 8U);
    EXPECT_EQ(suite, a2.activeTicks, 6U);
    EXPECT_EQ(suite, a2.recoveryTicks, 16U);
    EXPECT_EQ(suite, a1.hpDamage + a2.hpDamage, 25);
    EXPECT_EQ(suite, a1.breakDamage + a2.breakDamage, 11U);

    const auto& hv = GetWeaponMoveSpec(WeaponMoveKind::Heavy);
    EXPECT_EQ(suite, hv.startupTicks, 24U);
    EXPECT_EQ(suite, hv.activeTicks, 8U);
    EXPECT_EQ(suite, hv.recoveryTicks, 20U);
    EXPECT_EQ(suite, hv.hpDamage, 24);
    EXPECT_EQ(suite, hv.breakDamage, 18U);

    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    const EntityId pid = app.PlayerEntity();
    EXPECT_TRUE(suite, world.IsAlive(pid));

    PlayerController::SwitchActiveEchoSlot(pid, 0U, world);
    PlayerController::StartWeaponAttack(
        pid, WeaponMoveKind::Heavy, world, app.EngineApp().Gameplay(), app.NextAttackInstanceId());

    auto* atk = world.TryGet<WeaponAttackComp>(pid);
    EXPECT_TRUE(suite, atk != nullptr);
    EXPECT_EQ(suite, atk->capturedEchoId, EchoId::Ignis);

    PlayerController::SwitchActiveEchoSlot(pid, 1U, world);
    EXPECT_EQ(suite, world.TryGet<PlayerStateComp>(pid)->deck.ActiveEcho(), EchoId::Glacies);
    EXPECT_EQ(suite, atk->capturedEchoId, EchoId::Ignis);

    atk->elapsedTicks = 10U; // Heavy Startup
    EXPECT_TRUE(suite, !PlayerController::StartParryStance(pid, world));
    EXPECT_TRUE(suite, !PlayerController::TryStartDash(pid, world));

    atk->elapsedTicks = 28U; // Heavy Active
    EXPECT_TRUE(suite, !PlayerController::StartParryStance(pid, world));
    EXPECT_TRUE(suite, !PlayerController::TryStartDash(pid, world));

    atk->elapsedTicks = 40U; // Heavy Early Recovery (40 + 8 < 52)
    EXPECT_TRUE(suite, !PlayerController::StartParryStance(pid, world));

    atk->elapsedTicks = 44U; // Heavy Final 8 ticks of Recovery (44 + 8 >= 52)
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::None);

    app.Shutdown();
}

// 2. Verify 420-Tick Resonance Mark State Machine, Same-Echo Refresh, Invalid Pair Replace, and Exact 420-Tick Expiry
void TestResonanceMark420TickLifecycleAndExpirationOrder(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();

    const EntityId dummy = EnemyAiSystem::SpawnDrifter(world, app.AssetHandles(), Vec2{2.0f, 1.0f}).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.IsAlive(dummy));
    world.TryGet<en::DamageReceiver2D>(dummy)->hp = 5000;
    world.TryGet<en::DamageReceiver2D>(dummy)->maxHp = 5000;

    WeaponAttackComp hitIgnis{};
    hitIgnis.instanceId = 100U;
    hitIgnis.baseHpDamage = 10;
    hitIgnis.baseBreakDamage = 4U;
    hitIgnis.capturedEchoId = EchoId::Ignis;
    const auto rep1 = CombatSystem::ResolveWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, rep1.reactionTriggered, WaveReactionId::None);

    auto* mark = world.TryGet<ResonanceMarkComp>(dummy);
    EXPECT_TRUE(suite, mark != nullptr && mark->HasActiveMark());
    EXPECT_EQ(suite, mark->activeMark, EchoId::Ignis);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    const auto repDuplicate = CombatSystem::ResolveWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, repDuplicate.primaryHpDamageDealt, 0);

    for (u32 t = 0U; t < 200U; ++t) {
        CombatSystem::StepTimers(world, gp);
    }
    EXPECT_EQ(suite, mark->remainingTicks, 220U);

    hitIgnis.instanceId = 101U;
    const auto repRefresh = CombatSystem::ResolveWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, repRefresh.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Ignis);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    WeaponAttackComp hitLux = hitIgnis;
    hitLux.instanceId = 102U;
    hitLux.capturedEchoId = EchoId::Lux;
    const auto repInvalid = CombatSystem::ResolveWeaponHit(pid, dummy, hitLux, world, gp);
    EXPECT_EQ(suite, repInvalid.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Lux);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    for (u32 t = 0U; t < 420U; ++t) {
        CombatSystem::StepTimers(world, gp);
    }
    EXPECT_TRUE(suite, !mark->HasActiveMark());
    EXPECT_EQ(suite, mark->remainingTicks, 0U);

    WeaponAttackComp hitUmbra = hitIgnis;
    hitUmbra.instanceId = 103U;
    hitUmbra.capturedEchoId = EchoId::Umbra;
    const auto repAfterExpire = CombatSystem::ResolveWeaponHit(pid, dummy, hitUmbra, world, gp);
    EXPECT_EQ(suite, repAfterExpire.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Umbra);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    app.Shutdown();
}

// 3. Verify All 12 Valid Signature Reaction Formulas (10 Named Reactions), Order-Independence, Secondary Non-Marking, and Non-Recursion
void TestAll12SignatureReactionFormulasAndSecondaryRules(TestSuite& suite) noexcept {
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Ignis, EchoId::Glacies)->id, WaveReactionId::MeltShock);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Glacies, EchoId::Ignis)->id, WaveReactionId::MeltShock);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Terra, EchoId::Fulgur)->id, WaveReactionId::PiezoQuake);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Ignis, EchoId::Aqua)->id, WaveReactionId::SteamCavitation);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Fulgur, EchoId::Ventus)->id, WaveReactionId::SuperconductorGale);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Ignis, EchoId::Ventus)->id, WaveReactionId::PyroCyclone);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Glacies, EchoId::Terra)->id, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Glacies, EchoId::Stridor)->id, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Lux, EchoId::Umbra)->id, WaveReactionId::EclipseSingularity);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Gravitas, EchoId::Terra)->id, WaveReactionId::GravitonQuake);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Tempus, EchoId::Stridor)->id, WaveReactionId::ChronoOverdrive);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Aether, EchoId::Ignis)->id, WaveReactionId::SanguineBoil);
    EXPECT_EQ(suite, FindWaveReaction(EchoId::Aether, EchoId::Aqua)->id, WaveReactionId::SanguineBoil);

    EXPECT_TRUE(suite, FindWaveReaction(EchoId::Ignis, EchoId::Terra) == nullptr);
    EXPECT_TRUE(suite, FindWaveReaction(EchoId::Lux, EchoId::Gravitas) == nullptr);
    EXPECT_TRUE(suite, FindWaveReaction(EchoId::Aqua, EchoId::Stridor) == nullptr);

    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);

    const EntityId primary = EnemyAiSystem::SpawnElite(world, app.AssetHandles(), Vec2{20.0f, 1.0f}).value_or(kNullEntity);
    const EntityId sec1 = EnemyAiSystem::SpawnDrifter(world, app.AssetHandles(), Vec2{21.0f, 1.0f}).value_or(kNullEntity);
    const EntityId sec2 = EnemyAiSystem::SpawnDrifter(world, app.AssetHandles(), Vec2{21.5f, 1.0f}).value_or(kNullEntity);
    world.PropagateTransforms();

    world.TryGet<en::DamageReceiver2D>(primary)->hp = 10000;
    world.TryGet<en::DamageReceiver2D>(primary)->maxHp = 10000;
    world.TryGet<en::DamageReceiver2D>(sec1)->hp = 10000;
    world.TryGet<en::DamageReceiver2D>(sec2)->hp = 10000;
    world.TryGet<BreakAndCcComp>(primary)->breakResistance = 0.0f;

    u32 instId = 200U;
    const auto triggerPair = [&](EchoId eA, EchoId eB) noexcept -> CombatResolutionReport {
        WeaponAttackComp atkA{};
        atkA.instanceId = ++instId;
        atkA.baseHpDamage = 10;
        atkA.baseBreakDamage = 4U;
        atkA.capturedEchoId = eA;
        (void)CombatSystem::ResolveWeaponHit(pid, primary, atkA, world, gp);

        WeaponAttackComp atkB = atkA;
        atkB.instanceId = ++instId;
        atkB.capturedEchoId = eB;
        return CombatSystem::ResolveWeaponHit(pid, primary, atkB, world, gp);
    };

    ps->harmony = 0U;
    const auto rMelt = triggerPair(EchoId::Ignis, EchoId::Glacies);
    EXPECT_EQ(suite, rMelt.reactionTriggered, WaveReactionId::MeltShock);
    EXPECT_EQ(suite, rMelt.primaryHpDamageDealt, 34);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->fractureTicks, 720U);
    EXPECT_TRUE(suite, !world.TryGet<ResonanceMarkComp>(primary)->HasActiveMark());
    EXPECT_EQ(suite, ps->harmony, 10U);

    world.TryGet<BreakAndCcComp>(primary)->fractureTicks = 0U;

    const auto rPiezo = triggerPair(EchoId::Terra, EchoId::Fulgur);
    EXPECT_EQ(suite, rPiezo.reactionTriggered, WaveReactionId::PiezoQuake);
    EXPECT_EQ(suite, rPiezo.primaryHpDamageDealt, 30);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->actionLockTicks, 42U);

    const i32 sec1HpBefore = world.TryGet<en::DamageReceiver2D>(sec1)->hp;
    const auto rSteam = triggerPair(EchoId::Ignis, EchoId::Aqua);
    EXPECT_EQ(suite, rSteam.reactionTriggered, WaveReactionId::SteamCavitation);
    EXPECT_EQ(suite, rSteam.primaryHpDamageDealt, 32);
    EXPECT_EQ(suite, rSteam.secondaryTargetsHit, 2U);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(sec1)->hp, sec1HpBefore - 8);
    EXPECT_TRUE(suite, !world.TryGet<ResonanceMarkComp>(sec1)->HasActiveMark());

    const auto rGale = triggerPair(EchoId::Fulgur, EchoId::Ventus);
    EXPECT_EQ(suite, rGale.reactionTriggered, WaveReactionId::SuperconductorGale);
    EXPECT_EQ(suite, rGale.primaryHpDamageDealt, 28);
    EXPECT_EQ(suite, rGale.secondaryTargetsHit, 2U);

    const EntityId normProj = EnemyAiSystem::SpawnHarmonicProjectile(
        world, primary, Vec2{20.5f, 1.0f}, Vec2{-1.0f, 0.0f}, 10, false).value_or(kNullEntity);
    const EntityId protProj = EnemyAiSystem::SpawnHarmonicProjectile(
        world, primary, Vec2{20.5f, 1.2f}, Vec2{-1.0f, 0.0f}, 15, true).value_or(kNullEntity);
    world.PropagateTransforms();

    const auto rPyro = triggerPair(EchoId::Ignis, EchoId::Ventus);
    EXPECT_EQ(suite, rPyro.reactionTriggered, WaveReactionId::PyroCyclone);
    EXPECT_EQ(suite, rPyro.projectilesCleared, 1U);
    EXPECT_EQ(suite, world.TryGet<ProjectileComp>(normProj)->active, 0U);
    EXPECT_EQ(suite, world.TryGet<ProjectileComp>(protProj)->active, 1U);

    world.TryGet<BreakAndCcComp>(primary)->flags |= (BreakAndCcComp::kFlagBreakableGuard | BreakAndCcComp::kFlagGuardActive);
    const auto rTec1 = triggerPair(EchoId::Glacies, EchoId::Terra);
    EXPECT_EQ(suite, rTec1.reactionTriggered, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->guardBrokenTicks, 90U);
    EXPECT_TRUE(suite, !world.TryGet<BreakAndCcComp>(primary)->HasRuleFlag(BreakAndCcComp::kFlagGuardActive));

    const auto rTec2 = triggerPair(EchoId::Stridor, EchoId::Glacies);
    EXPECT_EQ(suite, rTec2.reactionTriggered, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, rTec2.primaryHpDamageDealt, 38);

    const auto rEclipse = triggerPair(EchoId::Lux, EchoId::Umbra);
    EXPECT_EQ(suite, rEclipse.reactionTriggered, WaveReactionId::EclipseSingularity);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(sec1)->pullTicks, 12U);

    const auto rGrav = triggerPair(EchoId::Gravitas, EchoId::Terra);
    EXPECT_EQ(suite, rGrav.reactionTriggered, WaveReactionId::GravitonQuake);
    EXPECT_EQ(suite, rGrav.primaryHpDamageDealt, 32);

    const auto rChrono = triggerPair(EchoId::Tempus, EchoId::Stridor);
    EXPECT_EQ(suite, rChrono.reactionTriggered, WaveReactionId::ChronoOverdrive);
    const i32 hpBeforeDelay = world.TryGet<en::DamageReceiver2D>(primary)->hp;
    const u8 harmonyBeforeDelay = ps->harmony;
    for (u32 t = 0U; t < 12U; ++t) {
        CombatSystem::StepTimers(world, gp);
    }
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(primary)->hp, hpBeforeDelay - 10);
    EXPECT_EQ(suite, ps->harmony, harmonyBeforeDelay);

    const auto rSang1 = triggerPair(EchoId::Aether, EchoId::Ignis);
    EXPECT_EQ(suite, rSang1.reactionTriggered, WaveReactionId::SanguineBoil);
    const auto rSang2 = triggerPair(EchoId::Aqua, EchoId::Aether);
    EXPECT_EQ(suite, rSang2.reactionTriggered, WaveReactionId::SanguineBoil);

    app.Shutdown();
}

// 4. Verify Boss Break State (150t, 1.25x), Break Recovery (240t), Fracture * Break (1.4375x), and Parry Window
void TestBossBreakRecoveryMultiplicativeDamageAndParryWindow(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);

    const EntityId boss = EnemyAiSystem::SpawnWarden(world, app.AssetHandles(), Vec2{30.0f, 1.35f}).value_or(kNullEntity);
    auto* bossBrk = world.TryGet<BreakAndCcComp>(boss);
    auto* bossRecv = world.TryGet<en::DamageReceiver2D>(boss);

    ps->harmony = 0U;
    PlayerController::SwitchActiveEchoSlot(pid, 1U, world); // Slot 1 = Glacies
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    EXPECT_EQ(suite, ps->parryWindowTicks, kParryWindowTicks);
    EXPECT_EQ(suite, ps->capturedParryEcho, EchoId::Glacies);

    PlayerController::SwitchActiveEchoSlot(pid, 0U, world);
    EXPECT_EQ(suite, ps->deck.ActiveEcho(), EchoId::Ignis);
    EXPECT_EQ(suite, ps->capturedParryEcho, EchoId::Glacies);

    const auto parryMatch = CombatSystem::ResolveEnemyHit(
        boss, pid, 28, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, parryMatch.parrySucceeded);
    EXPECT_TRUE(suite, parryMatch.phaseInversionBonus);
    EXPECT_EQ(suite, ps->harmony, 15U);
    EXPECT_EQ(suite, bossBrk->breakMeter, 30U);
    EXPECT_EQ(suite, ps->parryWindowTicks, 0U);

    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    const auto parryNormal = CombatSystem::ResolveEnemyHit(
        boss, pid, 28, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Terra, world, gp);
    EXPECT_TRUE(suite, parryNormal.parrySucceeded);
    EXPECT_TRUE(suite, !parryNormal.phaseInversionBonus);
    EXPECT_EQ(suite, bossBrk->breakMeter, 48U);

    world.TryGet<en::DamageReceiver2D>(pid)->iframeTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    for (u32 t = 0U; t < kParryWindowTicks; ++t) {
        if (ps->parryWindowTicks > 0U && --ps->parryWindowTicks == 0U) {
            world.TryGet<en::DamageReceiver2D>(pid)->RemoveFlags(en::ReceiverFlags::Parrying);
        }
    }
    const i32 kaelenHpBefore = world.TryGet<en::DamageReceiver2D>(pid)->hp;
    const auto parryExpired = CombatSystem::ResolveEnemyHit(
        boss, pid, 20, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, !parryExpired.parrySucceeded);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(pid)->hp, kaelenHpBefore - 20);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(pid)->iframeTicks, 42U);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(pid)->stunTicks, 18U);

    world.TryGet<en::DamageReceiver2D>(pid)->iframeTicks = 0U;
    world.TryGet<en::DamageReceiver2D>(pid)->stunTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    const auto crimsonHit = CombatSystem::ResolveEnemyHit(
        boss, pid, 25, 10U, AttackTelegraphKind::CrimsonOverloadWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, !crimsonHit.parrySucceeded);
    EXPECT_EQ(suite, crimsonHit.primaryHpDamageDealt, 25);

    CombatResolutionReport brkRep{};
    EXPECT_TRUE(suite, CombatSystem::ApplyBreakDamage(boss, 52U, world, *bossBrk, &brkRep));
    EXPECT_TRUE(suite, brkRep.targetEnteredBreakState);
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::BreakState);
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);
    EXPECT_EQ(suite, bossBrk->phaseTicks, 150U);

    bossBrk->fractureTicks = 720U;
    EXPECT_EQ(suite, CombatSystem::ComputeModifiedHpDamage(160, bossBrk), 230);

    EXPECT_TRUE(suite, !CombatSystem::ApplyBreakDamage(boss, 50U, world, *bossBrk));
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);

    for (u32 t = 0U; t < 150U; ++t) {
        CombatSystem::StepTimers(world, gp);
    }
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::BreakRecovery);
    EXPECT_EQ(suite, bossBrk->phaseTicks, 240U);

    EXPECT_TRUE(suite, !CombatSystem::ApplyBreakDamage(boss, 80U, world, *bossBrk));
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);

    for (u32 t = 0U; t < 240U; ++t) {
        CombatSystem::StepTimers(world, gp);
    }
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::Normal);
    (void)CombatSystem::ApplyBreakDamage(boss, 40U, world, *bossBrk);
    EXPECT_EQ(suite, bossBrk->breakMeter, 40U);
    (void)bossRecv;

    app.Shutdown();
}

// 5. Verify Air-Dash Cap, Unison/Sacred Chords, Single Lost Wick Husk, Save/Load, and 3 Endings
void TestHarmonySacredChordsLostWickHuskSaveAndEndings(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    MetroidvaniaSystem& mv = app.Metroidvania();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);
    auto* recv = world.TryGet<en::DamageReceiver2D>(pid);
    auto* motor = world.TryGet<en::CharacterMotor2D>(pid);

    motor->contacts = en::ContactFlags::None;
    ps->airDashCharges = 1U;
    ps->dashActiveTicks = 0U;
    ps->dashLockoutTicks = 0U;
    ps->dashCooldownTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::TryStartDash(pid, world));
    EXPECT_EQ(suite, ps->airDashCharges, 0U);
    EXPECT_EQ(suite, ps->dashActiveTicks, kPlayerDashDurationTicks);
    ps->dashActiveTicks = 0U;
    ps->dashLockoutTicks = 0U;
    ps->dashCooldownTicks = 0U;
    EXPECT_TRUE(suite, !PlayerController::TryStartDash(pid, world));

    ps->unlockedEchoMask = 0x0FFFU;
    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Fulgur, EchoId::Ignis, EchoId::Terra, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::ElementalCataclysm);

    ps->harmony = 60U;
    EXPECT_TRUE(suite, CombatSystem::TryActivateUnisonBurst(pid, world, gp));
    EXPECT_EQ(suite, ps->harmony, 10U);

    ps->harmony = 100U;
    CombatResolutionReport chordRep{};
    EXPECT_TRUE(suite, CombatSystem::TryActivateSacredChord(pid, world, gp, &chordRep));
    EXPECT_EQ(suite, chordRep.chordTriggered, SacredChordId::ElementalCataclysm);
    EXPECT_EQ(suite, ps->harmony, 0U);

    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Aqua, EchoId::Glacies, EchoId::Ventus, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::CryoTempestProtocol);

    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Tempus, EchoId::Lux, EchoId::Umbra, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::EclipseOfEternity);

    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Stridor, EchoId::Gravitas, EchoId::Aether, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::SingularityOverdrive);

    mv.ActivateTuningResonator(pid, 0U, Vec2{-2.0f, 1.0f}, world, gp);
    ps->carriedTuningShards = 120U;
    mv.HandlePlayerDeathAndSpawnHusk(pid, Vec2{8.0f, 1.0f}, 0U, world);
    EXPECT_TRUE(suite, mv.ActiveHusk().IsActive());
    EXPECT_NEAR(suite, mv.ActiveHusk().worldX, 8.0f, 1e-4f);
    EXPECT_EQ(suite, mv.ActiveHusk().bankedTuningShards, 120U);
    EXPECT_EQ(suite, ps->carriedTuningShards, 0U);
    EXPECT_EQ(suite, ps->harmony, 0U);
    EXPECT_EQ(suite, recv->maxHp, 75);

    ps->carriedTuningShards = 45U;
    mv.HandlePlayerDeathAndSpawnHusk(pid, Vec2{3.5f, 1.0f}, 0U, world);
    EXPECT_NEAR(suite, mv.ActiveHusk().worldX, 8.0f, 1e-4f);
    EXPECT_EQ(suite, mv.ActiveHusk().bankedTuningShards, 165U);
    EXPECT_EQ(suite, recv->maxHp, 75);

    recv->hp = 60;
    EXPECT_TRUE(suite, mv.RecoverLostWickHusk(pid, world, gp));
    EXPECT_TRUE(suite, !mv.ActiveHusk().IsActive());
    EXPECT_EQ(suite, ps->carriedTuningShards, 165U);
    EXPECT_EQ(suite, ps->harmony, 100U);
    EXPECT_EQ(suite, recv->maxHp, 100);
    EXPECT_EQ(suite, recv->hp, 60);

    EXPECT_TRUE(suite, mv.PlaceMapPin(0U, Vec2{14.0f, 1.0f}, MapPinKind::FrequencyGate, 7U));
    EXPECT_TRUE(suite, mv.PlaceMapPin(1U, Vec2{9.0f, 1.0f}, MapPinKind::LoreTablet, 2U));
    EXPECT_EQ(suite, mv.MapPins().size(), 2U);

    const SaveData exported = mv.ExportSaveData(pid, world, gp);
    std::array<u8, sizeof(SaveData)> saveBuf{};
    EXPECT_TRUE(suite, SaveSystem::SerializeToBuffer(exported, saveBuf).has_value());
    const auto loadedRes = SaveSystem::DeserializeFromBuffer(saveBuf);
    EXPECT_TRUE(suite, loadedRes.has_value());
    EXPECT_EQ(suite, loadedRes->carriedTuningShards, 165U);
    EXPECT_EQ(suite, loadedRes->pinCount, 2U);

    saveBuf[24] ^= 0x01U;
    EXPECT_EQ(suite, SaveSystem::DeserializeFromBuffer(saveBuf).error(), SaveErrc::ChecksumMismatch);

    gp.SetWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated), true);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, false, false, world, gp), EndingId::FalseDawn);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, true, false, world, gp), EndingId::ShatteredSpire);

    mv.CompleteMemoryTuningFork(0U, gp);
    mv.CompleteMemoryTuningFork(1U, gp);
    mv.CompleteMemoryTuningFork(2U, gp);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, false, true, world, gp), EndingId::SymphonyOfOffBeat);
    EXPECT_TRUE(suite, ps->LanternRelinquished());

    app.Shutdown();
}

// 6. Verify Deterministic Input Recording & Playback Across All 12 EngineN Subsystems + VeilGameApp
void TestFullGameReplayDeterminismAndRoomTransitions(TestSuite& suite) noexcept {
    en::InputReplayStream replayStream{};

    const auto runGameSession = [&](en::ReplayMode mode) noexcept -> en::FrameAuditDigest {
        VeilGameApp app{};
        EXPECT_TRUE(suite, app.Init(true).has_value());
        app.EngineApp().BindReplayStream(&replayStream, mode);

        if (mode == en::ReplayMode::Record) {
            app.EngineApp().Input().FeedButton(en::InputSource::Key(7U), true);  // Move Right (D)
            app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), true); // Light Attack (J)
        }
        (void)app.StepHeadlessTicks(15U, 2U);

        if (mode == en::ReplayMode::Record) {
            app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), false);
            app.EngineApp().Input().FeedButton(en::InputSource::Key(6U), true);  // Cycle Echo Next (C)
        }
        (void)app.StepHeadlessTicks(10U, 2U);

        EXPECT_TRUE(suite, GameScenes::RequestRoomTransition(app, 1U, en::SceneTransitionMode::RoomSwap).has_value());
        const auto finalDigest = app.StepHeadlessTicks(15U, 2U);
        EXPECT_TRUE(suite, finalDigest.has_value());

        const en::FrameAuditDigest out = *finalDigest;
        app.Shutdown();
        return out;
    };

    replayStream.Clear();
    const en::FrameAuditDigest dRec = runGameSession(en::ReplayMode::Record);
    const en::FrameAuditDigest dPlay = runGameSession(en::ReplayMode::Playback);

    EXPECT_EQ(suite, dRec.tickIndex, 40ULL);
    EXPECT_EQ(suite, dRec.worldHash, dPlay.worldHash);
    EXPECT_EQ(suite, dRec.physicsHash, dPlay.physicsHash);
    EXPECT_EQ(suite, dRec.gameplayHash, dPlay.gameplayHash);
    EXPECT_EQ(suite, dRec.audioHash, dPlay.audioHash);
    EXPECT_EQ(suite, dRec.renderListHash, dPlay.renderListHash);
    EXPECT_EQ(suite, dRec.directorHash, dPlay.directorHash);
    EXPECT_EQ(suite, dRec.combinedHash, dPlay.combinedHash);
}

// 7. Verify 2D Greedy Tilemap Meshing, Slash Arc VFX Rendering, Enemy Despawn/Respawn Lifecycle, Pass-Through, and True Screen-Edge Room Transitions
void TestVisualTilemapSlashVfxEnemyRespawnAndPassThrough(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    const EntityId pid = app.PlayerEntity();
    EXPECT_TRUE(suite, world.IsAlive(pid));

    const en::Camera2D& cam0 = app.EngineApp().GetRenderList().View(0U);
    EXPECT_TRUE(suite, cam0.yUp);

    // Verify runtime-loaded 768x1056 (8x11 of 96x96 frames) Spritesheet & 128x48 Pixel-Art Font Atlas from disk:
    const en::TextureRecord& sheetRec = app.EngineApp().Assets().GetTexture(app.AssetHandles().characterSheetTex);
    EXPECT_EQ(suite, sheetRec.width, 768U);
    EXPECT_EQ(suite, sheetRec.height, 1056U);
    EXPECT_TRUE(suite, sheetRec.pixels != nullptr);
    const en::TextureRecord& fontRec = app.EngineApp().Assets().GetTexture(app.AssetHandles().pixelFontTex);
    EXPECT_EQ(suite, fontRec.width, 512U);
    EXPECT_EQ(suite, fontRec.height, 192U);
    EXPECT_TRUE(suite, fontRec.pixels != nullptr);
    EXPECT_NEAR(suite, world.TryGet<en::Sprite2D>(pid)->width, kPlayerSpriteSizeWu, 1e-3f);
    EXPECT_NEAR(suite, world.TryGet<en::Sprite2D>(pid)->height, kPlayerSpriteSizeWu, 1e-3f);
    (void)app.StepHeadlessTicks(16U, 1U);
    EXPECT_EQ(suite, world.TryGet<en::SpriteAnimator2D>(pid)->firstFrame, 0U);
    EXPECT_EQ(suite, world.TryGet<en::SpriteAnimator2D>(pid)->frameCount, 6U);

    // 2D Rectangular Greedy Meshing + clean Top-Left HUD & zero redundant player VFX keep total quads super lean
    const u32 baseQuadCount = app.EngineApp().GetRenderList().Stats().submittedQuads;
    EXPECT_TRUE(suite, baseQuadCount > 80U && baseQuadCount < 350U);

    PlayerController::StartWeaponAttack(
        pid, WeaponMoveKind::Light1, world, app.EngineApp().Gameplay(), app.NextAttackInstanceId());
    (void)app.StepHeadlessTicks(8U, 1U);
    EXPECT_EQ(suite, world.TryGet<en::SpriteAnimator2D>(pid)->firstFrame, 32U);
    EXPECT_EQ(suite, world.TryGet<en::SpriteAnimator2D>(pid)->frameCount, 8U);

    EXPECT_EQ(suite, world.TryGet<en::MotionStack2D>(pid)->pushboxGroup, 0U);
    EXPECT_TRUE(suite, !world.Has<en::Collider2D>(pid));

    u32 room0EnemiesInitial = 0U;
    EntityId firstEnemy = kNullEntity;
    for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == 0U) {
            ++room0EnemiesInitial;
            if (ai.spawnSlotBit == 0x01U) firstEnemy = eid;
            // Verify enemies also have pushboxGroup == 0U so they overlap naturally without queuing in a line!
            EXPECT_EQ(suite, world.TryGet<en::MotionStack2D>(eid)->pushboxGroup, 0U);
        }
    }
    EXPECT_EQ(suite, room0EnemiesInitial, 2U);
    EXPECT_TRUE(suite, world.IsAlive(firstEnemy));

    world.TryGet<en::DamageReceiver2D>(firstEnemy)->hp = 0;
    (void)app.StepHeadlessTicks(1U, 1U);
    EXPECT_TRUE(suite, !world.IsAlive(firstEnemy));
    EXPECT_TRUE(suite, app.Metroidvania().IsEnemyCleared(0U, 0x01U));

    EXPECT_TRUE(suite, GameScenes::RequestRoomTransition(app, 1U, en::SceneTransitionMode::RoomSwap).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 1U);

    EXPECT_TRUE(suite, GameScenes::RequestRoomTransition(app, 0U, en::SceneTransitionMode::RoomSwap).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 0U);

    u32 room0EnemiesAfterSwap = 0U;
    for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == 0U && recv.hp > 0) ++room0EnemiesAfterSwap;
    }
    EXPECT_EQ(suite, room0EnemiesAfterSwap, 1U);

    app.Metroidvania().ActivateTuningResonator(pid, 0U, Vec2{-6.0f, 1.0f}, world, app.EngineApp().Gameplay());
    GameScenes::SpawnRoomEnemiesIfNeeded(app, world, 0U);
    u32 room0EnemiesAfterResonator = 0U;
    for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == 0U && recv.hp > 0) ++room0EnemiesAfterResonator;
    }
    EXPECT_EQ(suite, room0EnemiesAfterResonator, 2U);

    // Verify True Screen-Edge Transition:
    // 1) Walking to x = 21.5f (near the inner right wall drop-down, 2.5m before screen edge) MUST NOT teleport!
    app.SetPortalCooldownTicks(0U);
    const auto& r0 = GetRoomTopology(0U);
    en::LocalTransform2D pxf = world.GetLocalTransform(pid);
    pxf.x = 21.5f;
    pxf.y = 1.0f;
    world.Teleport(pid, pxf);
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 0U);

    // 2) Touching the upper right wall (x = 23.8f, y = 7.0f above the doorway) MUST NOT teleport!
    pxf.x = r0.EastExitThresholdX() + 0.1f;
    pxf.y = 7.0f;
    world.Teleport(pid, pxf);
    (void)app.StepHeadlessTicks(2U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 0U);

    // 3) Walking through the open doorway (y = 1.0f) all the way to the outer screen edge (x >= EastExitThresholdX) transitions Room 0 -> Room 1!
    pxf.x = r0.EastExitThresholdX() + 0.1f;
    pxf.y = 1.0f;
    world.Teleport(pid, pxf);
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 1U);

    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().ValidFrames() > 0U);
    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().Latest().aliveEntities > 0U);
    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().Latest().scopeUs[0] > 0U);

    app.Shutdown();
}

// 8. Verify Jump, Double Jump, Extended Dash + Projectile Evasion, 3-Hit J Combo Grace Window, and Boss Phase Transition
void TestGddV03JumpDoubleJumpDashProjectileDodgeComboAndBoss(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(6U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);
    auto* recv = world.TryGet<en::DamageReceiver2D>(pid);
    auto* atk = world.TryGet<WeaponAttackComp>(pid);

    en::InputMap& imap = app.EngineApp().Input().Map();
    for (u8 a = 0U; a < en::kMaxActions; ++a) {
        const u16 otherMask = static_cast<u16>(~(1U << a));
        for (const en::InputSource& src : imap.ActionBindings(a)) {
            if (src.IsValid()) {
                EXPECT_EQ(suite, imap.FindConflictingActions(src, otherMask), 0U);
            }
        }
    }

    const f32 startY = world.GetWorldByIndex(pid.index).y;
    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    f32 singleJumpPeakY = startY;
    for (u32 t = 0U; t < 66U; ++t) {
        (void)app.StepHeadlessTicks(1U, 1U);
        singleJumpPeakY = std::max(singleJumpPeakY, world.GetWorldByIndex(pid.index).y);
    }
    EXPECT_TRUE(suite, singleJumpPeakY > 4.1f);

    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), false);
    (void)app.StepHeadlessTicks(1U, 1U);
    EXPECT_EQ(suite, ps->airJumpsRemaining, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    EXPECT_EQ(suite, ps->airJumpsRemaining, 0U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), false);

    f32 doubleJumpPeakY = singleJumpPeakY;
    for (u32 t = 0U; t < 60U; ++t) {
        (void)app.StepHeadlessTicks(1U, 1U);
        doubleJumpPeakY = std::max(doubleJumpPeakY, world.GetWorldByIndex(pid.index).y);
    }
    EXPECT_TRUE(suite, doubleJumpPeakY > 6.5f);

    PlayerController::SetDoubleJumpEnabled(pid, false, world);
    EXPECT_EQ(suite, ps->doubleJumpEnabled, 0U);
    EXPECT_EQ(suite, ps->airJumpsRemaining, 0U);
    PlayerController::SetDoubleJumpEnabled(pid, true, world);

    (void)app.StepHeadlessTicks(80U, 1U);

    // Verify 20-tick Extended Dash travels > 3.1 wu AND phases cleanly through an incoming HarmonicProjectile!
    const f32 dashStartX = world.GetWorldByIndex(pid.index).x;
    const i32 hpBeforeDash = recv->hp;
    const EntityId incomingBolt = EnemyAiSystem::SpawnHarmonicProjectile(
        world, kNullEntity, Vec2{dashStartX + 1.4f, world.GetWorldByIndex(pid.index).y},
        Vec2{-8.0f, 0.0f}, 15, false).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.IsAlive(incomingBolt));

    app.EngineApp().Input().FeedButton(en::InputSource::Key(225U), true);
    (void)app.StepHeadlessTicks(20U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(225U), false);

    const f32 dashEndX = world.GetWorldByIndex(pid.index).x;
    EXPECT_TRUE(suite, (dashEndX - dashStartX) > 3.1f);
    EXPECT_EQ(suite, recv->hp, hpBeforeDash);
    if (world.IsAlive(incomingBolt)) {
        (void)world.DeferDestroy(incomingBolt);
    }
    (void)app.StepHeadlessTicks(16U, 1U);

    // Verify Relaxed 3-Hit Light Combo (J = Key 13U) via Post-Recovery Combo Grace Window
    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), false);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::Light1);

    (void)app.StepHeadlessTicks(35U, 1U);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::None);
    EXPECT_EQ(suite, atk->BufferedChainMove(), WeaponMoveKind::Light2);

    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), false);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::Light2);

    (void)app.StepHeadlessTicks(37U, 1U);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::None);
    EXPECT_EQ(suite, atk->BufferedChainMove(), WeaponMoveKind::Light3);

    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(13U), false);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::Light3);
    (void)app.StepHeadlessTicks(40U, 1U);

    // Verify Unison Startup (12t) Hitstun Refund before Resolve
    ps->harmony = 70U;
    recv->iframeTicks = 0U;
    recv->stunTicks = 0U;
    EXPECT_TRUE(suite, CombatSystem::BeginUnisonOrSacredChord(pid, UnisonActionState::StandardUnison, world));
    EXPECT_EQ(suite, ps->harmony, 20U);

    for (u32 t = 0U; t < 6U; ++t) {
        CombatSystem::StepActiveUnisonOrChord(pid, world, gp);
    }
    const EntityId drifter = EnemyAiSystem::SpawnDrifter(world, app.AssetHandles(), Vec2{-3.5f, 1.0f}).value_or(kNullEntity);
    (void)CombatSystem::ResolveEnemyHit(
        drifter, pid, 12, 5U, AttackTelegraphKind::GoldHarmonicWave, EchoId::None, world, gp);
    EXPECT_EQ(suite, ps->unisonState, UnisonActionState::None);
    EXPECT_EQ(suite, ps->harmony, 70U);

    // Verify Ash-Sump Warden 2-Phase Boss (1,200 HP/phase, 60-tick damage-locked Phase Transition)
    EXPECT_TRUE(suite, GameScenes::RequestRoomTransition(app, 6U, en::SceneTransitionMode::RoomSwap).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 6U);

    const EntityId warden = app.BossEntity();
    EXPECT_TRUE(suite, world.IsAlive(warden));
    auto* wRecv = world.TryGet<en::DamageReceiver2D>(warden);
    auto* wAi = world.TryGet<EnemyAiComp>(warden);
    auto* wBrk = world.TryGet<BreakAndCcComp>(warden);
    auto* wMark = world.TryGet<ResonanceMarkComp>(warden);
    EXPECT_EQ(suite, wRecv->hp, 1200);
    EXPECT_EQ(suite, wAi->bossPhase, 1U);

    wMark->activeMark = EchoId::Ignis;
    wMark->remainingTicks = 300U;
    wBrk->breakMeter = 65U;
    const EntityId bossProj = EnemyAiSystem::SpawnHarmonicProjectile(
        world, warden, Vec2{4.0f, 1.5f}, Vec2{-8.0f, 0.0f}, 12, true).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.IsAlive(bossProj));

    wRecv->hp = 0;
    (void)app.StepHeadlessTicks(1U, 1U);
    EXPECT_EQ(suite, wAi->bossPhase, 2U);
    EXPECT_EQ(suite, wRecv->hp, 1200);
    EXPECT_EQ(suite, wBrk->breakMeter, 0U);
    EXPECT_TRUE(suite, !wMark->HasActiveMark());
    EXPECT_TRUE(suite, !world.IsAlive(bossProj));
    EXPECT_TRUE(suite, wAi->phaseTransitionTicks > 0U);

    WeaponAttackComp testHit{};
    testHit.instanceId = 999U;
    testHit.baseHpDamage = 24;
    testHit.baseBreakDamage = 18U;
    testHit.capturedEchoId = EchoId::Glacies;
    const auto immuneRep = CombatSystem::ResolveWeaponHit(pid, warden, testHit, world, gp);
    EXPECT_EQ(suite, immuneRep.primaryHpDamageDealt, 0);
    EXPECT_EQ(suite, wRecv->hp, 1200);

    (void)app.StepHeadlessTicks(60U, 1U);
    EXPECT_EQ(suite, wAi->phaseTransitionTicks, 0U);
    testHit.instanceId = 1000U;
    const auto activeRep = CombatSystem::ResolveWeaponHit(pid, warden, testHit, world, gp);
    EXPECT_EQ(suite, activeRep.primaryHpDamageDealt, 24);
    EXPECT_EQ(suite, wRecv->hp, 1176);

    app.Shutdown();
}

// 9. Verify GDD v0.4 & Playtest Upgrades
void TestGddV04HitstunEscapeMultiDashParryInterceptionCinemachineAndWorldBlueprint(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(6U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);
    auto* recv = world.TryGet<en::DamageReceiver2D>(pid);
    auto* atk = world.TryGet<WeaponAttackComp>(pid);

    u32 portalEntityCount = 0U;
    for (auto [propId, inter] : world.View<InteractableComp>()) {
        if (inter.kind == InteractableKind::RoomExitPortal) ++portalEntityCount;
    }
    EXPECT_EQ(suite, portalEntityCount, 0U);

    PlayerController::StartWeaponAttack(pid, WeaponMoveKind::Light1, world, gp, app.NextAttackInstanceId());
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::Light1);
    recv->iframeTicks = 0U;
    (void)CombatSystem::ResolveEnemyHit(
        kNullEntity, pid, 10, 5U, AttackTelegraphKind::GoldHarmonicWave, EchoId::None, world, gp);
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::None);
    EXPECT_EQ(suite, recv->stunTicks, kPlayerHitstunTicks);

    app.EngineApp().Input().FeedButton(en::InputSource::Key(225U), true);
    (void)app.StepHeadlessTicks(1U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(225U), false);
    EXPECT_EQ(suite, recv->stunTicks, 0U);
    EXPECT_TRUE(suite, ps->IsDashing());
    (void)app.StepHeadlessTicks(30U, 1U);

    PlayerController::SetMaxDashCharges(pid, 2U, world);
    EXPECT_EQ(suite, ps->MaxDashCharges(), 2U);
    EXPECT_EQ(suite, ps->airDashCharges, 2U);
    ps->dashCooldownTicks = 0U;
    ps->dashLockoutTicks = 0U;

    const f32 xBeforeChainDash = world.GetWorldByIndex(pid.index).x;
    EXPECT_TRUE(suite, PlayerController::TryStartDash(pid, world, 1.0f));
    EXPECT_EQ(suite, ps->airDashCharges, 1U);
    (void)app.StepHeadlessTicks(26U, 1U);
    EXPECT_TRUE(suite, PlayerController::TryStartDash(pid, world, 1.0f));
    (void)app.StepHeadlessTicks(20U, 1U);
    const f32 xAfterChainDash = world.GetWorldByIndex(pid.index).x;
    EXPECT_TRUE(suite, (xAfterChainDash - xBeforeChainDash) > 6.2f);

    (void)app.StepHeadlessTicks(20U, 1U);
    PlayerController::StartWeaponAttack(pid, WeaponMoveKind::Light1, world, gp, app.NextAttackInstanceId());
    atk->elapsedTicks = 4U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    EXPECT_EQ(suite, atk->moveKind, WeaponMoveKind::None);
    EXPECT_EQ(suite, ps->parryWindowTicks, kParryWindowTicks);

    const en::WorldTransform2D& pxfNow = world.GetWorldByIndex(pid.index);
    const EntityId attacker = EnemyAiSystem::SpawnDrifter(
        world, app.AssetHandles(), Vec2{pxfNow.x + 1.0f, pxfNow.y}, 0U, 0U).value_or(kNullEntity);
    world.PropagateTransforms();
    world.TryGet<en::MotionStack2D>(attacker)->SetFacingSign(-1); // Facing Left toward Kaelen
    auto* attAi = world.TryGet<EnemyAiComp>(attacker);
    auto* attBrk = world.TryGet<BreakAndCcComp>(attacker);
    attAi->activeAttackId = EnemyAttackId::DrifterCloseSwipe;
    attAi->actionTimerTicks = 24U; // Within 12-tick strike interception lead window before windupTicks (30U)
    ps->harmony = 0U;

    EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
    EXPECT_EQ(suite, ps->parryWindowTicks, 0U);
    EXPECT_EQ(suite, ps->harmony, kParryHarmonyGain);
    EXPECT_TRUE(suite, attBrk->breakMeter >= kParryBaseBreakDamage);

    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    const EntityId parryBolt = EnemyAiSystem::SpawnHarmonicProjectile(
        world, attacker, Vec2{pxfNow.x + 1.15f, pxfNow.y}, Vec2{-7.0f, 0.0f}, 10, false).value_or(kNullEntity);
    world.PropagateTransforms();
    EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
    EXPECT_EQ(suite, ps->parryWindowTicks, 0U);
    EXPECT_EQ(suite, ps->harmony, static_cast<u8>(kParryHarmonyGain * 2U));
    EXPECT_EQ(suite, world.TryGet<ProjectileComp>(parryBolt)->active, 0U);

    const EntityId camId = app.CameraEntity();
    EXPECT_TRUE(suite, world.IsAlive(camId));
    const auto* camCtrl = world.TryGet<en::CameraController2D>(camId);
    EXPECT_TRUE(suite, camCtrl != nullptr);
    EXPECT_TRUE(suite, camCtrl->anchorX >= kRoomCamCenterMinX - 1e-3f && camCtrl->anchorX <= kRoomCamCenterMaxX + 1e-3f);
    EXPECT_TRUE(suite, camCtrl->anchorY >= kRoomCamCenterMinY && camCtrl->anchorY <= kRoomCamCenterMaxY);

    gp.SetWorldFlag(static_cast<u16>(WorldFlagId::GateSpireOpened), false);
    EXPECT_EQ(suite, WorldTopology::CountCollectedConductorSeals(gp), 0U);
    EXPECT_TRUE(suite, !WorldTopology::IsSilentReliquaryUnlocked(gp));

    WorldTopology::GrantMidgameBranchSeal(RegionId::R4_GravityFoundry, gp);
    EXPECT_EQ(suite, WorldTopology::CountCollectedConductorSeals(gp), 1U);
    WorldTopology::GrantMidgameBranchSeal(RegionId::R2_Choirworks, gp);
    EXPECT_EQ(suite, WorldTopology::CountCollectedConductorSeals(gp), 2U);
    EXPECT_TRUE(suite, !WorldTopology::IsSilentReliquaryUnlocked(gp));
    WorldTopology::GrantMidgameBranchSeal(RegionId::R3_DrownedArchive, gp);
    EXPECT_EQ(suite, WorldTopology::CountCollectedConductorSeals(gp), 3U);
    EXPECT_TRUE(suite, WorldTopology::IsSilentReliquaryUnlocked(gp));

    const TriadDeck deckBeforeUnlock = ps->deck;
    app.Metroidvania().UnlockEchoAtAltar(pid, EchoId::Stridor, world);
    EXPECT_TRUE(suite, ps->deck == deckBeforeUnlock);
    EXPECT_TRUE(suite, (ps->unlockedEchoMask & (1U << static_cast<u8>(EchoId::Stridor))) != 0U);

    EXPECT_TRUE(suite, !WorldTopology::GetMainPathObjective(app.ActiveRoomId(), ps->unlockedEchoMask, gp).empty());

    app.Shutdown();
}

// 10. Verify GDD v0.5 Final Refactor:
//     - Wall-Sticking Fix (Wall Slide & Wall Jump)
//     - AI Vertical Elevation Leash, Facing Lock on Windup, Dash-Behind Evasion, Turn-Around Delay, and Dual Attack-Token Coordinator
//     - Tactile Combat Recoil & Knockback Arc
void TestGddV05WallSlideElevationAiCombatRecoilAndVariableRooms(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(6U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* recv = world.TryGet<en::DamageReceiver2D>(pid);
    auto* motor = world.TryGet<en::CharacterMotor2D>(pid);

    // 1. Verify Variable Room Dimensions across GDD v0.5 Room Graph:
    const auto& r0 = GetRoomTopology(0U); // R0-01 Hub/Onboarding (36x12)
    const auto& r2 = GetRoomTopology(2U); // R1-03 Glass Conservatory Gallery (50x13)
    const auto& r5 = GetRoomTopology(5U); // R4-01 Vertical Shaft (32x22)
    EXPECT_EQ(suite, r0.tileWidth, 36U);
    EXPECT_EQ(suite, r0.tileHeight, 12U);
    EXPECT_EQ(suite, r2.tileWidth, 50U);
    EXPECT_EQ(suite, r2.tileHeight, 13U);
    EXPECT_EQ(suite, r5.tileWidth, 32U);
    EXPECT_EQ(suite, r5.tileHeight, 22U);

    // 2. Verify Wall-Sticking Fix + Smooth Wall Slide & Wall Jump when holding Left (A) against stacked left wall tiles:
    en::LocalTransform2D wallTestXf = world.GetLocalTransform(pid);
    wallTestXf.x = r0.MinWorldX() + 2.0f + (kPlayerColliderWidthWu * 0.5f) + 0.01f;
    wallTestXf.y = 5.5f;
    world.Teleport(pid, wallTestXf);
    motor->vx = -4.0f;
    motor->vy = 0.0f;
    motor->contacts = en::ContactFlags::None;

    app.EngineApp().Input().FeedButton(en::InputSource::Key(4U), true); // Hold A (Move Left into wall)
    (void)app.StepHeadlessTicks(18U, 1U);

    const f32 yAfterWallSlide = world.GetWorldByIndex(pid.index).y;
    EXPECT_TRUE(suite, yAfterWallSlide < 5.35f);
    EXPECT_NEAR(suite, motor->vy, -kPlayerWallSlideSpeedWu, 0.25f);

    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), true);
    (void)app.StepHeadlessTicks(2U, 1U);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(44U), false);
    app.EngineApp().Input().FeedButton(en::InputSource::Key(4U), false);
    EXPECT_TRUE(suite, motor->vy > 9.0f);
    EXPECT_TRUE(suite, motor->vx > 4.5f);
    EXPECT_TRUE(suite, world.GetWorldByIndex(pid.index).y > yAfterWallSlide);

    // 3. Verify AI Vertical Elevation Leash:
    en::LocalTransform2D highPlatXf = world.GetLocalTransform(pid);
    highPlatXf.x = 0.0f;
    highPlatXf.y = 3.8f;
    world.Teleport(pid, highPlatXf);
    world.PropagateTransforms();

    const EntityId groundDrifter = EnemyAiSystem::SpawnDrifter(
        world, app.AssetHandles(), Vec2{0.4f, 1.0f}, 0U, 0U).value_or(kNullEntity);
    world.PropagateTransforms();
    world.TryGet<EnemyAiComp>(groundDrifter)->cooldownTicks = 0U;
    world.TryGet<EnemyAiComp>(groundDrifter)->actionTimerTicks = 0U;

    for (u32 t = 0U; t < 20U; ++t) {
        EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
    }
    EXPECT_EQ(suite, world.TryGet<EnemyAiComp>(groundDrifter)->actionTimerTicks, 0U);

    // 4. Verify Attack-Token Coordinator + Facing Lock During Windup + Dash-Behind Evasion + Turn-Around Delay:
    highPlatXf.x = 0.0f;
    highPlatXf.y = 1.0f;
    world.Teleport(pid, highPlatXf);
    const EntityId secondDrifter = EnemyAiSystem::SpawnDrifter(
        world, app.AssetHandles(), Vec2{0.6f, 1.0f}, 0U, 0U).value_or(kNullEntity);
    world.PropagateTransforms();
    world.TryGet<en::MotionStack2D>(groundDrifter)->SetFacingSign(-1); // Facing Left toward Kaelen at x=0.0
    world.TryGet<en::MotionStack2D>(secondDrifter)->SetFacingSign(-1);
    world.TryGet<EnemyAiComp>(groundDrifter)->cooldownTicks = 0U;
    world.TryGet<EnemyAiComp>(secondDrifter)->cooldownTicks = 0U;

    EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
    u32 simultaneousWindups = 0U;
    for (auto [eid, ai] : world.View<EnemyAiComp>()) {
        if (ai.archetypeId != 4U && ai.actionTimerTicks > 0U) {
            ++simultaneousWindups;
        }
    }
    EXPECT_EQ(suite, simultaneousWindups, 1U);

    // Now simulate Kaelen dashing BEHIND groundDrifter (to x = 1.1f, right behind groundDrifter at x = 0.4f facing Left -1)
    // while groundDrifter is mid-windup! Verify groundDrifter NEVER flips facing mid-windup and its slash MISSES Kaelen!
    highPlatXf.x = 1.1f;
    highPlatXf.y = 1.0f;
    world.Teleport(pid, highPlatXf);
    world.PropagateTransforms();
    recv->iframeTicks = 0U;
    const i32 hpBeforeBehindDodge = recv->hp;

    for (u32 t = 0U; t < 36U; ++t) {
        EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
        if (world.TryGet<EnemyAiComp>(groundDrifter)->actionTimerTicks > 0U) {
            // Facing must remain locked to -1 throughout windup, active, and recovery!
            EXPECT_EQ(suite, world.TryGet<en::MotionStack2D>(groundDrifter)->FacingSign(), -1);
        }
    }
    // Because Kaelen crossed behind groundDrifter, the front slash completely missed!
    EXPECT_EQ(suite, recv->hp, hpBeforeBehindDodge);

    // Verify Turn-Around Reaction Delay (turnDelayTicks) when groundDrifter finishes recovery with Kaelen still behind it:
    for (u32 t = 0U; t < 40U; ++t) {
        EnemyAiSystem::Step(pid, world, gp, &app.Metroidvania());
    }
    EXPECT_EQ(suite, world.TryGet<EnemyAiComp>(groundDrifter)->actionTimerTicks, 0U);
    EXPECT_TRUE(suite, world.TryGet<EnemyAiComp>(groundDrifter)->cooldownTicks >= 80U);

    // 5. Verify Tactile Combat Feel: Attacker Nail Recoil + Player Knockback Arc:
    motor->vx = 0.0f;
    world.TryGet<en::MotionStack2D>(pid)->SetFacingSign(1);
    WeaponAttackComp strike{};
    strike.instanceId = 7777U;
    strike.baseHpDamage = 12;
    strike.baseBreakDamage = 5U;
    strike.capturedEchoId = EchoId::Ignis;
    // Place groundDrifter to the right of Kaelen for recoil test
    en::LocalTransform2D drifterXf = world.GetLocalTransform(groundDrifter);
    drifterXf.x = 2.0f;
    world.Teleport(groundDrifter, drifterXf);
    world.PropagateTransforms();

    (void)CombatSystem::ResolveWeaponHit(pid, groundDrifter, strike, world, gp);
    EXPECT_TRUE(suite, motor->vx <= -kPlayerAttackHitRecoilVx + 0.05f);
    EXPECT_TRUE(suite, world.TryGet<en::CharacterMotor2D>(groundDrifter)->vx >= kEnemyHitKnockbackVx - 0.05f);

    recv->iframeTicks = 0U;
    recv->stunTicks = 0U;
    motor->vx = 0.0f;
    motor->vy = 0.0f;
    (void)CombatSystem::ResolveEnemyHit(
        groundDrifter, pid, 12, 5U, AttackTelegraphKind::GoldHarmonicWave, EchoId::None, world, gp);
    EXPECT_EQ(suite, recv->iframeTicks, kPlayerHitProtectionTicks);
    EXPECT_NEAR(suite, motor->vx, -kPlayerHitKnockbackAwayVx, 0.1f);
    EXPECT_NEAR(suite, motor->vy, kPlayerHitKnockbackUpVy, 0.1f);

    app.Shutdown();
}

} // namespace vots::tests

int main() {
    using namespace vots::tests;
    TestSuite suite{};

    TestSingleWeaponMovesetAndEchoCaptureInvariance(suite);
    TestResonanceMark420TickLifecycleAndExpirationOrder(suite);
    TestAll12SignatureReactionFormulasAndSecondaryRules(suite);
    TestBossBreakRecoveryMultiplicativeDamageAndParryWindow(suite);
    TestHarmonySacredChordsLostWickHuskSaveAndEndings(suite);
    TestFullGameReplayDeterminismAndRoomTransitions(suite);
    TestVisualTilemapSlashVfxEnemyRespawnAndPassThrough(suite);
    TestGddV03JumpDoubleJumpDashProjectileDodgeComboAndBoss(suite);
    TestGddV04HitstunEscapeMultiDashParryInterceptionCinemachineAndWorldBlueprint(suite);
    TestGddV05WallSlideElevationAiCombatRecoilAndVariableRooms(suite);

    std::printf("Veil of the Silent (GDD v0.5) Verification Suite: %u passed, %u failed\n",
                suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}

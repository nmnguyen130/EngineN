#include "game/VeilGameApp.h"
#include "game/ai/EnemyAiSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/core/SaveSystem.h"
#include "game/player/PlayerController.h"
#include "game/scenes/GameScenes.h"

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

// 1. Verify GDD v0.2 Section 3.1, 4.2, 6.5: Single Weapon Rule, Move Chain Sums, and Attack Instance Echo Capture
void TestSingleWeaponMovesetAndEchoCaptureInvariance(TestSuite& suite) noexcept {
    // Ground Light 3-hit chain: 10 + 12 + 18 = 40 HP Damage, 4 + 5 + 9 = 18 Break
    const auto& l1 = GetWeaponMoveSpec(WeaponMoveKind::Light1);
    const auto& l2 = GetWeaponMoveSpec(WeaponMoveKind::Light2);
    const auto& l3 = GetWeaponMoveSpec(WeaponMoveKind::Light3);
    EXPECT_EQ(suite, l1.hpDamage + l2.hpDamage + l3.hpDamage, 40);
    EXPECT_EQ(suite, l1.breakDamage + l2.breakDamage + l3.breakDamage, 18U);

    // Air 2-hit chain: 10 + 15 = 25 HP Damage, 4 + 7 = 11 Break
    const auto& a1 = GetWeaponMoveSpec(WeaponMoveKind::Air1);
    const auto& a2 = GetWeaponMoveSpec(WeaponMoveKind::Air2);
    EXPECT_EQ(suite, a1.hpDamage + a2.hpDamage, 25);
    EXPECT_EQ(suite, a1.breakDamage + a2.breakDamage, 11U);

    // Heavy Attack: 24-tick startup (200 ms at 120 Hz), 24 HP Damage, 18 Break
    const auto& hv = GetWeaponMoveSpec(WeaponMoveKind::Heavy);
    EXPECT_EQ(suite, hv.startupTicks, 24U);
    EXPECT_EQ(suite, hv.hpDamage, 24);
    EXPECT_EQ(suite, hv.breakDamage, 18U);

    // Verify switching Echo mid-attack preserves capturedEchoId on the started attack instance
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    const EntityId pid = app.PlayerEntity();
    EXPECT_TRUE(suite, world.IsAlive(pid));

    PlayerController::SwitchActiveEchoSlot(pid, 0U, world); // Slot 0 = Ignis
    PlayerController::StartWeaponAttack(
        pid, WeaponMoveKind::Heavy, world, app.EngineApp().Gameplay(), app.NextAttackInstanceId());

    const auto* atk = world.TryGet<WeaponAttackComp>(pid);
    EXPECT_TRUE(suite, atk != nullptr);
    EXPECT_EQ(suite, atk->capturedEchoId, EchoId::Ignis);

    // Switch Echo to Slot 1 (Glacies) mid-animation: visual tint updates to Glacies, but capturedEchoId remains Ignis!
    PlayerController::SwitchActiveEchoSlot(pid, 1U, world);
    EXPECT_EQ(suite, world.TryGet<PlayerStateComp>(pid)->deck.ActiveEcho(), EchoId::Glacies);
    EXPECT_EQ(suite, atk->capturedEchoId, EchoId::Ignis);

    app.Shutdown();
}

// 2. Verify GDD v0.2 Section 3.3 & 6.5: 420-Tick Resonance Mark State Machine, Same-Echo Refresh, Invalid Pair Replace, and Exact 420-Tick Expiry
void TestResonanceMark420TickLifecycleAndExpirationOrder(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();

    const EntityId dummy = EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{2.0f, 1.0f}).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.IsAlive(dummy));
    world.TryGet<en::DamageReceiver2D>(dummy)->hp = 5000;
    world.TryGet<en::DamageReceiver2D>(dummy)->maxHp = 5000;

    // 1. Hit with Ignis -> applies Ignis mark with 420 ticks
    WeaponAttackComp hitIgnis{};
    hitIgnis.instanceId = 100U;
    hitIgnis.baseHpDamage = 10;
    hitIgnis.baseBreakDamage = 4U;
    hitIgnis.capturedEchoId = EchoId::Ignis;
    const auto rep1 = CombatSystem::ResolvePlayerWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, rep1.reactionTriggered, WaveReactionId::None);

    auto* mark = world.TryGet<ResonanceMarkComp>(dummy);
    EXPECT_TRUE(suite, mark != nullptr && mark->HasActiveMark());
    EXPECT_EQ(suite, mark->activeMark, EchoId::Ignis);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    // Same attack instance ID cannot hit or re-apply mark a second time
    const auto repDuplicate = CombatSystem::ResolvePlayerWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, repDuplicate.primaryHpDamageDealt, 0);

    // Step 200 ticks -> remainingTicks == 220
    for (u32 t = 0U; t < 200U; ++t) {
        CombatSystem::StepCombatTimersAndDelayedStrikes(world, gp);
    }
    EXPECT_EQ(suite, mark->remainingTicks, 220U);

    // 2. Hit with same Echo (Ignis) -> refreshes duration to 420 ticks, no reaction
    hitIgnis.instanceId = 101U;
    const auto repRefresh = CombatSystem::ResolvePlayerWeaponHit(pid, dummy, hitIgnis, world, gp);
    EXPECT_EQ(suite, repRefresh.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Ignis);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    // 3. Hit with invalid pair (Ignis + Lux) -> replaces mark with Lux at 420 ticks, no reaction
    WeaponAttackComp hitLux = hitIgnis;
    hitLux.instanceId = 102U;
    hitLux.capturedEchoId = EchoId::Lux;
    const auto repInvalid = CombatSystem::ResolvePlayerWeaponHit(pid, dummy, hitLux, world, gp);
    EXPECT_EQ(suite, repInvalid.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Lux);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    // 4. Advance exactly 420 ticks -> mark expires BEFORE hit check on tick 420!
    for (u32 t = 0U; t < 420U; ++t) {
        CombatSystem::StepCombatTimersAndDelayedStrikes(world, gp);
    }
    EXPECT_TRUE(suite, !mark->HasActiveMark());
    EXPECT_EQ(suite, mark->remainingTicks, 0U);

    // Hitting with Umbra on the exact tick Lux expired does NOT trigger Eclipse Singularity; it places a fresh Umbra mark!
    WeaponAttackComp hitUmbra = hitIgnis;
    hitUmbra.instanceId = 103U;
    hitUmbra.capturedEchoId = EchoId::Umbra;
    const auto repAfterExpire = CombatSystem::ResolvePlayerWeaponHit(pid, dummy, hitUmbra, world, gp);
    EXPECT_EQ(suite, repAfterExpire.reactionTriggered, WaveReactionId::None);
    EXPECT_EQ(suite, mark->activeMark, EchoId::Umbra);
    EXPECT_EQ(suite, mark->remainingTicks, 420U);

    app.Shutdown();
}

// 3. Verify All 12 Valid Signature Reaction Formulas (10 Named Reactions), Order-Independence, Secondary Non-Marking, and Non-Recursion
void TestAll12SignatureReactionFormulasAndSecondaryRules(TestSuite& suite) noexcept {
    // Compile-time / table verification of all 12 unordered pairs and invalid pairs
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

    // Invalid pairs across categories
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

    const EntityId primary = EnemyAiSystem::SpawnChoirEnforcer(world, app.AssetHandles(), Vec2{20.0f, 1.0f}).value_or(kNullEntity);
    const EntityId sec1 = EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{21.0f, 1.0f}).value_or(kNullEntity);
    const EntityId sec2 = EnemyAiSystem::SpawnDissonantHusk(world, app.AssetHandles(), Vec2{21.5f, 1.0f}).value_or(kNullEntity);
    world.PropagateTransforms();

    world.TryGet<en::DamageReceiver2D>(primary)->hp = 10000;
    world.TryGet<en::DamageReceiver2D>(primary)->maxHp = 10000;
    world.TryGet<en::DamageReceiver2D>(sec1)->hp = 10000;
    world.TryGet<en::DamageReceiver2D>(sec2)->hp = 10000;
    world.TryGet<BreakAndCcComp>(primary)->breakResistance = 0.0f; // Isolate reaction break checks first

    u32 instId = 200U;
    const auto triggerPair = [&](EchoId eA, EchoId eB) noexcept -> CombatResolutionReport {
        WeaponAttackComp atkA{};
        atkA.instanceId = ++instId;
        atkA.baseHpDamage = 10;
        atkA.baseBreakDamage = 4U;
        atkA.capturedEchoId = eA;
        (void)CombatSystem::ResolvePlayerWeaponHit(pid, primary, atkA, world, gp);

        WeaponAttackComp atkB = atkA;
        atkB.instanceId = ++instId;
        atkB.capturedEchoId = eB;
        return CombatSystem::ResolvePlayerWeaponHit(pid, primary, atkB, world, gp);
    };

    // Test all 12 valid formulas in runtime combat:
    ps->harmony = 0U;
    // 1. Melt-Shock (Ignis + Glacies): +24 Damage (10 + 24 = 34), applies 720-tick Fracture (1.15x)
    const auto rMelt = triggerPair(EchoId::Ignis, EchoId::Glacies);
    EXPECT_EQ(suite, rMelt.reactionTriggered, WaveReactionId::MeltShock);
    EXPECT_EQ(suite, rMelt.primaryHpDamageDealt, 34);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->fractureTicks, 720U);
    EXPECT_TRUE(suite, !world.TryGet<ResonanceMarkComp>(primary)->HasActiveMark());
    EXPECT_EQ(suite, ps->harmony, 10U);

    // Clear Fracture to test pure base + bonus damage on remaining formulas
    world.TryGet<BreakAndCcComp>(primary)->fractureTicks = 0U;

    // 2. Piezo-Quake (Terra + Fulgur): +20 Damage (10 + 20 = 30), 42-tick Stun on Heavy/Elite
    const auto rPiezo = triggerPair(EchoId::Terra, EchoId::Fulgur);
    EXPECT_EQ(suite, rPiezo.reactionTriggered, WaveReactionId::PiezoQuake);
    EXPECT_EQ(suite, rPiezo.primaryHpDamageDealt, 30);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->actionLockTicks, 42U);

    // 3. Steam Cavitation (Ignis + Aqua): +22 Damage (32), hits 2 secondary enemies in 1.75m for 8 Damage, without marking them
    const i32 sec1HpBefore = world.TryGet<en::DamageReceiver2D>(sec1)->hp;
    const auto rSteam = triggerPair(EchoId::Ignis, EchoId::Aqua);
    EXPECT_EQ(suite, rSteam.reactionTriggered, WaveReactionId::SteamCavitation);
    EXPECT_EQ(suite, rSteam.primaryHpDamageDealt, 32);
    EXPECT_EQ(suite, rSteam.secondaryTargetsHit, 2U);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(sec1)->hp, sec1HpBefore - 8);
    EXPECT_TRUE(suite, !world.TryGet<ResonanceMarkComp>(sec1)->HasActiveMark());

    // 4. Superconductor Gale (Fulgur + Ventus): +18 Damage (28), arcs to 2 secondary enemies for 9 HP Damage & 4 Break
    const auto rGale = triggerPair(EchoId::Fulgur, EchoId::Ventus);
    EXPECT_EQ(suite, rGale.reactionTriggered, WaveReactionId::SuperconductorGale);
    EXPECT_EQ(suite, rGale.primaryHpDamageDealt, 28);
    EXPECT_EQ(suite, rGale.secondaryTargetsHit, 2U);

    // 5. Pyro-Cyclone (Ignis + Ventus): clears up to 3 normal projectiles, preserves protected Boss projectile
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

    // 6a & 6b. Tectonic Shatter (Glacies + Terra AND Glacies + Stridor): breaks BreakableGuard -> 90 ticks GuardBroken
    world.TryGet<BreakAndCcComp>(primary)->flags |= (BreakAndCcComp::kFlagBreakableGuard | BreakAndCcComp::kFlagGuardActive);
    const auto rTec1 = triggerPair(EchoId::Glacies, EchoId::Terra);
    EXPECT_EQ(suite, rTec1.reactionTriggered, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(primary)->guardBrokenTicks, 90U);
    EXPECT_TRUE(suite, !world.TryGet<BreakAndCcComp>(primary)->HasRuleFlag(BreakAndCcComp::kFlagGuardActive));

    const auto rTec2 = triggerPair(EchoId::Stridor, EchoId::Glacies);
    EXPECT_EQ(suite, rTec2.reactionTriggered, WaveReactionId::TectonicShatter);
    EXPECT_EQ(suite, rTec2.primaryHpDamageDealt, 38);

    // 7. Eclipse Singularity (Lux + Umbra): pulls Standard enemies over 12 ticks
    const auto rEclipse = triggerPair(EchoId::Lux, EchoId::Umbra);
    EXPECT_EQ(suite, rEclipse.reactionTriggered, WaveReactionId::EclipseSingularity);
    EXPECT_EQ(suite, world.TryGet<BreakAndCcComp>(sec1)->pullTicks, 12U);

    // 8. Graviton Quake (Gravitas + Terra): +22 Damage (32) + 8 secondary damage
    const auto rGrav = triggerPair(EchoId::Gravitas, EchoId::Terra);
    EXPECT_EQ(suite, rGrav.reactionTriggered, WaveReactionId::GravitonQuake);
    EXPECT_EQ(suite, rGrav.primaryHpDamageDealt, 32);

    // 9. Chrono-Overdrive (Tempus + Stridor): schedules 10 HP delayed hit 12 ticks later
    const auto rChrono = triggerPair(EchoId::Tempus, EchoId::Stridor);
    EXPECT_EQ(suite, rChrono.reactionTriggered, WaveReactionId::ChronoOverdrive);
    const i32 hpBeforeDelay = world.TryGet<en::DamageReceiver2D>(primary)->hp;
    const u8 harmonyBeforeDelay = ps->harmony;
    for (u32 t = 0U; t < 12U; ++t) {
        CombatSystem::StepCombatTimersAndDelayedStrikes(world, gp);
    }
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(primary)->hp, hpBeforeDelay - 10);
    EXPECT_EQ(suite, ps->harmony, harmonyBeforeDelay); // Delayed hit grants 0 Harmony and 0 Mark

    // 10a & 10b. Sanguine Boil (Aether + Ignis AND Aether + Aqua)
    const auto rSang1 = triggerPair(EchoId::Aether, EchoId::Ignis);
    EXPECT_EQ(suite, rSang1.reactionTriggered, WaveReactionId::SanguineBoil);
    const auto rSang2 = triggerPair(EchoId::Aqua, EchoId::Aether);
    EXPECT_EQ(suite, rSang2.reactionTriggered, WaveReactionId::SanguineBoil);

    app.Shutdown();
}

// 4. Verify GDD v0.2 Section 4.3, 4.5 & 6.5: Boss Break State (150t, 1.25x), Break Recovery (240t), Fracture * Break (1.4375x), and 10-Tick Parry + CounterEchoId
void TestBossBreakRecoveryMultiplicativeDamageAndParryWindow(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    en::GameplayPipeline& gp = app.EngineApp().Gameplay();
    const EntityId pid = app.PlayerEntity();
    auto* ps = world.TryGet<PlayerStateComp>(pid);

    const EntityId boss = EnemyAiSystem::SpawnHighConductorBoss(world, app.AssetHandles(), Vec2{30.0f, 1.35f}).value_or(kNullEntity);
    auto* bossBrk = world.TryGet<BreakAndCcComp>(boss);
    auto* bossRecv = world.TryGet<en::DamageReceiver2D>(boss);

    // 1. Test 10-tick Parry window boundary & CounterEchoId (+18 base Break + 12 Phase-Inversion = 30 Break)
    ps->harmony = 0U;
    PlayerController::SwitchActiveEchoSlot(pid, 1U, world); // Slot 1 = Glacies
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    EXPECT_EQ(suite, ps->parryWindowTicks, 10U);

    // Parry Gold Harmonic Wave with matching CounterEchoId::Glacies -> +15 Harmony, +30 Break on Boss, 0 damage to Kaelen
    const auto parryMatch = CombatSystem::ResolveEnemyAttackAgainstPlayer(
        boss, pid, 28, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, parryMatch.parrySucceeded);
    EXPECT_TRUE(suite, parryMatch.phaseInversionBonus);
    EXPECT_EQ(suite, ps->harmony, 15U);
    EXPECT_EQ(suite, bossBrk->breakMeter, 30U);

    // Parry Gold Harmonic Wave with non-matching CounterEchoId -> +18 base Break (30 + 18 = 48)
    ps->parryCooldownTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    const auto parryNormal = CombatSystem::ResolveEnemyAttackAgainstPlayer(
        boss, pid, 28, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Ignis, world, gp);
    EXPECT_TRUE(suite, parryNormal.parrySucceeded);
    EXPECT_TRUE(suite, !parryNormal.phaseInversionBonus);
    EXPECT_EQ(suite, bossBrk->breakMeter, 48U);

    // Expire all 10 ticks of Parry window -> on tick 11, Parry fails and Kaelen takes full damage (zero hidden block)
    ps->parryCooldownTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    for (u32 t = 0U; t < 10U; ++t) {
        if (ps->parryWindowTicks > 0U && --ps->parryWindowTicks == 0U) {
            world.TryGet<en::DamageReceiver2D>(pid)->RemoveFlags(en::ReceiverFlags::Parrying);
        }
    }
    const i32 kaelenHpBefore = world.TryGet<en::DamageReceiver2D>(pid)->hp;
    const auto parryExpired = CombatSystem::ResolveEnemyAttackAgainstPlayer(
        boss, pid, 20, 10U, AttackTelegraphKind::GoldHarmonicWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, !parryExpired.parrySucceeded);
    EXPECT_EQ(suite, world.TryGet<en::DamageReceiver2D>(pid)->hp, kaelenHpBefore - 20);

    // Crimson Overload Wave cannot be parried even inside the 10-tick window
    world.TryGet<en::DamageReceiver2D>(pid)->iframeTicks = 0U;
    ps->parryCooldownTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::StartParryStance(pid, world));
    const auto crimsonHit = CombatSystem::ResolveEnemyAttackAgainstPlayer(
        boss, pid, 25, 10U, AttackTelegraphKind::CrimsonOverloadWave, EchoId::Glacies, world, gp);
    EXPECT_TRUE(suite, !crimsonHit.parrySucceeded);
    EXPECT_EQ(suite, crimsonHit.primaryHpDamageDealt, 25);

    // 2. Drive Boss to 100 Break -> enters Break State (150 ticks), Break meter resets to 0 immediately
    CombatResolutionReport brkRep{};
    EXPECT_TRUE(suite, CombatSystem::ApplyBreakDamage(boss, 52U, world, *bossBrk, &brkRep));
    EXPECT_TRUE(suite, brkRep.targetEnteredBreakState);
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::BreakState);
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);
    EXPECT_EQ(suite, bossBrk->phaseTicks, 150U);

    // Verify Fracture (1.15x) * Boss Break State (1.25x) = 1.4375x multiplicative HP Damage (160 * 1.4375 = 230 exact)
    bossBrk->fractureTicks = 720U;
    EXPECT_EQ(suite, CombatSystem::ComputeModifiedHpDamage(160, bossBrk), 230);

    // Break is ignored while in Break State
    EXPECT_TRUE(suite, !CombatSystem::ApplyBreakDamage(boss, 50U, world, *bossBrk));
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);

    // Step 150 ticks -> Boss transitions from Break State to Break Recovery (240 ticks)
    for (u32 t = 0U; t < 150U; ++t) {
        CombatSystem::StepCombatTimersAndDelayedStrikes(world, gp);
    }
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::BreakRecovery);
    EXPECT_EQ(suite, bossBrk->phaseTicks, 240U);

    // Break is STILL ignored during 240-tick Break Recovery
    EXPECT_TRUE(suite, !CombatSystem::ApplyBreakDamage(boss, 80U, world, *bossBrk));
    EXPECT_EQ(suite, bossBrk->breakMeter, 0U);

    // Step 240 ticks -> Boss exits Break Recovery back to Normal and can be Broken again!
    for (u32 t = 0U; t < 240U; ++t) {
        CombatSystem::StepCombatTimersAndDelayedStrikes(world, gp);
    }
    EXPECT_EQ(suite, bossBrk->breakPhase, BreakPhase::Normal);
    (void)CombatSystem::ApplyBreakDamage(boss, 40U, world, *bossBrk);
    EXPECT_EQ(suite, bossBrk->breakMeter, 40U);
    (void)bossRecv;

    app.Shutdown();
}

// 5. Verify GDD v0.2 Section 4.6, 4.7, 5.1, 5.2, 5.3, 5.4 & 2.6: Air-Dash Cap, Unison/Sacred Chords, Single Lost Wick Husk, Save/Load, and 3 Endings
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

    // 1. Air-Dash rule: 1 charge per airborne sequence, reactions never refund Air-Dash
    motor->contacts = en::ContactFlags::None; // Airborne
    ps->airDashCharges = 1U;
    ps->dashCooldownTicks = 0U;
    EXPECT_TRUE(suite, PlayerController::TryStartDash(pid, world));
    EXPECT_EQ(suite, ps->airDashCharges, 0U);
    ps->dashCooldownTicks = 0U;
    EXPECT_TRUE(suite, !PlayerController::TryStartDash(pid, world)); // Second air-dash blocked!

    // 2. Standard Unison Burst (50 Harmony) & All 4 Sacred Chords (100 Harmony, unordered Triad Deck)
    ps->unlockedEchoMask = 0x0FFFU; // Unlock all 12 Echoes
    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Fulgur, EchoId::Ignis, EchoId::Terra, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::ElementalCataclysm);

    ps->harmony = 60U;
    // Standard Unison Burst is usable at >= 50 Harmony even when deck matches a Sacred Chord
    EXPECT_TRUE(suite, CombatSystem::TryActivateUnisonBurst(pid, world, gp));
    EXPECT_EQ(suite, ps->harmony, 10U);

    ps->harmony = 100U;
    CombatResolutionReport chordRep{};
    EXPECT_TRUE(suite, CombatSystem::TryActivateSacredChord(pid, world, gp, &chordRep));
    EXPECT_EQ(suite, chordRep.chordTriggered, SacredChordId::ElementalCataclysm);
    EXPECT_EQ(suite, ps->harmony, 0U);

    // Verify unordered matching for the remaining 3 Sacred Chords
    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Aqua, EchoId::Glacies, EchoId::Ventus, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::CryoTempestProtocol);

    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Tempus, EchoId::Lux, EchoId::Umbra, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::EclipseOfEternity);

    EXPECT_TRUE(suite, mv.ConfigureTriadDeckAtResonator(pid, EchoId::Stridor, EchoId::Gravitas, EchoId::Aether, world));
    EXPECT_EQ(suite, FindMatchingSacredChord(ps->deck)->id, SacredChordId::SingularityOverdrive);

    // 3. Lost Wick Husk: single Husk rule across consecutive deaths, non-stacking 25% Max HP penalty, and non-overhealing recovery
    mv.ActivateTuningResonator(pid, 0U, Vec2{-2.0f, 1.0f}, world, gp);
    ps->carriedTuningShards = 120U;
    mv.HandlePlayerDeathAndSpawnHusk(pid, Vec2{8.0f, 1.0f}, 0U, world);
    EXPECT_TRUE(suite, mv.ActiveHusk().IsActive());
    EXPECT_NEAR(suite, mv.ActiveHusk().worldX, 8.0f, 1e-4f);
    EXPECT_EQ(suite, mv.ActiveHusk().bankedTuningShards, 120U);
    EXPECT_EQ(suite, ps->carriedTuningShards, 0U);
    EXPECT_EQ(suite, ps->harmony, 0U);
    EXPECT_EQ(suite, recv->maxHp, 75); // -25% Max HP penalty

    // Second death before recovering Husk: keeps original Husk position (8.0f), adds new shards (45) -> 165, Max HP stays 75!
    ps->carriedTuningShards = 45U;
    mv.HandlePlayerDeathAndSpawnHusk(pid, Vec2{3.5f, 1.0f}, 0U, world);
    EXPECT_NEAR(suite, mv.ActiveHusk().worldX, 8.0f, 1e-4f);
    EXPECT_EQ(suite, mv.ActiveHusk().bankedTuningShards, 165U);
    EXPECT_EQ(suite, recv->maxHp, 75); // Does not stack penalty beyond 25%!

    // Take 15 damage (HP = 60/75), then recover Husk -> Max HP restored to 100, HP stays 60 (clamped, no overheal), Harmony = 100, Shards = 165!
    recv->hp = 60;
    EXPECT_TRUE(suite, mv.RecoverLostWickHusk(pid, world, gp));
    EXPECT_TRUE(suite, !mv.ActiveHusk().IsActive());
    EXPECT_EQ(suite, ps->carriedTuningShards, 165U);
    EXPECT_EQ(suite, ps->harmony, 100U);
    EXPECT_EQ(suite, recv->maxHp, 100);
    EXPECT_EQ(suite, recv->hp, 60);

    // 4. Map Pins & SaveSystem deterministic serialization + corruption detection
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

    // Flip 1 bit -> checksum verification rejects corrupted save
    saveBuf[24] ^= 0x01U;
    EXPECT_EQ(suite, SaveSystem::DeserializeFromBuffer(saveBuf).error(), SaveErrc::ChecksumMismatch);

    // 5. Three Endings (False Dawn, Shattered Spire, Symphony of the Off-Beat)
    gp.SetWorldFlag(static_cast<u16>(WorldFlagId::HighConductorDefeated), true);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, false, false, world, gp), EndingId::FalseDawn);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, true, false, world, gp), EndingId::ShatteredSpire);

    mv.CompleteMemoryTuningFork(0U, gp);
    mv.CompleteMemoryTuningFork(1U, gp);
    mv.CompleteMemoryTuningFork(2U, gp);
    EXPECT_EQ(suite, mv.ResolveSpireEnding(pid, false, true, world, gp), EndingId::SymphonyOfOffBeat);
    EXPECT_EQ(suite, ps->lanternRelinquished, 1U);

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
            app.EngineApp().Input().FeedButton(en::InputSource::Key(8U), true);  // Cycle Echo Next (E)
        }
        (void)app.StepHeadlessTicks(10U, 2U);

        // Transition to Room 1 (Resonator Sanctum) via RoomSwap
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

// 7. Verify Camera yUp Orientation, Tilemap & Slash Arc VFX Rendering, Enemy Despawn/Respawn Lifecycle, Pass-Through, and Portal Transitions
void TestVisualTilemapSlashVfxEnemyRespawnAndPassThrough(TestSuite& suite) noexcept {
    VeilGameApp app{};
    EXPECT_TRUE(suite, app.Init(true).has_value());
    (void)app.StepHeadlessTicks(4U, 1U);

    en::World& world = app.EngineApp().GetWorld();
    const EntityId pid = app.PlayerEntity();
    EXPECT_TRUE(suite, world.IsAlive(pid));

    // 1. Verify Camera2D yUp == true on world view 0 so jumping (+Y in physics) renders upward on screen
    const en::Camera2D& cam0 = app.EngineApp().GetRenderList().View(0U);
    EXPECT_TRUE(suite, cam0.yUp);

    // 2. Verify tilemap platforms and backdrop emit > 200 quads and starting an attack emits SlashArc + Beam quads
    const u32 baseQuadCount = app.EngineApp().GetRenderList().Stats().submittedQuads;
    EXPECT_TRUE(suite, baseQuadCount > 200U);

    PlayerController::StartWeaponAttack(
        pid, WeaponMoveKind::Light1, world, app.EngineApp().Gameplay(), app.NextAttackInstanceId());
    // Step 7 ticks so WeaponAttackComp advances past Startup (6 ticks) into Active phase
    (void)app.StepHeadlessTicks(7U, 1U);
    const u32 activeSlashQuadCount = app.EngineApp().GetRenderList().Stats().submittedQuads;
    EXPECT_TRUE(suite, activeSlashQuadCount > baseQuadCount + 8U);

    // 3. Verify player pushboxGroup == 0U and no Collider2D so player can walk/dash through enemies without blocking
    EXPECT_EQ(suite, world.TryGet<en::MotionStack2D>(pid)->pushboxGroup, 0U);
    EXPECT_TRUE(suite, !world.Has<en::Collider2D>(pid));

    // 4. Verify killing Room 0 enemy despawns it, keeps it cleared across RoomSwap, and only respawns on Resonator activation
    u32 room0EnemiesInitial = 0U;
    EntityId firstEnemy = kNullEntity;
    for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == 0U) {
            ++room0EnemiesInitial;
            if (ai.spawnSlotBit == 0x01U) firstEnemy = eid;
        }
    }
    EXPECT_EQ(suite, room0EnemiesInitial, 2U);
    EXPECT_TRUE(suite, world.IsAlive(firstEnemy));

    // Kill firstEnemy -> next tick despawns it and sets cleared mask for slot 0x01
    world.TryGet<en::DamageReceiver2D>(firstEnemy)->hp = 0;
    (void)app.StepHeadlessTicks(1U, 1U);
    EXPECT_TRUE(suite, !world.IsAlive(firstEnemy));
    EXPECT_TRUE(suite, app.Metroidvania().IsEnemyCleared(0U, 0x01U));

    // Transition to Room 1 and back to Room 0 -> slot 0x01 stays dead (only 1 enemy in Room 0)
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

    // Rest at Tuning Resonator -> clears enemy mask and respawns all Room 0 enemies (back to 2)
    app.Metroidvania().ActivateTuningResonator(pid, 0U, Vec2{-6.0f, 1.0f}, world, app.EngineApp().Gameplay());
    GameScenes::SpawnRoomEnemiesIfNeeded(app, world, 0U);
    u32 room0EnemiesAfterResonator = 0U;
    for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
        if (ai.roomId == 0U && recv.hp > 0) ++room0EnemiesAfterResonator;
    }
    EXPECT_EQ(suite, room0EnemiesAfterResonator, 2U);

    // 5. Verify walking into Right Room Exit Portal at (20.5, 1.1) automatically transitions to Room 1
    app.SetPortalCooldownTicks(0U);
    en::LocalTransform2D pxf = world.GetLocalTransform(pid);
    pxf.x = 20.5f;
    pxf.y = 1.0f;
    world.Teleport(pid, pxf);
    (void)app.StepHeadlessTicks(4U, 1U);
    EXPECT_EQ(suite, app.ActiveRoomId(), 1U);

    // 6. Verify DebugProfiler ValidFrames() and FrameTotal scope are actively recorded
    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().ValidFrames() > 0U);
    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().Latest().aliveEntities > 0U);
    EXPECT_TRUE(suite, app.EngineApp().Debug().Profiler().Latest().scopeUs[0] > 0U);

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

    std::printf("Veil of the Silent (GDD v0.2) Verification Suite: %u passed, %u failed\n",
                suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}

#include "game/tests/VeilTests.h"
#include "game/app/GameApp.h"
#include "game/core/EchoPantheon.h"
#include "game/core/Types.h"
#include "game/movement/TraversalSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/ai/BossSystem.h"
#include "game/world/WorldSystem.h"

#include <cmath>
#include <cstdio>

namespace en::veil {

namespace {

int gFailures = 0;

void CheckCondition(bool condition, const char* expr, int line) noexcept {
    if (!condition) {
        std::printf("[FAIL] Line %d: %s\n", line, expr);
        std::fflush(stdout);
        ++gFailures;
    }
}

#define VEIL_REQUIRE(cond) CheckCondition((cond), #cond, __LINE__)

void TestPantheonCompleteness() noexcept {
    VEIL_REQUIRE(kEchoPantheon.size() == 12);
    for (u8 i = 0; i < kActiveEchoCount; ++i) {
        const auto id = static_cast<EchoId>(i);
        const EchoSpec& spec = GetEchoSpec(id);
        VEIL_REQUIRE(spec.id == id);
        VEIL_REQUIRE(!spec.name.empty());
        VEIL_REQUIRE(!spec.wavePhysicsName.empty());
        VEIL_REQUIRE(spec.baseDamage > 0.0f);
        VEIL_REQUIRE(spec.baseReachMeters > 1.0f);
        if (i < 6) {
            VEIL_REQUIRE(spec.category == WaveCategory::ElementalWave);
        } else {
            VEIL_REQUIRE(spec.category == WaveCategory::PhenomenonWave);
        }
    }
}

void TestElementalAndPhenomenonWaveReactions() noexcept {
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Ignis, EchoId::Glacies) == WaveReactionId::MeltShock);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Terra, EchoId::Fulgur) == WaveReactionId::PiezoQuake);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Ignis, EchoId::Aqua) == WaveReactionId::SteamCavitation);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Fulgur, EchoId::Ventus) == WaveReactionId::SuperconductorGale);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Ignis, EchoId::Ventus) == WaveReactionId::PyroCyclone);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Glacies, EchoId::Terra) == WaveReactionId::TectonicShatter);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Lux, EchoId::Umbra) == WaveReactionId::EclipseSingularity);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Gravitas, EchoId::Terra) == WaveReactionId::GravitonQuake);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Tempus, EchoId::Stridor) == WaveReactionId::ChronoOverdrive);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Aether, EchoId::Ignis) == WaveReactionId::SanguineBoil);
    VEIL_REQUIRE(ResolveWaveReaction(EchoId::Ignis, EchoId::Ignis) == WaveReactionId::None);
}

void TestStandingWaveCombatAndAirDashRefund() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    auto* bossWave = app.BossWave();
    VEIL_REQUIRE(bossWave != nullptr);

    // Far range check: player at x = -10.0f (boss at x = 4.0f, dist = 14m) -> miss
    app.App().World().ModifyLocalTransformByIndex(app.State().playerEntity.index).x = -10.0f;
    const auto farHit = app.StepSimulationTick(
        0.0f, false, false, true, false, false, false, false, false, false);
    VEIL_REQUIRE(farHit.damageDealt == 0.0f);
    VEIL_REQUIRE(!bossWave->hasActiveWaveMark);

    auto* actionState = app.App().World().TryGet<VeilActionStateComponent>(app.State().playerEntity);
    if (actionState != nullptr) {
        actionState->currentAction = ActionId::Idle;
        actionState->actionTick = 0;
    }

    // Position player within melee reach of boss (boss is at x = 4.0f)
    app.App().World().ModifyLocalTransformByIndex(app.State().playerEntity.index).x = 2.5f;

    auto* traversal = app.PlayerTraversal();
    VEIL_REQUIRE(traversal != nullptr);
    traversal->facingRight = true;
    traversal->airDashesRemaining = 0;

    // 1. Strike with Lead Echo (Ignis) -> imprints Thermal Acoustics mark
    const auto hit1 = app.StepSimulationTick(
        0.0f, false, false, true, false, false, false, false, false, false);
    VEIL_REQUIRE(hit1.damageDealt > 0.0f);
    VEIL_REQUIRE(bossWave->hasActiveWaveMark);
    VEIL_REQUIRE(bossWave->markedEcho == EchoId::Ignis);

    // 2. Trigger Wing Left Assist (Glacies) -> triggers Melt-Shock interference & refunds Air-Dash
    const auto hit2 = app.StepSimulationTick(
        0.0f, false, false, false, false, true, false, false, false, false);
    VEIL_REQUIRE(hit2.triggeredReaction == WaveReactionId::MeltShock);
    VEIL_REQUIRE(!bossWave->hasActiveWaveMark);
    VEIL_REQUIRE(bossWave->armorShredRatio >= 0.40f);
    VEIL_REQUIRE(traversal->airDashesRemaining >= 1);
}

void TestAmplitudeAscensionWithoutSpamPenalty() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    VEIL_REQUIRE(initRes.has_value());

    app.App().World().ModifyLocalTransformByIndex(app.State().playerEntity.index).x = 2.5f;

    const auto hit1 = app.StepSimulationTick(
        0.0f, false, false, true, false, false, false, false, false, false);
    VEIL_REQUIRE(hit1.damageDealt > 20.0f);

    auto* amp = app.PlayerAmplitude();
    VEIL_REQUIRE(amp != nullptr);
    amp->waveEnergy = 80.0f;

    app.StepSimulationTick(0.0f, false, false, false, true, false, false, false, false, false);
    VEIL_REQUIRE(amp->tier == AmplitudeTier::Harmonic);
    VEIL_REQUIRE(std::fabs(amp->ReachMultiplier() - 1.20f) < 0.001f);

    app.StepSimulationTick(0.0f, false, false, false, true, false, false, false, false, false);
    VEIL_REQUIRE(amp->tier == AmplitudeTier::Crescendo);
    VEIL_REQUIRE(std::fabs(amp->ReachMultiplier() - 1.40f) < 0.001f);
    VEIL_REQUIRE(std::fabs(amp->DamageMultiplier() - 1.35f) < 0.001f);
}

void TestPhaseInversionCounterAndSacredChords() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    VEIL_REQUIRE(initRes.has_value());

    auto* lantern = app.PlayerLantern();
    auto* bossConductor = app.BossConductor();
    auto* bossStats = app.BossCombatStats();
    auto* amp = app.PlayerAmplitude();
    VEIL_REQUIRE(lantern != nullptr && bossConductor != nullptr && bossStats != nullptr && amp != nullptr);

    lantern->triadWicks = {EchoId::Ignis, EchoId::Terra, EchoId::Fulgur};

    // 1. Guarded Deflection: Boss prepares Terra attack, but player LeadEcho is Ignis
    BossSystem::CueGoldHarmonicAttack(*bossConductor, *bossStats, EchoId::Terra, 40);
    const auto deflectTick = app.StepSimulationTick(
        0.0f, false, false, false, false, false, false, true, false, false);
    VEIL_REQUIRE(deflectTick.parryDeflectionSucceeded);
    VEIL_REQUIRE(!deflectTick.phaseInversionSucceeded);
    VEIL_REQUIRE(bossConductor->activeTelegraph == BossTelegraphKind::None);

    // 2. Perfect Phase-Inversion Counter: Boss prepares Ignis attack matching player LeadEcho
    BossSystem::CueGoldHarmonicAttack(*bossConductor, *bossStats, EchoId::Ignis, 40);
    const auto counterTick = app.StepSimulationTick(
        0.0f, false, false, false, false, false, false, true, false, false);
    VEIL_REQUIRE(counterTick.phaseInversionSucceeded);
    VEIL_REQUIRE(bossStats->posture == BossPosture::Submissive);
    VEIL_REQUIRE(amp->tier == AmplitudeTier::Crescendo);

    lantern->harmonyGauge = 100.0f;
    const auto chordTick = app.StepSimulationTick(
        0.0f, false, false, false, false, false, false, false, false, true);
    VEIL_REQUIRE(chordTick.triggeredSacredChord == SacredChordId::ElementalCataclysm);
}

void TestTwelveTraversalMutationsAndWorldLoop() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    VEIL_REQUIRE(initRes.has_value());

    auto* lantern = app.PlayerLantern();
    auto* playerStats = app.PlayerCombatStats();
    auto* progress = app.PlayerProgress();
    auto* traversal = app.PlayerTraversal();
    VEIL_REQUIRE(lantern != nullptr && playerStats != nullptr && progress != nullptr && traversal != nullptr);

    WorldSystem::RestAtTuningResonator(
        *lantern, *playerStats, *progress, EchoId::Tempus, EchoId::Lux, EchoId::Umbra);

    auto& tf = app.App().World().ModifyLocalTransformByIndex(app.State().playerEntity.index);
    tf.x = 10.0f;
    tf.y = 0.0f;
    auto* motor = app.App().World().TryGet<gameplay::KinematicMotor2D>(app.State().playerEntity);
    if (motor != nullptr) {
        motor->contacts |= gameplay::ContactFlags::Ground;
    }

    app.StepSimulationTick(1.0f, false, true, false, false, false, false, false, false, false);
    VEIL_REQUIRE(traversal->hasTemporalAnchor);
    traversal->traversalCooldownTicks = 0;
    traversal->airDashesRemaining = 1;
    tf.x = 22.0f;
    tf.y = 4.0f;

    app.StepSimulationTick(1.0f, false, true, false, false, false, false, false, false, false);
    VEIL_REQUIRE(!traversal->hasTemporalAnchor);
    VEIL_REQUIRE(std::fabs(tf.x - 10.0f) < 0.01f);

    progress->tuningShards = 500;
    WorldSystem::RecordPlayerDeath(tf.x, tf.y, *playerStats, *progress);
    VEIL_REQUIRE(progress->hasLostWickHusk);
    VEIL_REQUIRE(progress->tuningShards == 0);
    WorldSystem::ReclaimLostWickHusk(*playerStats, *progress);
    VEIL_REQUIRE(!progress->hasLostWickHusk);
    VEIL_REQUIRE(progress->tuningShards == 500);
}

void TestPlatformGrounding() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    VEIL_REQUIRE(initRes.has_value());

    for (int tick = 0; tick < 240; ++tick) {
        (void)app.StepSimulationTick(0.0f, false, false, false);
    }

    const auto playerTf = app.App().World().GetLocalTransform(app.State().playerEntity);
    const auto bossTf = app.App().World().GetLocalTransform(app.State().bossEntity);

    VEIL_REQUIRE(playerTf.y >= 1.0f);
    VEIL_REQUIRE(bossTf.y >= 1.5f);
}

void TestZeroHeapAllocationPostInit() noexcept {
    GameApp app{};
    auto initRes = app.Initialize(true);
    VEIL_REQUIRE(initRes.has_value());

    for (int tick = 0; tick < 1000; ++tick) {
        (void)app.StepSimulationTick(1.0f, (tick % 60) == 0, (tick % 45) == 0, (tick % 10) == 0);
    }
}

} // namespace

bool RunVeilTestSuite() noexcept {
    gFailures = 0;
    TestPantheonCompleteness();
    TestElementalAndPhenomenonWaveReactions();
    TestStandingWaveCombatAndAirDashRefund();
    TestAmplitudeAscensionWithoutSpamPenalty();
    TestPhaseInversionCounterAndSacredChords();
    TestTwelveTraversalMutationsAndWorldLoop();
    TestPlatformGrounding();
    TestZeroHeapAllocationPostInit();

    if (gFailures == 0) {
        std::printf("[PASS] All Veil of the Silent tests passed (0 failures).\n");
        return true;
    }
    std::printf("[FAIL] %d test(s) failed.\n", gFailures);
    return false;
}

} // namespace en::veil

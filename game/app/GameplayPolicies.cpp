#include "game/app/GameplayPolicies.h"
#include "game/movement/TraversalSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/ai/BossSystem.h"
#include "game/core/EchoPantheon.h"
#include "game/core/ActionTable.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "world/World.h"

namespace en::veil {

void GameplayPolicies::EvaluateActor(
    const gameplay::ActorPolicyContext& ctx,
    gameplay::KinematicMotor2D& motor,
    gameplay::MotionStack2D& motion,
    gameplay::StateMachine2D& fsm,
    gameplay::HitboxRig2D* optRig) noexcept {
    (void)fsm;
    (void)optRig;

    auto* traversal = ctx.world.TryGet<TraversalMutationComponent>(ctx.entity);
    if (traversal == nullptr) {
        return;
    }

    auto* lantern = ctx.world.TryGet<ResonanceLanternComponent>(ctx.entity);
    static constexpr ResonanceLanternComponent kDefaultLantern{};
    const ResonanceLanternComponent& activeLantern = (lantern != nullptr) ? *lantern : kDefaultLantern;

    world::LocalTransform2D* localTf = ctx.world.IsAlive(ctx.entity)
        ? &ctx.world.ModifyLocalTransformByIndex(ctx.entity.index)
        : nullptr;

    f32 moveX = ctx.tickInput.Axis(0u);
    if (std::abs(moveX) < 0.1f) {
        if (ctx.tickInput.Held(static_cast<u8>(GameAction::MoveRight))) {
            moveX = 1.0f;
        } else if (ctx.tickInput.Held(static_cast<u8>(GameAction::MoveLeft))) {
            moveX = -1.0f;
        }
    }

    const bool jumpPressed = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::Jump), 6) ||
                             ctx.tickInput.Pressed(static_cast<u8>(GameAction::Jump));
    const bool evadePressed = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::EvadeDash), 6) ||
                              ctx.tickInput.Pressed(static_cast<u8>(GameAction::EvadeDash));

    TraversalSystem::Step(
        motor, motion, *traversal, activeLantern, moveX, jumpPressed, evadePressed, localTf, nullptr);
}

void GameplayPolicies::ResolveCombatHits(
    world::World& world,
    time::SimStepView step,
    std::span<const gameplay::ResolvedHitEvent> hits,
    gameplay::GameplayPipeline& pipeline) noexcept {
    (void)step;

    for (const auto& hit : hits) {
        if (hit.attacker == world::kNullEntity || hit.defender == world::kNullEntity) {
            continue;
        }

        auto* lantern = world.TryGet<ResonanceLanternComponent>(hit.attacker);
        auto* amp = world.TryGet<AmplitudeAscensionComponent>(hit.attacker);
        auto* traversal = world.TryGet<TraversalMutationComponent>(hit.attacker);
        auto* action = world.TryGet<VeilActionStateComponent>(hit.attacker);
        auto* playerStats = world.TryGet<VeilCombatStatsComponent>(hit.attacker);

        auto* targetStats = world.TryGet<VeilCombatStatsComponent>(hit.defender);
        auto* targetWave = world.TryGet<StandingWaveComponent>(hit.defender);
        auto* bossOpt = world.TryGet<BossConductorComponent>(hit.defender);

        if (lantern != nullptr && amp != nullptr && traversal != nullptr &&
            playerStats != nullptr && targetStats != nullptr && targetWave != nullptr) {
            CombatTickSummary summary{};
            const EchoId lead = lantern->LeadEcho();
            const EchoSpec& spec = GetEchoSpec(lead);

            f32 baseDmg = spec.baseDamage;
            f32 baseBrk = spec.baseBreak;

            if (action != nullptr) {
                const auto& actSpec = GetActionSpec(action->currentAction);
                if (actSpec.motionValue > 0.0f) {
                    baseDmg *= actSpec.motionValue;
                    baseBrk *= actSpec.breakMultiplier;
                }
            }

            CombatSystem::ApplyEchoStrike(
                *lantern, *amp, *traversal, *playerStats, *targetStats, *targetWave,
                lead, baseDmg, baseBrk, 14.0f, 8.0f, summary);

            pipeline.EmitCue(gameplay::SimCue{
                .assetId = assets::AssetId::Hash("sfx/hit.wav"),
                .worldX = hit.contactX,
                .worldY = hit.contactY,
                .emitter = hit.defender,
                .magnitude_q16 = static_cast<u16>(std::min(65535.0f, summary.damageDealt * 100.0f)),
                .durationTicks = 6,
                .kind = gameplay::SimCueKind::AudioOneShot,
                .channel = 0,
                .userTag = static_cast<u16>(summary.triggeredReaction)
            });

            if (summary.triggeredReaction != WaveReactionId::None) {
                pipeline.EmitCue(gameplay::SimCue{
                    .assetId = assets::AssetId::Hash("fx/resonance_wave.epfb"),
                    .worldX = hit.contactX,
                    .worldY = hit.contactY,
                    .emitter = hit.defender,
                    .magnitude_q16 = 32768,
                    .durationTicks = 15,
                    .kind = gameplay::SimCueKind::CameraTraumaPulse,
                    .channel = 1,
                    .userTag = static_cast<u16>(summary.triggeredReaction)
                });
            }

            if (bossOpt != nullptr && bossOpt->activeTelegraph == BossTelegraphKind::GoldHarmonic) {
                auto* parryComp = world.TryGet<PhaseInversionCounterComponent>(hit.attacker);
                if (parryComp != nullptr && lantern->LeadEcho() == bossOpt->requiredCounterEcho) {
                    bossOpt->activeTelegraph = BossTelegraphKind::None;
                    bossOpt->telegraphTicksRemaining = 0;
                    ++bossOpt->phaseInversionsSuffered;
                    targetStats->posture = BossPosture::Submissive;
                    targetStats->submissiveTicksRemaining = 360;
                    amp->tier = AmplitudeTier::Crescendo;
                    amp->tierDecayTicksRemaining = CombatSystem::kAmplitudeDecayTicks;
                    lantern->harmonyGauge = std::min(100.0f, lantern->harmonyGauge + 50.0f);
                    parryComp->phaseCounterSucceeded = true;
                    ++parryComp->totalCountersLanded;

                    pipeline.EmitCue(gameplay::SimCue{
                        .assetId = assets::AssetId::Hash("sfx/phase_inversion_counter.wav"),
                        .worldX = hit.contactX,
                        .worldY = hit.contactY,
                        .emitter = hit.attacker,
                        .magnitude_q16 = 65535,
                        .durationTicks = 30,
                        .kind = gameplay::SimCueKind::CameraTraumaPulse,
                        .channel = 2,
                        .userTag = 0xAAAA
                    });
                }
            }
        }
    }
}

gameplay::GameplayPolicyTable GameplayPolicies::CreatePolicyTable() noexcept {
    return gameplay::GameplayPolicyTable{
        .evaluateActor = &GameplayPolicies::EvaluateActor,
        .resolveCombatHits = &GameplayPolicies::ResolveCombatHits
    };
}

} // namespace en::veil

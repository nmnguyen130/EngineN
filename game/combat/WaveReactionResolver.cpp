#include "game/combat/WaveReactionResolver.h"

#include <algorithm>
#include <cmath>

namespace vots {

namespace {

void ApplyDirectHpDamageToTarget(
    en::DamageReceiver2D& recv,
    const BreakAndCcComp* breakComp,
    i32 baseHpDamage) noexcept {
    if (baseHpDamage <= 0 || recv.HasFlag(en::ReceiverFlags::Dead)) return;
    const i32 finalDmg = CombatSystem::ComputeModifiedHpDamage(baseHpDamage, breakComp);
    recv.hp -= finalDmg;
    if (recv.hp <= 0) {
        recv.hp = 0;
        recv.AddFlags(en::ReceiverFlags::Dead);
    }
}

} // namespace

u32 WaveReactionResolver::GatherLivingEnemiesInRadius(
    en::World& world,
    Vec2 center,
    f32 radiusMeters,
    EntityId excludeEntity,
    std::span<EntityId> outTargets) noexcept {
    struct Candidate {
        EntityId id{kNullEntity};
        f32 distSq{0.0f};
    };
    std::array<Candidate, 32> candidates{};
    u32 candCount = 0U;
    const f32 rSq = radiusMeters * radiusMeters;

    for (auto [id, recv, breakComp] : world.View<en::DamageReceiver2D, BreakAndCcComp>()) {
        (void)breakComp;
        if (id == excludeEntity) continue;
        if (recv.TeamId() == 1U || recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
        if (const auto* ai = world.TryGet<EnemyAiComp>(id)) {
            if (ai->phaseTransitionTicks > 0U) continue;
        }

        const en::WorldTransform2D& xf = world.GetWorldByIndex(id.index);
        const f32 dx = xf.x - center.x;
        const f32 dy = xf.y - center.y;
        const f32 dSq = dx * dx + dy * dy;
        if (dSq <= rSq && candCount < candidates.size()) {
            candidates[candCount++] = Candidate{id, dSq};
        }
    }

    std::sort(candidates.begin(), candidates.begin() + candCount,
              [](const Candidate& a, const Candidate& b) noexcept {
                  if (std::abs(a.distSq - b.distSq) > 1e-5f) return a.distSq < b.distSq;
                  return a.id.index < b.id.index;
              });

    const u32 count = std::min<u32>(candCount, static_cast<u32>(outTargets.size()));
    for (u32 i = 0U; i < count; ++i) {
        outTargets[i] = candidates[i].id;
    }
    return count;
}

void WaveReactionResolver::ApplySignatureReactionPayload(
    EntityId primaryTargetId,
    const WaveReactionSpec& reactionSpec,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport& ioReport) noexcept {
    auto* breakComp = world.TryGet<BreakAndCcComp>(primaryTargetId);
    auto* markComp = world.TryGet<ResonanceMarkComp>(primaryTargetId);
    const en::WorldTransform2D& primaryXf = world.GetWorldByIndex(primaryTargetId.index);
    const Vec2 primaryPos{primaryXf.x, primaryXf.y};

    en::SimCue reactCue{};
    reactCue.kind = en::SimCueKind::AudioOneShot;
    reactCue.assetId = en::AssetId::Hash("sfx_reaction_burst");
    reactCue.worldX = primaryPos.x;
    reactCue.worldY = primaryPos.y;
    reactCue.emitter = primaryTargetId;
    pipeline.EmitCue(reactCue);

    switch (reactionSpec.id) {
        case WaveReactionId::MeltShock:
            if (breakComp != nullptr) {
                breakComp->fractureTicks = kFractureDurationTicks;
            }
            break;

        case WaveReactionId::PiezoQuake:
            if (breakComp != nullptr) {
                CombatSystem::ApplyCrowdControl(
                    primaryTargetId, ActionLockKind::Stun, kPiezoQuakeStunTicks, true, world, *breakComp);
            }
            break;

        case WaveReactionId::SteamCavitation: {
            if (breakComp != nullptr &&
                breakComp->category == TargetCategory::Standard &&
                !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                if (auto* ms = world.TryGet<en::MotionStack2D>(primaryTargetId)) {
                    ms->AddKnockbackImpulse(2.5f, 0.0f);
                }
            }
            std::array<EntityId, 2> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 1.75f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secRecv != nullptr) {
                    ApplyDirectHpDamageToTarget(*secRecv, secBreak, 8);
                    ++ioReport.secondaryTargetsHit;
                }
                if (secBreak != nullptr &&
                    secBreak->category == TargetCategory::Standard &&
                    !secBreak->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                    const en::WorldTransform2D& sxf = world.GetWorldByIndex(secId.index);
                    const f32 dirSign = (sxf.x >= primaryPos.x) ? 1.0f : -1.0f;
                    if (auto* sms = world.TryGet<en::MotionStack2D>(secId)) {
                        sms->AddKnockbackImpulse(dirSign * 2.5f, 0.0f);
                    }
                }
            }
            break;
        }

        case WaveReactionId::SuperconductorGale: {
            std::array<EntityId, 2> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.5f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secRecv != nullptr) {
                    ApplyDirectHpDamageToTarget(*secRecv, secBreak, 9);
                    ++ioReport.secondaryTargetsHit;
                }
                if (secBreak != nullptr) {
                    (void)CombatSystem::ApplyBreakDamage(secId, 4U, world, *secBreak);
                }
            }
            break;
        }

        case WaveReactionId::PyroCyclone: {
            std::array<EntityId, 4> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.5f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secRecv != nullptr) {
                    ApplyDirectHpDamageToTarget(*secRecv, secBreak, 12);
                    ++ioReport.secondaryTargetsHit;
                }
            }
            const f32 rSq = 2.5f * 2.5f;
            for (auto [projId, proj] : world.View<ProjectileComp>()) {
                if (ioReport.projectilesCleared >= 3U) break;
                if (proj.active == 0U || proj.protectedFromClear != 0U) continue;
                const en::WorldTransform2D& pxf = world.GetWorldByIndex(projId.index);
                const f32 dx = pxf.x - primaryPos.x;
                const f32 dy = pxf.y - primaryPos.y;
                if (dx * dx + dy * dy <= rSq) {
                    proj.active = 0U;
                    (void)world.DeferDestroy(projId);
                    ++ioReport.projectilesCleared;
                }
            }
            break;
        }

        case WaveReactionId::TectonicShatter:
            if (breakComp != nullptr) {
                CombatSystem::ApplyCrowdControl(
                    primaryTargetId, ActionLockKind::MicroStagger, kTectonicMicroStaggerTicks, true, world, *breakComp);
                if (breakComp->HasRuleFlag(BreakAndCcComp::kFlagBreakableGuard) &&
                    breakComp->HasRuleFlag(BreakAndCcComp::kFlagGuardActive)) {
                    breakComp->flags &= static_cast<u8>(~BreakAndCcComp::kFlagGuardActive);
                    breakComp->guardBrokenTicks = kGuardBrokenLockoutTicks;
                }
            }
            break;

        case WaveReactionId::EclipseSingularity: {
            std::array<EntityId, 4> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 3.5f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secBreak != nullptr &&
                    secBreak->category == TargetCategory::Standard &&
                    !secBreak->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
                    secBreak->pullTargetX = primaryPos.x;
                    secBreak->pullTargetY = primaryPos.y;
                    secBreak->pullTicks = kEclipsePullDurationTicks;
                    ++ioReport.secondaryTargetsHit;
                }
            }
            break;
        }

        case WaveReactionId::GravitonQuake: {
            if (breakComp != nullptr &&
                breakComp->category == TargetCategory::Standard &&
                !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                if (auto* motor = world.TryGet<en::CharacterMotor2D>(primaryTargetId)) {
                    if (!motor->IsGrounded()) {
                        motor->vy = std::min(motor->vy, -18.0f);
                    }
                }
            }
            std::array<EntityId, 4> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.0f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secRecv != nullptr) {
                    ApplyDirectHpDamageToTarget(*secRecv, secBreak, 8);
                    ++ioReport.secondaryTargetsHit;
                }
            }
            break;
        }

        case WaveReactionId::ChronoOverdrive:
            if (markComp != nullptr) {
                markComp->chronoDelayTicks = kChronoOverdriveDelayTicks;
                markComp->chronoPendingDamage = 10;
            }
            break;

        case WaveReactionId::SanguineBoil: {
            std::array<EntityId, 2> secTargets{};
            const u32 count = GatherLivingEnemiesInRadius(world, primaryPos, 2.0f, primaryTargetId, secTargets);
            for (u32 i = 0U; i < count; ++i) {
                const EntityId secId = secTargets[i];
                auto* secRecv = world.TryGet<en::DamageReceiver2D>(secId);
                const auto* secBreak = world.TryGet<BreakAndCcComp>(secId);
                if (secRecv != nullptr) {
                    ApplyDirectHpDamageToTarget(*secRecv, secBreak, 8);
                    ++ioReport.secondaryTargetsHit;
                }
            }
            break;
        }

        case WaveReactionId::None:
            break;
    }
}

bool WaveReactionResolver::ExecuteUnisonBurst(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    if (!world.IsAlive(playerId)) return false;
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    if (playerState == nullptr || playerState->harmony < kUnisonHarmonyCost) {
        return false;
    }

    playerState->harmony = static_cast<u8>(playerState->harmony - kUnisonHarmonyCost);
    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const Vec2 center{pxf.x, pxf.y};

    std::array<EntityId, 5> targets{};
    const u32 count = GatherLivingEnemiesInRadius(world, center, 3.0f, playerId, targets);
    for (u32 i = 0U; i < count; ++i) {
        const EntityId tid = targets[i];
        auto* recv = world.TryGet<en::DamageReceiver2D>(tid);
        auto* breakComp = world.TryGet<BreakAndCcComp>(tid);
        if (recv != nullptr) {
            ApplyDirectHpDamageToTarget(*recv, breakComp, 18);
        }
        if (breakComp != nullptr) {
            (void)CombatSystem::ApplyBreakDamage(tid, 15U, world, *breakComp);
            if (breakComp->category == TargetCategory::Standard) {
                CombatSystem::ApplyCrowdControl(
                    tid, ActionLockKind::MicroStagger, kUnisonInterruptTicks, true, world, *breakComp);
            }
        }
        if (outReport != nullptr) {
            ++outReport->secondaryTargetsHit;
        }
    }

    en::SimCue cue{};
    cue.kind = en::SimCueKind::AudioOneShot;
    cue.assetId = en::AssetId::Hash("sfx_unison_pulse");
    cue.worldX = center.x;
    cue.worldY = center.y;
    cue.emitter = playerId;
    pipeline.EmitCue(cue);
    return true;
}

bool WaveReactionResolver::ExecuteSacredChord(
    EntityId playerId,
    en::World& world,
    en::GameplayPipeline& pipeline,
    CombatResolutionReport* outReport) noexcept {
    if (!world.IsAlive(playerId)) return false;
    auto* playerState = world.TryGet<PlayerStateComp>(playerId);
    if (playerState == nullptr || playerState->harmony < kSacredChordHarmonyCost) {
        return false;
    }

    const SacredChordSpec* chord = FindMatchingSacredChord(playerState->deck);
    if (chord == nullptr) {
        return false;
    }

    playerState->harmony = 0U;
    if (outReport != nullptr) {
        outReport->chordTriggered = chord->id;
    }

    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const Vec2 center{pxf.x, pxf.y};

    std::array<EntityId, 5> targets{};
    const u32 maxT = std::min<u32>(chord->maxTargets, static_cast<u32>(targets.size()));
    const u32 count = GatherLivingEnemiesInRadius(
        world, center, chord->radiusMeters, playerId, std::span<EntityId>{targets.data(), maxT});

    for (u32 i = 0U; i < count; ++i) {
        const EntityId tid = targets[i];
        auto* recv = world.TryGet<en::DamageReceiver2D>(tid);
        auto* breakComp = world.TryGet<BreakAndCcComp>(tid);

        if (chord->id == SacredChordId::EclipseOfEternity) {
            if (recv != nullptr) {
                for (u32 s = 0U; s < 3U; ++s) {
                    ApplyDirectHpDamageToTarget(*recv, breakComp, 8);
                }
            }
        } else if (recv != nullptr) {
            ApplyDirectHpDamageToTarget(*recv, breakComp, chord->totalHpDamage);
        }

        if (breakComp != nullptr) {
            (void)CombatSystem::ApplyBreakDamage(tid, chord->breakDamage, world, *breakComp);

            switch (chord->id) {
                case SacredChordId::ElementalCataclysm:
                    if (breakComp->category == TargetCategory::Standard &&
                        !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant)) {
                        const en::WorldTransform2D& txf = world.GetWorldByIndex(tid.index);
                        const f32 dirSign = (txf.x >= center.x) ? 1.0f : -1.0f;
                        if (auto* ms = world.TryGet<en::MotionStack2D>(tid)) {
                            ms->AddKnockbackImpulse(dirSign * 2.0f, 0.0f);
                        }
                    }
                    break;
                case SacredChordId::CryoTempestProtocol:
                    CombatSystem::ApplySlowEffect(*breakComp, 40U, kCryoTempestSlowTicks);
                    break;
                case SacredChordId::EclipseOfEternity:
                    if (breakComp->category == TargetCategory::Standard) {
                        CombatSystem::ApplyCrowdControl(
                            tid, ActionLockKind::Stun, kEclipseChordStunTicks, true, world, *breakComp);
                    }
                    break;
                case SacredChordId::SingularityOverdrive:
                    if (breakComp->category == TargetCategory::Standard &&
                        !breakComp->HasRuleFlag(BreakAndCcComp::kFlagDisplacementResistant | BreakAndCcComp::kFlagAnchored)) {
                        breakComp->pullTargetX = center.x;
                        breakComp->pullTargetY = center.y;
                        breakComp->pullTicks = kSingularityPullTicks;
                    }
                    break;
                case SacredChordId::None:
                    break;
            }
        }

        if (outReport != nullptr) {
            ++outReport->secondaryTargetsHit;
        }
    }

    if (chord->id == SacredChordId::ElementalCataclysm) {
        const f32 rSq = chord->radiusMeters * chord->radiusMeters;
        for (auto [terId, ter] : world.View<DestructibleTerrainComp>()) {
            if (ter.broken != 0U) continue;
            const en::WorldTransform2D& txf = world.GetWorldByIndex(terId.index);
            const f32 dx = txf.x - center.x;
            const f32 dy = txf.y - center.y;
            if (dx * dx + dy * dy <= rSq) {
                ter.durability -= chord->totalHpDamage;
                if (ter.durability <= 0) {
                    ter.durability = 0;
                    ter.broken = 1U;
                    if (ter.unlockedWorldFlagOnBreak != 0U) {
                        pipeline.SetWorldFlag(ter.unlockedWorldFlagOnBreak, true);
                    }
                    (void)world.DeferDestroy(terId);
                    if (outReport != nullptr) {
                        ++outReport->destructiblesBroken;
                    }
                }
            }
        }
    }

    en::SimCue cue{};
    cue.kind = en::SimCueKind::AudioOneShot;
    cue.assetId = en::AssetId::Hash("sfx_sacred_chord");
    cue.worldX = center.x;
    cue.worldY = center.y;
    cue.emitter = playerId;
    pipeline.EmitCue(cue);
    return true;
}

} // namespace vots

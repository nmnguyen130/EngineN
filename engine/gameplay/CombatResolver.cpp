#include "gameplay/GameplaySystem.h"

namespace en {

void GameplaySystem::ResolveCombat(World& world, const TickStep& step) noexcept {
    if (!m_cues || !step) return;
    ++m_tickCount;

    auto& combatPool = world.GetPool<CombatBox2D>();
    if (combatPool.Size() < 2U) return;

    const auto entities = combatPool.Entities();
    auto       boxes    = combatPool.Components();
    const u64  regMask  = world.RegisteredPoolsMask();
    const bool hasMut   = (regMask & (1ULL << WorldMutation2D::kTypeId)) != 0ULL;
    const bool hasFlash = (regMask & (1ULL << HitFlash2D::kTypeId)) != 0ULL;

    const auto getFacing = [&](EntityId id) noexcept -> i8 {
        return world.Has<CharacterController2D>(id)
            ? world.GetPool<CharacterController2D>().DirectAtEntityIndex(id.index).facing
            : static_cast<i8>(1);
    };

    const auto applyHitstop = [&](EntityId a, EntityId b, Vec2 pt, u8 ticks) noexcept {
        if (ticks == 0U) return;
        if (world.Has<StateMachine2D>(a)) {
            world.GetPool<StateMachine2D>().DirectAtEntityIndex(a.index).hitstopTicks = ticks;
        }
        if (world.Has<StateMachine2D>(b)) {
            world.GetPool<StateMachine2D>().DirectAtEntityIndex(b.index).hitstopTicks = ticks;
        }
        EmitCue({
            .position      = pt,
            .emitter       = a,
            .durationTicks = ticks,
            .kind          = CueKind::Hitstop,
            .channel       = static_cast<TimeChannel>(world.GetTimeChannelByIndex(b.index)),
        });
    };

    for (usize i = 0U; i < entities.size() && m_hitCount < m_cfg.maxCombatHitsPerTick; ++i) {
        CombatBox2D& attBox = boxes[i];
        if (!HasFlag(attBox.flags, CombatFlags::HitboxActive)) continue;

        const EntityId attId = entities[i];
        if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(attId.index)))) continue;

        const WorldTransform2D&     attWt   = world.GetWorldByIndex(attId.index);
        const PrevWorldSnapshot2D&  attPrev = world.GetPrevWorldByIndex(attId.index);
        const i8                    attFace = getFacing(attId);
        // Continuous swept hitbox hull between previous and current world positions prevents high-speed tunneling.
        const Rect                  hitRect = attBox.SweptWorldHitbox({attPrev.x, attPrev.y}, {attWt.x, attWt.y}, attFace);
        const u16                   swingTag =
            static_cast<u16>((attBox.swingId << 8U) ^ (attId.index & 0xFFU));

        for (usize j = 0U; j < entities.size() && m_hitCount < m_cfg.maxCombatHitsPerTick; ++j) {
            if (i == j) continue;
            const EntityId defId = entities[j];
            if (step.IsFrozen(static_cast<TimeChannel>(world.GetTimeChannelByIndex(defId.index)))) continue;

            CombatBox2D&            defBox = boxes[j];
            const WorldTransform2D& defWt  = world.GetWorldByIndex(defId.index);

            if (j > i && HasFlag(attBox.flags, CombatFlags::CanClash) &&
                HasFlag(defBox.flags, CombatFlags::HitboxActive) &&
                HasFlag(defBox.flags, CombatFlags::CanClash)) {
                const PrevWorldSnapshot2D& defPrev    = world.GetPrevWorldByIndex(defId.index);
                const Rect                 defHitRect = defBox.SweptWorldHitbox(
                    {defPrev.x, defPrev.y}, {defWt.x, defWt.y}, getFacing(defId));
                if (hitRect.Overlaps(defHitRect)) {
                    attBox.EndSwing();
                    defBox.EndSwing();
                    const Vec2 mid = hitRect.Intersection(defHitRect).Center();
                    const u8   hs  = std::max(attBox.hitstopTicks, defBox.hitstopTicks);
                    CombatHitEvent ev{
                        .attacker     = attId,
                        .defender     = defId,
                        .point        = mid,
                        .swingTag     = swingTag,
                        .outcome      = HitOutcome::Clashed,
                        .hitstopTicks = hs,
                    };
                    m_hits[m_hitCount++] = ev;
                    ++m_totalHits;
                    EmitCue({
                        .position      = mid,
                        .emitter       = attId,
                        .magnitude     = 1.4f,
                        .durationTicks = 16U,
                        .userTag       = 10U,
                        .kind          = CueKind::Effect,
                    });
                    applyHitstop(attId, defId, mid, hs);
                    if (m_callbacks.onCombatHit) m_callbacks.onCombatHit(ev, world, *this);
                    break;
                }
            }

            if (!HasFlag(defBox.flags, CombatFlags::HurtboxActive)) continue;

            Health2D* defHealth = world.TryGet<Health2D>(defId);
            if (defHealth) {
                if (!defHealth->IsAlive() || defHealth->IsInvulnerable()) continue;
                if (defHealth->lastSwingTag == swingTag && swingTag != 0U) continue;
                if ((attBox.targetTeamMask & (1U << (defHealth->team & 7U))) == 0U) continue;
                if (const Health2D* attHealth = world.TryGet<Health2D>(attId)) {
                    if (attHealth->team != 0U && attHealth->team == defHealth->team) continue;
                }
            }

            const Rect hurtRect = defBox.WorldHurtbox({defWt.x, defWt.y}, getFacing(defId));
            if (!hitRect.Overlaps(hurtRect)) continue;

            const Vec2 kb{attBox.knockback.x * static_cast<f32>(attFace), attBox.knockback.y};
            CombatHitEvent ev{
                .attacker     = attId,
                .defender     = defId,
                .point        = hitRect.Intersection(hurtRect).Center(),
                .knockback    = kb,
                .swingTag     = swingTag,
                .hitstopTicks = attBox.hitstopTicks,
            };

            if (defHealth) {
                defHealth->lastSwingTag = swingTag;
                if (HasFlag(defHealth->flags, HealthFlags::Parrying)) {
                    ev.outcome = HitOutcome::Parried;
                    if (auto* attCc = world.TryGet<CharacterController2D>(attId)) {
                        attCc->ApplyKnockback({-kb.x * m_cfg.parryRecoilScale, 0.0f});
                    }
                } else if (HasFlag(defHealth->flags, HealthFlags::SuperArmor) &&
                           defHealth->poise > attBox.poiseDamage) {
                    ev.outcome       = HitOutcome::ArmorAbsorbed;
                    ev.damage        = attBox.damage;
                    defHealth->poise = static_cast<u8>(defHealth->poise - attBox.poiseDamage);
                    ev.killed        = defHealth->ApplyDamage(attBox.damage) ? 1U : 0U;
                } else {
                    ev.outcome             = HitOutcome::Damaged;
                    ev.damage              = attBox.damage;
                    defHealth->iframeTicks = std::max(defHealth->iframeTicks, defHealth->autoIframeTicks);
                    if (defHealth->poise <= attBox.poiseDamage) {
                        defHealth->poise     = defHealth->maxPoise;
                        defHealth->stunTicks = m_cfg.defaultStunTicks;
                        ev.poiseBroken       = 1U;
                    } else {
                        defHealth->poise = static_cast<u8>(defHealth->poise - attBox.poiseDamage);
                    }
                    ev.killed = defHealth->ApplyDamage(attBox.damage) ? 1U : 0U;
                }
            } else {
                ev.outcome = HitOutcome::Damaged;
                ev.damage  = attBox.damage;
            }

            // Opt-in HitFlash2D component trigger and spark cue emission.
            if (hasFlash && world.Has<HitFlash2D>(defId)) {
                HitFlash2D& hf = world.GetPool<HitFlash2D>().DirectAtEntityIndex(defId.index);
                if (ev.outcome == HitOutcome::Damaged || ev.outcome == HitOutcome::ArmorAbsorbed) {
                    hf.Trigger();
                }
                if (hf.emitSparks != 0U) {
                    EmitCue({
                        .position      = ev.point,
                        .emitter       = defId,
                        .magnitude     = 1.1f,
                        .durationTicks = 14U,
                        .userTag       = 8U,
                        .kind          = CueKind::Effect,
                    });
                }
            }

            if (ev.outcome == HitOutcome::Damaged) {
                if (auto* defCc = world.TryGet<CharacterController2D>(defId)) {
                    defCc->ApplyKnockback(kb);
                } else if (auto* defBody = world.TryGet<PhysicsBody2D>(defId)) {
                    defBody->ApplyImpulse(kb);
                }
            }

            if (HasFlag(attBox.flags, CombatFlags::PogoStrike)) {
                if (auto* attCc = world.TryGet<CharacterController2D>(attId)) {
                    attCc->RequestPogo();
                }
            }

            if (ev.killed != 0U && hasMut && world.Has<WorldMutation2D>(defId)) {
                const WorldMutation2D& wm = world.GetPool<WorldMutation2D>().DirectAtEntityIndex(defId.index);
                if (wm.recordOnDeath != 0U) {
                    if (wm.guid != 0ULL) (void)RecordMutation(wm.guid, 1U);
                    if (wm.flagId < kMaxFlags) SetWorldFlag(wm.flagId, true);
                }
            }

            m_hits[m_hitCount++] = ev;
            ++m_totalHits;
            applyHitstop(attId, defId, ev.point, ev.hitstopTicks);
            if (m_callbacks.onCombatHit) m_callbacks.onCombatHit(ev, world, *this);
        }
    }
}

} // namespace en

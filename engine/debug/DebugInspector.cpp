#include "debug/DebugInspector.h"
#include <algorithm>
#include <cmath>

namespace en {

bool DebugInspector::Init(AlignedArena& arena) noexcept {
    m_history = arena.Carve<ActorTickRecord>(kFlightHistoryTicks);
    Clear();
    return !m_history.empty();
}

void DebugInspector::Clear() noexcept {
    for (ActorTickRecord& r : m_history) r = ActorTickRecord{};
    m_writeCursor = 0;
    m_validRecords = 0;
    m_scrubOffset = 0;
}

void DebugInspector::RecordActorTick(
    const World& world, u64 tickIndex, const TickInput& tickInput,
    const ActionBuffer& actionBuffer) noexcept {
    if (m_history.empty()) return;
    if (!world.IsAlive(m_pinnedEntity)) {
        world.Each<KinematicMotor2D>([&](EntityId id, const KinematicMotor2D&) noexcept {
            if (!world.IsAlive(m_pinnedEntity)) m_pinnedEntity = id;
        });
    }
    if (!world.IsAlive(m_pinnedEntity)) return;

    ActorTickRecord rec{};
    rec.tickIndex = static_cast<u32>(tickIndex);
    rec.entityId = m_pinnedEntity;
    const WorldTransform2D& wt = world.GetWorldByIndex(m_pinnedEntity.index);
    rec.worldX = wt.x;
    rec.worldY = wt.y;
    if (const KinematicMotor2D* m = world.TryGet<KinematicMotor2D>(m_pinnedEntity)) {
        rec.velX = m->vx;
        rec.velY = m->vy;
        rec.contactFlags = static_cast<u8>(m->contacts);
    }
    if (const StateMachine2D* fsm = world.TryGet<StateMachine2D>(m_pinnedEntity)) {
        rec.stateId = fsm->stateId;
        rec.stateTicks = fsm->stateTicks;
    }
    if (const DamageReceiver2D* dmg = world.TryGet<DamageReceiver2D>(m_pinnedEntity)) {
        rec.hp = dmg->hp;
        rec.iframeTicks = static_cast<u8>( std::min<u16>(dmg->iframeTicks, 255));
    }
    if (const HitboxRig2D* rig = world.TryGet<HitboxRig2D>(m_pinnedEntity)) {
        rec.hitboxActive = HasFlag(rig->rigFlags, HitboxRigFlags::HitboxActive) ? 1 : 0;
    }
    rec.inputHeld = tickInput.held;
    rec.inputPressed = tickInput.pressed;
    rec.inputReleased = tickInput.released;
    rec.actionBufferMask = actionBuffer.PendingPressMask();

    m_history[m_writeCursor] = rec;
    m_writeCursor = static_cast<u8>((m_writeCursor + 1U) % kFlightHistoryTicks);
    if (m_validRecords < kFlightHistoryTicks) ++m_validRecords;
}

EntityId DebugInspector::PickEntityAtWorldPos(const World& world, Vec2 worldPos) noexcept {
    EntityId best = kNullEntity;
    f32 bestDistSq = 1.5f * 1.5f;
    world.Each<KinematicMotor2D>([&](EntityId id, const KinematicMotor2D& motor) noexcept {
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        const Rect r = motor.bounds.WorldRect({wt.x, wt.y});
        if (r.Contains(worldPos)) {
            const f32 d2 = (r.Center() - worldPos).LengthSq();
            if (d2 < bestDistSq) {
                bestDistSq = d2;
                best = id;
            }
        }
    });
    if (best.IsValid()) m_pinnedEntity = best;
    return best;
}

void DebugInspector::EmitWorldAndInspectorOverlays(
    OverlayFlags flags, const DebugFrameContext& ctx,
    DebugBatcher& worldBatcher, DebugBatcher& hudBatcher) const noexcept {
    if (!ctx.world) return;
    const World& world = *ctx.world;

    if (HasFlag(flags, OverlayFlags::KinematicBounds)) {
        world.Each<KinematicMotor2D>([&](EntityId id, const KinematicMotor2D& motor) noexcept {
            const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
            const Rect r = motor.bounds.WorldRect({wt.x, wt.y});
            const Color c = motor.Grounded()
                                ? Color::FromRGBA8(60, 240, 120, 200)
                                : Color::FromRGBA8(240, 200, 60, 200);
            worldBatcher.PushBoxOutline(r, 0.03f, c);
            if (HasFlag(flags, OverlayFlags::VelocityVectors)) {
                worldBatcher.PushArrow({wt.x, wt.y}, {wt.x + motor.vx * 0.15f, wt.y + motor.vy * 0.15f}, 0.12f, 0.03f, Color::FromRGBA8(80, 190, 255, 220));
            }
        });
    }

    if (HasFlag(flags, OverlayFlags::CombatHitboxes)) {
        world.Each<HitboxRig2D>([&](EntityId id, const HitboxRig2D& rig) noexcept {
            const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
            if (HasFlag(rig.rigFlags, HitboxRigFlags::HurtboxActive)) {
                worldBatcher.PushBoxOutline(
                    rig.hurtbox.WorldRect({wt.x, wt.y}), 0.03f, Color::FromRGBA8(80, 160, 255, 180));
            }
            if (HasFlag(rig.rigFlags, HitboxRigFlags::HitboxActive)) {
                worldBatcher.PushBoxOutline(
                    rig.primaryHitbox.WorldRect({wt.x, wt.y}), 0.045f, Color::FromRGBA8(255, 60, 60, 240));
            }
        });
    }

    if (HasFlag(flags, OverlayFlags::AudioEmitters)) {
        for (const AudioEmitterView& em : ctx.audioEmitters) {
            worldBatcher.PushCircleOutline(
                {em.worldX, em.worldY}, em.maxDist * 0.25f, 16, 0.03f, Color::FromRGBA8(190, 120, 255, 180));
        }
    }

    if (HasFlag(flags, OverlayFlags::EntityInspector) && m_validRecords > 0) {
        const u8 idx = static_cast<u8>((m_writeCursor + kFlightHistoryTicks - 1U -
                                        std::min<u8>(m_scrubOffset, m_validRecords - 1U)) % kFlightHistoryTicks);
        const ActorTickRecord& rec = m_history[idx];
        hudBatcher.PushSolidRect({16.0f, 120.0f, 240.0f, 76.0f}, Color::FromRGBA8(14, 18, 28, 215));
        hudBatcher.PushText({24.0f, 128.0f}, "ENTITY INSPECTOR", Color::FromRGBA8(90, 220, 255, 255));
        char buf[64]{};
        char* p = buf;
        char* end = buf + sizeof(buf);
        std::memcpy(p, "HP:", 3);
        p = DebugBatcher::FormatI64(p + 3, end, rec.hp);
        std::memcpy(p, " ST:", 4);
        p = DebugBatcher::FormatI64(p + 4, end, rec.stateId);
        hudBatcher.PushText({24.0f, 148.0f}, std::string_view(buf, static_cast<usize>(p - buf)), Color::FromRGBA8(230, 240, 255, 255));
    }
}

u64 DebugInspector::ComputeAuditHash() const noexcept {
    return Mix64(static_cast<u64>(m_pinnedEntity.Packed()) |
                 (static_cast<u64>(m_validRecords) << 32U));
}

} // namespace en

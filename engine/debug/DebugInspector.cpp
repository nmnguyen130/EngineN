#include "debug/DebugInspector.h"
#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "physics/PhysicsSystem.h"
#include "physics/PhysicsTypes.h"
#include "physics/SpatialIndex2D.h"
#include "world/World.h"

namespace en {

void DebugInspector::Record(
    const World& world,
    u32 tick,
    const TickInput* input,
    const ActionBuffer* buffer) noexcept {
    if (!m_hist || !m_pin.IsValid() || !world.IsAlive(m_pin)) return;

    const u16 idx = m_pin.index;
    const u64 mask = world.RegisteredPoolsMask() & world.GetComponentMaskByIndex(idx);
    const WorldTransform2D& wt = world.GetWorldByIndex(idx);

    ActorSample rec{};
    rec.tick = tick;
    rec.entity = idx;
    rec.x = wt.x;
    rec.y = wt.y;

    if ((mask & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
        const CharacterMotor2D& m = world.GetPool<CharacterMotor2D>().DirectAtEntityIndex(idx);
        rec.vx = m.vx;
        rec.vy = m.vy;
        rec.contacts = static_cast<u8>(m.contacts);
    } else if ((mask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
        const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(idx);
        rec.vx = rb.vx;
        rec.vy = rb.vy;
    }

    if ((mask & MakeComponentMask<StateMachine2D>()) != 0ULL) {
        rec.state = world.GetPool<StateMachine2D>().DirectAtEntityIndex(idx).stateId;
    }
    if ((mask & MakeComponentMask<DamageReceiver2D>()) != 0ULL) {
        const DamageReceiver2D& dr = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(idx);
        rec.hp = dr.hp;
        rec.iframe = static_cast<u8>(std::min<u16>(255U, dr.iframeTicks));
    }
    if (input) {
        rec.held = static_cast<u8>(input->held & 0xFFU);
        rec.pressed = static_cast<u8>(input->pressed & 0xFFU);
        rec.ax = static_cast<i8>(std::clamp(input->Axis(0) * 127.0f, -127.0f, 127.0f));
        rec.ay = static_cast<i8>(std::clamp(input->Axis(1) * 127.0f, -127.0f, 127.0f));
    }
    if (buffer) {
        u8 bits = 0U;
        for (u8 a = 0U; a < 8U; ++a) {
            if (buffer->Peek(a)) bits = static_cast<u8>(bits | (1U << a));
        }
        rec.buffer = bits;
    }

    const u16 slot = m_write & (kFlightTicks - 1U);
    m_hist[slot] = rec;
    m_write = static_cast<u16>((slot + 1U) & (kFlightTicks - 1U));
    if (m_valid < kFlightTicks) ++m_valid;
}

const ActorSample* DebugInspector::Current() const noexcept {
    if (!m_hist || m_valid == 0U) return nullptr;
    const u8 off = std::min<u8>(m_scrub, static_cast<u8>(m_valid - 1U));
    const u32 i = (static_cast<u32>(m_write) + kFlightTicks - 1U - off) & (kFlightTicks - 1U);
    return &m_hist[i];
}

EntityId DebugInspector::Pick(const World& world, Vec2 pos, f32 radius) noexcept {
    EntityId best = kNullEntity;
    f32 bestSq = radius * radius;
    const u64 reg = world.RegisteredPoolsMask();
    const auto words = world.AliveWords();

    for (usize w = 0U; w < words.size(); ++w) {
        u64 bits = words[w];
        while (bits) {
            const u32 b = static_cast<u32>(std::countr_zero(bits));
            bits &= bits - 1ULL;
            const u16 eIdx = static_cast<u16>((w << 6U) + b);
            if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) continue;

            const EntityId e = world.EntityAtSlot(eIdx);
            if (!e.IsValid()) continue;

            const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
            const u64 mask = reg & world.GetComponentMaskByIndex(eIdx);
            f32 cx = wt.x, cy = wt.y, hw = 0.0f, hh = 0.0f;

            if ((mask & MakeComponentMask<Collider2D>()) != 0ULL) {
                const Collider2D& col = world.GetPool<Collider2D>().DirectAtEntityIndex(eIdx);
                if (col.shape != ShapeKind2D::Segment) {
                    const Vec2 c = wt.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
                    cx = c.x; cy = c.y;
                    hw = col.localBounds.HalfW() * std::abs(wt.scaleX);
                    hh = (col.shape == ShapeKind2D::Circle) ? hw : col.localBounds.HalfH() * std::abs(wt.scaleY);
                }
            } else if ((mask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
                const HitboxRig2D& hb = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(eIdx);
                cx = wt.x + hb.hurtbox.OffsetX();
                cy = wt.y + hb.hurtbox.OffsetY();
                hw = hb.hurtbox.HalfW();
                hh = hb.hurtbox.HalfH();
            }

            const f32 dx = std::max(0.0f, std::abs(pos.x - cx) - hw);
            const f32 dy = std::max(0.0f, std::abs(pos.y - cy) - hh);
            const f32 d2 = dx * dx + dy * dy;
            if (d2 <= bestSq) {
                bestSq = d2;
                best = e;
            }
        }
    }
    if (best.IsValid()) Pin(best);
    return best;
}

EntityId DebugInspector::PickScreen(
    const World& world, Vec2 screen, Viewport vp, const Camera2D& cam) noexcept {
    return Pick(world, cam.ToWorld(screen, vp), 1.25f);
}

void DebugInspector::Emit(
    Overlay flags, const FrameCtx& ctx, DebugDraw& worldDraw, DebugDraw& hud) const noexcept {
    if (!ctx.world) return;
    const World& world = *ctx.world;
    const u64 reg = world.RegisteredPoolsMask();

    if (HasFlag(flags, Overlay::Colliders) && (reg & MakeComponentMask<Collider2D>()) != 0ULL) {
        const auto& pool = world.GetPool<Collider2D>();
        const bool hasRb = (reg & MakeComponentMask<RigidBody2D>()) != 0ULL;
        for (u16 i = 0U; i < pool.Size(); ++i) {
            const EntityId e = pool.Entities()[i];
            const Collider2D& col = pool.ComponentAtDense(i);
            const WorldTransform2D& wt = world.GetWorldByIndex(e.index);

            Color wire{0.2f, 0.9f, 1.0f, 0.85f};
            if (col.IsSensor()) wire = {0.9f, 0.9f, 0.2f, 0.7f};
            else if (col.IsOneWay()) wire = {1.0f, 0.55f, 0.15f, 0.85f};
            else if (hasRb && (world.GetComponentMaskByIndex(e.index) & MakeComponentMask<RigidBody2D>()) != 0ULL) {
                const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(e.index);
                if (rb.IsSleeping()) wire = {0.25f, 0.55f, 0.55f, 0.7f};
                else if (rb.IsKinematic()) wire = {0.75f, 0.35f, 1.0f, 0.9f};
                else if (rb.IsDynamic()) wire = {0.15f, 0.95f, 0.45f, 0.9f};
            }

            const Vec2 c = wt.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
            const f32 hw = col.localBounds.HalfW() * std::abs(wt.scaleX);
            const f32 hh = col.localBounds.HalfH() * std::abs(wt.scaleY);

            switch (col.shape) {
            case ShapeKind2D::Aabb:
                worldDraw.Box({c.x - hw, c.y - hh, hw * 2.0f, hh * 2.0f}, 0.04f, wire);
                break;
            case ShapeKind2D::Circle:
                worldDraw.Circle(c.x, c.y, hw, 16U, 0.04f, wire);
                break;
            case ShapeKind2D::CapsuleY:
                worldDraw.Capsule(c.x, c.y, hw, hh, 0.04f, wire);
                break;
            case ShapeKind2D::Obb:
                worldDraw.Obb({c.x, c.y}, {hw, hh}, wt.cosRot, wt.sinRot, 0.04f, wire);
                break;
            case ShapeKind2D::Segment: {
                const Vec2 p0 = wt.TransformPoint(col.localBounds.SegmentP0());
                const Vec2 p1 = wt.TransformPoint(col.localBounds.SegmentP1());
                worldDraw.Line(p0.x, p0.y, p1.x, p1.y, 0.05f, wire);
                break;
            }
            }
        }
    }

    if (HasFlag(flags, Overlay::Velocity)) {
        auto vel = [&](EntityId e, f32 vx, f32 vy, Color col) noexcept {
            if (vx * vx + vy * vy <= 0.0025f) return;
            const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
            worldDraw.Arrow(wt.x, wt.y, wt.x + vx * 0.15f, wt.y + vy * 0.15f, 0.18f, 0.04f, col);
        };
        if ((reg & MakeComponentMask<RigidBody2D>()) != 0ULL) {
            const auto& pool = world.GetPool<RigidBody2D>();
            for (u16 i = 0U; i < pool.Size(); ++i)
                vel(pool.Entities()[i], pool.ComponentAtDense(i).vx, pool.ComponentAtDense(i).vy,
                    {1.0f, 0.55f, 0.1f, 0.9f});
        }
        if ((reg & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
            const auto& pool = world.GetPool<CharacterMotor2D>();
            for (u16 i = 0U; i < pool.Size(); ++i)
                vel(pool.Entities()[i], pool.ComponentAtDense(i).vx, pool.ComponentAtDense(i).vy,
                    {0.2f, 0.9f, 1.0f, 0.9f});
        }
    }

    if (HasFlag(flags, Overlay::Hitboxes) && (reg & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
        const bool hasMotion = (reg & MakeComponentMask<MotionStack2D>()) != 0ULL;
        const auto& pool = world.GetPool<HitboxRig2D>();
        for (u16 i = 0U; i < pool.Size(); ++i) {
            const EntityId e = pool.Entities()[i];
            const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
            const HitboxRig2D& hb = pool.ComponentAtDense(i);
            const f32 face = (hasMotion &&
                (world.GetComponentMaskByIndex(e.index) & MakeComponentMask<MotionStack2D>()) != 0ULL)
                ? static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(e.index).FacingSign())
                : 1.0f;

            auto box = [&](const CompactAabb2D& b, f32 thick, Color col) noexcept {
                const f32 cx = wt.x + b.OffsetX() * face;
                const f32 cy = wt.y + b.OffsetY();
                const f32 hw = b.HalfW(), hh = b.HalfH();
                worldDraw.Box({cx - hw, cy - hh, hw * 2.0f, hh * 2.0f}, thick, col);
            };
            if (HasFlag(hb.flags, HitboxFlags::HurtboxActive))
                box(hb.hurtbox, 0.04f, {0.1f, 0.95f, 0.35f, 0.85f});
            if (HasFlag(hb.flags, HitboxFlags::HitboxActive))
                box(hb.hitbox, 0.05f, {1.0f, 0.2f, 0.2f, 0.9f});
        }
    }

    if (HasFlag(flags, Overlay::Broadphase) && ctx.physics) {
        for (const SpatialBox2D& b : ctx.physics->Spatial().ActiveBoxes()) {
            worldDraw.Box({b.minX, b.minY, b.maxX - b.minX, b.maxY - b.minY}, 0.03f,
                          {0.4f, 0.4f, 1.0f, 0.35f});
        }
    }

    if (HasFlag(flags, Overlay::Contacts) && ctx.physics) {
        for (const ContactManifold2D& m : ctx.physics->ActiveManifolds()) {
            for (u8 pi = 0U; pi < m.pointCount && pi < 2U; ++pi) {
                const f32 px = m.pointX[pi];
                const f32 py = m.pointY[pi];
                worldDraw.Cross(px, py, 0.08f, 0.03f, {1.0f, 0.9f, 0.1f, 0.95f});
                worldDraw.Arrow(px, py, px + m.normalX * 0.25f, py + m.normalY * 0.25f,
                                0.1f, 0.03f, {1.0f, 0.6f, 0.1f, 0.9f});
            }
        }
    }

    if (HasFlag(flags, Overlay::Inspector)) {
        if (const ActorSample* s = Current()) {
            worldDraw.Cross(s->x, s->y, 0.2f, 0.04f, {1.0f, 1.0f, 0.2f, 1.0f});
            char buf[96]{};
            char* p = buf;
            char* end = buf + sizeof(buf);
            p = DebugDraw::Append(p, end, "e");
            p = DebugDraw::FmtI64(p, end, s->entity);
            p = DebugDraw::Append(p, end, " hp");
            p = DebugDraw::FmtI64(p, end, s->hp);
            p = DebugDraw::Append(p, end, " st");
            p = DebugDraw::FmtI64(p, end, s->state);
            hud.Text(12.0f, 12.0f, {buf, static_cast<usize>(p - buf)}, {1.0f, 1.0f, 0.6f, 1.0f}, 1.0f);

            p = buf;
            p = DebugDraw::Append(p, end, "v ");
            p = DebugDraw::FmtF2(p, end, s->vx);
            p = DebugDraw::Append(p, end, " ");
            p = DebugDraw::FmtF2(p, end, s->vy);
            hud.Text(12.0f, 28.0f, {buf, static_cast<usize>(p - buf)}, {0.7f, 0.9f, 1.0f, 1.0f}, 1.0f);
        }
    }
}

} // namespace en

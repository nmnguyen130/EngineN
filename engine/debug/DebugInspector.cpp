#include "debug/DebugTools.h"

#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace en {

void DebugInspector::Record(
    const World& world,
    u32 tick,
    const TickInput* input,
    const ActionBuffer* buffer) noexcept {
    if (!m_hist || !m_pin.IsValid() || !world.IsAlive(m_pin)) return;

    const u16               idx = m_pin.index;
    const WorldTransform2D& wt  = world.GetWorldByIndex(idx);

    ActorSample rec{};
    rec.tick   = tick;
    rec.entity = idx;
    rec.x      = wt.x;
    rec.y      = wt.y;

    if (const auto* body = world.TryGet<PhysicsBody2D>(m_pin)) {
        rec.vx       = body->velocity.x;
        rec.vy       = body->velocity.y;
        rec.contacts = static_cast<u8>(body->contacts);
    } else if (const auto* vel = world.TryGet<Velocity2D>(m_pin)) {
        rec.vx = vel->vx;
        rec.vy = vel->vy;
    }

    if (const auto* fsm = world.TryGet<StateMachine2D>(m_pin)) {
        rec.state = fsm->state;
    }
    if (const auto* hp = world.TryGet<Health2D>(m_pin)) {
        rec.hp     = hp->hp;
        rec.iframe = static_cast<u8>(std::min<u16>(255U, hp->iframeTicks));
    }
    if (input) {
        rec.held    = static_cast<u8>(input->held & 0xFFU);
        rec.pressed = static_cast<u8>(input->pressed & 0xFFU);
        rec.ax      = static_cast<i8>(std::clamp(input->Axis(0) * 127.0f, -127.0f, 127.0f));
        rec.ay      = static_cast<i8>(std::clamp(input->Axis(1) * 127.0f, -127.0f, 127.0f));
    }
    if (buffer) {
        u8 bits = 0U;
        for (u8 a = 0U; a < 8U; ++a) {
            if (buffer->Peek(a)) bits = static_cast<u8>(bits | (1U << a));
        }
        rec.buffer = bits;
    }

    const u16 slot = m_write & (kFlightTicks - 1U);
    m_hist[slot]   = rec;
    m_write        = static_cast<u16>((slot + 1U) & (kFlightTicks - 1U));
    if (m_valid < kFlightTicks) ++m_valid;
}

const ActorSample* DebugInspector::Current() const noexcept {
    if (!m_hist || m_valid == 0U) return nullptr;
    const u8  off = std::min<u8>(m_scrub, static_cast<u8>(m_valid - 1U));
    const u32 i   = (static_cast<u32>(m_write) + kFlightTicks - 1U - off) & (kFlightTicks - 1U);
    return &m_hist[i];
}

EntityId DebugInspector::Pick(const World& world, Vec2 pos, f32 radius) noexcept {
    EntityId   best   = kNullEntity;
    f32        bestSq = radius * radius;
    const auto words  = world.AliveWords();

    for (usize w = 0U; w < words.size(); ++w) {
        for (u64 bits = words[w]; bits != 0ULL; bits &= bits - 1ULL) {
            const u16 eIdx = static_cast<u16>((w << 6U) + std::countr_zero(bits));
            if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) continue;

            const EntityId e = world.EntityAtSlot(eIdx);
            if (!e.IsValid()) continue;

            const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
            Rect bounds = Rect::FromCenterHalf({wt.x, wt.y}, {0.25f, 0.25f});
            if (const auto* col = world.TryGet<Collider2D>(e)) {
                bounds = col->WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY});
            }

            const Vec2 center = bounds.Center();
            const Vec2 half   = bounds.HalfExtents();
            const f32  dx     = std::max(0.0f, std::abs(pos.x - center.x) - half.x);
            const f32  dy     = std::max(0.0f, std::abs(pos.y - center.y) - half.y);
            const f32  d2     = dx * dx + dy * dy;
            if (d2 <= bestSq) {
                bestSq = d2;
                best   = e;
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
    const u64    reg   = world.RegisteredPoolsMask();

    if (HasFlag(flags, Overlay::Colliders) && (reg & MakeComponentMask<Collider2D>()) != 0ULL) {
        const auto& pool = world.GetPool<Collider2D>();
        for (u16 i = 0U; i < pool.Size(); ++i) {
            const EntityId          e   = pool.Entities()[i];
            const Collider2D&       col = pool.ComponentAtDense(i);
            const WorldTransform2D& wt  = world.GetWorldByIndex(e.index);
            const Rect              box = col.WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY});

            Color wire{0.2f, 0.9f, 1.0f, 0.85f};
            if (col.isTrigger != 0U) wire = {0.9f, 0.9f, 0.2f, 0.75f};
            else if (col.isStatic != 0U) wire = {0.5f, 0.6f, 0.8f, 0.75f};
            worldDraw.Box(box, 0.04f, wire);
        }
    }

    if (HasFlag(flags, Overlay::Velocity) && (reg & MakeComponentMask<PhysicsBody2D>()) != 0ULL) {
        const auto& pool = world.GetPool<PhysicsBody2D>();
        for (u16 i = 0U; i < pool.Size(); ++i) {
            const EntityId          e    = pool.Entities()[i];
            const PhysicsBody2D&    body = pool.ComponentAtDense(i);
            if (body.velocity.LengthSq() <= 0.0025f) continue;
            const WorldTransform2D& wt   = world.GetWorldByIndex(e.index);
            worldDraw.Arrow(wt.x, wt.y, wt.x + body.velocity.x * 0.15f, wt.y + body.velocity.y * 0.15f,
                            0.18f, 0.04f, {1.0f, 0.55f, 0.1f, 0.9f});
        }
    }

    if (HasFlag(flags, Overlay::Hitboxes) && (reg & MakeComponentMask<CombatBox2D>()) != 0ULL) {
        const auto& pool = world.GetPool<CombatBox2D>();
        for (u16 i = 0U; i < pool.Size(); ++i) {
            const EntityId          e    = pool.Entities()[i];
            const CombatBox2D&      cb   = pool.ComponentAtDense(i);
            const WorldTransform2D& wt   = world.GetWorldByIndex(e.index);
            const i8                face = world.Has<CharacterController2D>(e)
                ? world.GetPool<CharacterController2D>().DirectAtEntityIndex(e.index).facing
                : static_cast<i8>(1);

            if (HasFlag(cb.flags, CombatFlags::HurtboxActive)) {
                worldDraw.Box(cb.WorldHurtbox({wt.x, wt.y}, face), 0.04f, {0.1f, 0.95f, 0.35f, 0.85f});
            }
            if (HasFlag(cb.flags, CombatFlags::HitboxActive)) {
                worldDraw.Box(cb.WorldHitbox({wt.x, wt.y}, face), 0.05f, {1.0f, 0.2f, 0.2f, 0.9f});
            }
        }
    }

    if (HasFlag(flags, Overlay::Broadphase) && ctx.physics) {
        for (const SpatialProxy2D& p : ctx.physics->Grid().Proxies()) {
            worldDraw.Box(p.bounds, 0.03f, {0.4f, 0.4f, 1.0f, 0.35f});
        }
    }

    if (HasFlag(flags, Overlay::Audio)) {
        for (const AudioEmitterView& em : ctx.emitters) {
            const Color col = (em.occlusion > 0.5f)
                ? Color{1.0f, 0.4f, 0.2f, 0.6f}
                : Color{0.3f, 0.9f, 0.7f, 0.6f};
            worldDraw.Circle(em.position.x, em.position.y, em.maxDist, 16U, 0.03f, col);
        }
    }

    if (HasFlag(flags, Overlay::Inspector)) {
        if (const ActorSample* s = Current()) {
            worldDraw.Cross(s->x, s->y, 0.2f, 0.04f, {1.0f, 1.0f, 0.2f, 1.0f});
            char  buf[96]{};
            char* end = buf + sizeof(buf);
            char* p   = DebugDraw::Append(buf, end, "e");
            p         = DebugDraw::FmtI64(p, end, s->entity);
            p         = DebugDraw::Append(p, end, " hp");
            p         = DebugDraw::FmtI64(p, end, s->hp);
            p         = DebugDraw::Append(p, end, " st");
            p         = DebugDraw::FmtI64(p, end, s->state);
            hud.Text(12.0f, 12.0f, {buf, static_cast<usize>(p - buf)}, {1.0f, 1.0f, 0.6f, 1.0f}, 1.0f);

            p = DebugDraw::Append(buf, end, "v ");
            p = DebugDraw::FmtF2(p, end, s->vx);
            p = DebugDraw::Append(p, end, " ");
            p = DebugDraw::FmtF2(p, end, s->vy);
            hud.Text(12.0f, 28.0f, {buf, static_cast<usize>(p - buf)}, {0.7f, 0.9f, 0.0f, 1.0f}, 1.0f);
        }
    }
}

} // namespace en

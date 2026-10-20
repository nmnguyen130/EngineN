#include "gameplay/ParticleAndCamera.h"
#include "physics/SpatialIndex2D.h"
#include <algorithm>
#include <cmath>

namespace en {

bool ParticlePool2D::Init(AlignedArena& arena, u32 capacity) noexcept {
    m_posX = arena.Carve<f32>(capacity);
    m_posY = arena.Carve<f32>(capacity);
    m_velX = arena.Carve<f32>(capacity);
    m_velY = arena.Carve<f32>(capacity);
    m_drag = arena.Carve<f32>(capacity);
    m_gravScale = arena.Carve<f32>(capacity);
    m_age = arena.Carve<f32>(capacity);
    m_invLife = arena.Carve<f32>(capacity);
    m_startSize = arena.Carve<f32>(capacity);
    m_endSize = arena.Carve<f32>(capacity);
    m_size = arena.Carve<f32>(capacity);
    m_startColor = arena.Carve<Color>(capacity);
    m_endColor = arena.Carve<Color>(capacity);
    m_color = arena.Carve<Color>(capacity);
    if (m_posX.empty() || m_color.empty()) return false;
    m_capacity = capacity;
    m_activeCount = 0;
    return true;
}

void ParticlePool2D::EmitBurst(const ParticleBurstDesc& desc) noexcept {
    for (u16 i = 0; i < desc.count && m_activeCount < m_capacity; ++i) {
        const u32 idx = m_activeCount++;
        const f32 angle = m_rng.RangeF32(0.0f, kTwoPi);
        const f32 speed = m_rng.RangeF32(0.2f, 1.0f) * desc.speedSpread;
        f32 c = 1.0f, s = 0.0f;
        FastSinCos(angle, c, s);
        m_posX[idx] = desc.origin.x;
        m_posY[idx] = desc.origin.y;
        m_velX[idx] = desc.baseVelocity.x + c * speed;
        m_velY[idx] = desc.baseVelocity.y + s * speed;
        m_drag[idx] = desc.drag;
        m_gravScale[idx] = desc.gravityScale;
        m_age[idx] = 0.0f;
        const f32 life = std::max(0.05f, desc.lifetimeSec * m_rng.RangeF32(0.75f, 1.25f));
        m_invLife[idx] = 1.0f / life;
        m_startSize[idx] = desc.startSize;
        m_endSize[idx] = desc.endSize;
        m_size[idx] = desc.startSize;
        m_startColor[idx] = desc.startColor;
        m_endColor[idx] = desc.endColor;
        m_color[idx] = desc.startColor;
    }
}

void ParticlePool2D::Step(f32 dt, f32 worldGravity) noexcept {
    if (dt <= 0.0f || m_activeCount == 0) return;
    u32 i = 0;
    while (i < m_activeCount) {
        m_age[i] += dt;
        const f32 t = m_age[i] * m_invLife[i];
        if (t >= 1.0f) {
            const u32 last = --m_activeCount;
            if (i != last) {
                m_posX[i] = m_posX[last];
                m_posY[i] = m_posY[last];
                m_velX[i] = m_velX[last];
                m_velY[i] = m_velY[last];
                m_drag[i] = m_drag[last];
                m_gravScale[i] = m_gravScale[last];
                m_age[i] = m_age[last];
                m_invLife[i] = m_invLife[last];
                m_startSize[i] = m_startSize[last];
                m_endSize[i] = m_endSize[last];
                m_size[i] = m_size[last];
                m_startColor[i] = m_startColor[last];
                m_endColor[i] = m_endColor[last];
                m_color[i] = m_color[last];
            }
            continue;
        }
        const f32 damp = std::max(0.0f, 1.0f - m_drag[i] * dt);
        m_velX[i] *= damp;
        m_velY[i] = (m_velY[i] - worldGravity * m_gravScale[i] * dt) * damp;
        m_posX[i] += m_velX[i] * dt;
        m_posY[i] += m_velY[i] * dt;
        m_size[i] = m_startSize[i] + (m_endSize[i] - m_startSize[i]) * t;
        m_color[i] = Color::Lerp(m_startColor[i], m_endColor[i], t);
        ++i;
    }
}

void CameraAndBreadcrumbSolver::StepCameraControllers(World& world, const SimStepView& step) noexcept {
    bool anyMoved = false;
    world.Each<CameraController2D>([&](EntityId camId, CameraController2D& ctrl) noexcept {
        if (!world.IsAlive(ctrl.primaryTarget)) return;
        const f32 dt = step.Dt(TimeChannel::Camera);
        if (dt <= 0.0f) return;

        const WorldTransform2D& targetWt = world.GetWorldByIndex(ctrl.primaryTarget.index);
        f32 facing = 0.0f;
        if (const MotionStack2D* ms = world.TryGet<MotionStack2D>(ctrl.primaryTarget)) {
            facing = static_cast<f32>(ms->facingSign);
        }
        Vec2 desired{targetWt.x + facing * ctrl.lookaheadDist, targetWt.y + ctrl.verticalOffset};
        if (world.IsAlive(ctrl.secondaryTarget)) {
            const WorldTransform2D& secWt = world.GetWorldByIndex(ctrl.secondaryTarget.index);
            desired = Vec2::Lerp(desired, {secWt.x, secWt.y}, ctrl.secondaryWeight);
        }

        LocalTransform2D& camLoc = world.ModifyLocalTransformByIndex(camId.index);
        const f32 dx = desired.x - camLoc.x;
        const f32 dy = desired.y - camLoc.y;
        const f32 targetX = (std::abs(dx) > ctrl.deadzoneHalfW)
                                ? (desired.x - (dx > 0.0f ? ctrl.deadzoneHalfW : -ctrl.deadzoneHalfW))
                                : camLoc.x;
        const f32 targetY = (std::abs(dy) > ctrl.deadzoneHalfH)
                                ? (desired.y - (dy > 0.0f ? ctrl.deadzoneHalfH : -ctrl.deadzoneHalfH))
                                : camLoc.y;

        StepSpring(camLoc.x, ctrl.velX, targetX, ctrl.springFreqX, dt);
        StepSpring(camLoc.y, ctrl.velY, targetY, ctrl.springFreqY, dt);

        f32 shakeX = 0.0f;
        f32 shakeY = 0.0f;
        if (ctrl.trauma > 0.0f) {
            const f32 shake = ctrl.trauma * ctrl.trauma;
            const u64 h = Mix64(step.tickIndex ^ (static_cast<u64>(camId.index) << 32U));
            shakeX = (static_cast<f32>(h & 0xFFFFU) / 32768.0f - 1.0f) * 0.45f * shake;
            shakeY = (static_cast<f32>((h >> 16U) & 0xFFFFU) / 32768.0f - 1.0f) * 0.45f * shake;
            ctrl.trauma = std::max(0.0f, ctrl.trauma - 1.8f * dt);
        }
        StepSpring(ctrl.kickOffsetX, ctrl.kickVelX, shakeX, 18.0f, dt);
        StepSpring(ctrl.kickOffsetY, ctrl.kickVelY, shakeY, 18.0f, dt);

        camLoc.x = std::clamp(
            camLoc.x + ctrl.kickOffsetX,
            ctrl.activeRoomBounds.x,
            ctrl.activeRoomBounds.Right());
        camLoc.y = std::clamp(
            camLoc.y + ctrl.kickOffsetY,
            ctrl.activeRoomBounds.y,
            ctrl.activeRoomBounds.Bottom());
        anyMoved = true;
    });
    if (anyMoved) world.PropagateTransforms();
}

void CameraAndBreadcrumbSolver::StepSafeBreadcrumbs(
    World& world, const SpatialIndex2D& spatial, const SimStepView& /*step*/) noexcept {
    world.Each<SafeGroundBreadcrumb2D, KinematicMotor2D>(
        [&](EntityId id, SafeGroundBreadcrumb2D& bc, const KinematicMotor2D& motor) noexcept {
            if (!motor.Grounded() || motor.carrierId.IsValid() ||
                HasFlag(motor.contacts, ContactFlags::OnOneWay | ContactFlags::InHazard)) {
                return;
            }
            const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
            const i32 tx = static_cast<i32>(std::floor((wt.x - spatial.Origin().x) / spatial.TileSize()));
            const i32 ty = static_cast<i32>(std::floor((wt.y - motor.bounds.HalfExtents().y - 0.1f - spatial.Origin().y) / spatial.TileSize()));
            if (spatial.GetTile(tx, ty) == TileType::Solid &&
                spatial.GetTile(tx - 1, ty) == TileType::Solid &&
                spatial.GetTile(tx + 1, ty) == TileType::Solid) {
                bc.prevSafeX = bc.safeX;
                bc.prevSafeY = bc.safeY;
                bc.safeX = wt.x;
                bc.safeY = wt.y;
            }
        });
}

} // namespace en

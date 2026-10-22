#include "gameplay/CameraDirector2D.h"

namespace en {

namespace {

[[nodiscard]] constexpr f32 HashSignedNoise(u64 seed) noexcept {
    u64 z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31U);
    return static_cast<f32>(static_cast<i32>(z & 0xFFFFULL) - 32768) * (1.0f / 32768.0f);
}

} // namespace

void CameraDirector2D::StepCameras(
    World& world,
    const SimStepView& step,
    const GameplayConfig& cfg,
    u64 tickCount) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << CameraController2D::kTypeId)) == 0ULL) return;

    auto& ctrlPool = world.GetPool<CameraController2D>();
    if (ctrlPool.Size() == 0U) return;

    const bool hasRigs = (regMask & (1ULL << CameraRig2D::kTypeId)) != 0ULL;
    const bool hasMotors = (regMask & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL;
    const bool hasMotions = (regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL;

    const auto entities = ctrlPool.Entities();
    auto ctrls = ctrlPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId camEntity = entities[i];
        CameraController2D& ctrl = ctrls[i];
        if (!world.IsAlive(ctrl.primaryTarget)) continue;

        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(camEntity.index));
        const f32 dt = step.IsFrozen(ch) ? 0.0f : step.Dt(ch);
        if (dt <= 0.0f) continue;

        const WorldTransform2D& primXf = world.GetWorldByIndex(ctrl.primaryTarget.index);
        f32 focusX = primXf.x;
        f32 focusY = primXf.y;

        // 1. Optional platformer ground-height latching
        if (HasPhysicsFlag(ctrl.flags, CameraCtrlFlags::LatchGroundY) && hasMotors &&
            (world.GetComponentMaskByIndex(ctrl.primaryTarget.index) & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL) {
            const CharacterMotor2D& motor = world.GetPool<CharacterMotor2D>().DirectAtEntityIndex(ctrl.primaryTarget.index);
            if (motor.IsGrounded()) {
                ctrl.latchedGroundY = primXf.y;
            }
            if (std::abs(primXf.y - ctrl.latchedGroundY) <= 2.5f) {
                focusY = ctrl.latchedGroundY;
            }
        }

        // 2. Dual-target weighted framing
        if (ctrl.secondaryWeight_q8 > 0U && world.IsAlive(ctrl.secondaryTarget)) {
            const WorldTransform2D& secXf = world.GetWorldByIndex(ctrl.secondaryTarget.index);
            const f32 w = static_cast<f32>(ctrl.secondaryWeight_q8) * (1.0f / 255.0f);
            focusX = focusX + (secXf.x - focusX) * w;
            focusY = focusY + (secXf.y - focusY) * w;
        }

        // 3. Velocity/facing lookahead
        if (HasPhysicsFlag(ctrl.flags, CameraCtrlFlags::EnableLookahead) && hasMotions &&
            (world.GetComponentMaskByIndex(ctrl.primaryTarget.index) & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
            const MotionStack2D& ms = world.GetPool<MotionStack2D>().DirectAtEntityIndex(ctrl.primaryTarget.index);
            const f32 targetLookX = static_cast<f32>(ms.FacingSign()) * cfg.lookaheadDistance;
            const f32 curLookX = static_cast<f32>(ctrl.lookaheadX_q8) * (1.0f / 256.0f);
            const f32 nextLookX = curLookX + (targetLookX - curLookX) * std::min(1.0f, dt * 6.0f);
            ctrl.lookaheadX_q8 = CompactAabb2D::QuantizeS16(nextLookX);
            focusX += nextLookX;
        }

        if (HasPhysicsFlag(ctrl.flags, CameraCtrlFlags::SnapNextStep)) {
            ctrl.anchorX = focusX;
            ctrl.anchorY = focusY;
            ctrl.velX = 0.0f;
            ctrl.velY = 0.0f;
            ctrl.flags &= ~CameraCtrlFlags::SnapNextStep;
        } else {
            // 4. Deadzone window clamping around drift-free anchorX/anchorY
            const f32 dzW = ctrl.DeadzoneHalfW();
            const f32 dzH = ctrl.DeadzoneHalfH();
            const f32 springTargetX = std::clamp(ctrl.anchorX, focusX - dzW, focusX + dzW);
            const f32 springTargetY = std::clamp(ctrl.anchorY, focusY - dzH, focusY + dzH);

            StepCriticallyDampedSpring1D(ctrl.anchorX, ctrl.velX, springTargetX, 10.0f, dt);
            StepCriticallyDampedSpring1D(ctrl.anchorY, ctrl.velY, springTargetY, 12.0f, dt);
        }

        // 5. Room confiner clamping
        if (HasPhysicsFlag(ctrl.flags, CameraCtrlFlags::ConfineToRoom) && !ctrl.activeRoomBounds.IsEmpty()) {
            f32 halfViewW = 0.0f;
            f32 halfViewH = 0.0f;
            if (hasRigs && (world.GetComponentMaskByIndex(camEntity.index) & (1ULL << CameraRig2D::kTypeId)) != 0ULL) {
                const CameraRig2D& rig = world.GetPool<CameraRig2D>().DirectAtEntityIndex(camEntity.index);
                const f32 invZoom = (rig.zoom > 1e-4f) ? (1.0f / rig.zoom) : 1.0f;
                halfViewW = 0.5f * rig.viewport.w * invZoom;
                halfViewH = 0.5f * rig.viewport.h * invZoom;
            }
            const f32 minX = ctrl.activeRoomBounds.x + halfViewW;
            const f32 maxX = ctrl.activeRoomBounds.Right() - halfViewW;
            const f32 minY = ctrl.activeRoomBounds.y + halfViewH;
            const f32 maxY = ctrl.activeRoomBounds.Bottom() - halfViewH;

            if (minX <= maxX) ctrl.anchorX = std::clamp(ctrl.anchorX, minX, maxX);
            else              ctrl.anchorX = ctrl.activeRoomBounds.Center().x;

            if (minY <= maxY) ctrl.anchorY = std::clamp(ctrl.anchorY, minY, maxY);
            else              ctrl.anchorY = ctrl.activeRoomBounds.Center().y;
        }

        // 6. Step drift-free directional kick return spring and quadratic trauma shake
        f32 kvx = ctrl.KickVelX();
        f32 kvy = ctrl.KickVelY();
        StepCriticallyDampedSpring1D(ctrl.kickOffsetX, kvx, 0.0f, 18.0f, dt);
        StepCriticallyDampedSpring1D(ctrl.kickOffsetY, kvy, 0.0f, 18.0f, dt);
        ctrl.SetKickVel(kvx, kvy);

        f32 shakeX = ctrl.kickOffsetX;
        f32 shakeY = ctrl.kickOffsetY;
        const f32 trauma = ctrl.Trauma();
        if (trauma > 0.0f) {
            constexpr f32 kTraumaDecayRate = 1.5f;
            const f32 shakeMag = trauma * trauma * 0.45f;
            shakeX += HashSignedNoise((tickCount << 2U) ^ static_cast<u64>(camEntity.index)) * shakeMag;
            shakeY += HashSignedNoise((tickCount << 2U) ^ static_cast<u64>(camEntity.index) ^ 0x55AA55AAULL) * shakeMag;
            const f32 nextTrauma = std::max(0.0f, trauma - kTraumaDecayRate * dt);
            ctrl.trauma_q16 = static_cast<u16>(nextTrauma * 65535.0f + 0.5f);
        }

        LocalTransform2D& camLoc = world.ModifyLocalTransformByIndex(camEntity.index);
        camLoc.x = ctrl.anchorX + shakeX;
        camLoc.y = ctrl.anchorY + shakeY;

        if (hasRigs && (world.GetComponentMaskByIndex(camEntity.index) & (1ULL << CameraRig2D::kTypeId)) != 0ULL) {
            CameraRig2D& rig = world.GetPool<CameraRig2D>().DirectAtEntityIndex(camEntity.index);
            rig.trauma = ctrl.Trauma();
        }
    }
    world.PropagateTransforms();
}

} // namespace en

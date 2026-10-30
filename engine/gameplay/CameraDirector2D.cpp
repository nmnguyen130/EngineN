#include "gameplay/CameraDirector2D.h"

namespace en {

namespace {

[[nodiscard]] constexpr f32 HashNoise(u64 seed) noexcept {
    u64 z = seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= (z >> 31U);
    return static_cast<f32>(static_cast<i32>(z & 0xFFFFULL) - 32768) * (1.0f / 32768.0f);
}

} // namespace

void CameraDirector2D::Step(
    World& world,
    const SimStepView& step,
    const GameplayConfig& cfg,
    u64 tickCount) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << CameraController2D::kTypeId)) == 0ULL) return;

    auto& ctrlPool = world.GetPool<CameraController2D>();
    if (ctrlPool.Size() == 0U) return;

    const bool hasRigs    = (regMask & (1ULL << CameraRig2D::kTypeId)) != 0ULL;
    const bool hasMotors  = (regMask & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL;
    const bool hasMotions = (regMask & (1ULL << MotionStack2D::kTypeId)) != 0ULL;

    const auto entities = ctrlPool.Entities();
    auto ctrls = ctrlPool.Components();
    bool anyUpdated = false;

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

        if (HasFlag(ctrl.flags, CameraCtrlFlags::LatchGroundY) && hasMotors &&
            (world.GetComponentMaskByIndex(ctrl.primaryTarget.index) & (1ULL << CharacterMotor2D::kTypeId)) != 0ULL) {
            if (world.GetPool<CharacterMotor2D>().DirectAtEntityIndex(ctrl.primaryTarget.index).IsGrounded()) {
                ctrl.latchedGroundY = primXf.y;
            }
            if (std::abs(primXf.y - ctrl.latchedGroundY) <= 2.5f) {
                focusY = ctrl.latchedGroundY;
            }
        }

        if (ctrl.secondaryWeight_q8 > 0U && world.IsAlive(ctrl.secondaryTarget)) {
            const WorldTransform2D& secXf = world.GetWorldByIndex(ctrl.secondaryTarget.index);
            const f32 w = static_cast<f32>(ctrl.secondaryWeight_q8) * (1.0f / 255.0f);
            focusX += (secXf.x - focusX) * w;
            focusY += (secXf.y - focusY) * w;
        }

        if (HasFlag(ctrl.flags, CameraCtrlFlags::EnableLookahead) && hasMotions &&
            (world.GetComponentMaskByIndex(ctrl.primaryTarget.index) & (1ULL << MotionStack2D::kTypeId)) != 0ULL) {
            const MotionStack2D& ms = world.GetPool<MotionStack2D>().DirectAtEntityIndex(ctrl.primaryTarget.index);
            const f32 targetLookX = static_cast<f32>(ms.FacingSign()) * cfg.lookaheadDistance;
            const f32 curLookX = static_cast<f32>(ctrl.lookaheadX_q8) * (1.0f / 256.0f);
            const f32 nextLookX = curLookX + (targetLookX - curLookX) * std::min(1.0f, dt * 6.0f);
            ctrl.lookaheadX_q8 = CompactAabb2D::QuantizeS16(nextLookX);
            focusX += nextLookX;
        }

        if (HasFlag(ctrl.flags, CameraCtrlFlags::SnapNextStep)) {
            ctrl.anchorX = focusX;
            ctrl.anchorY = focusY;
            ctrl.velX = 0.0f;
            ctrl.velY = 0.0f;
            ctrl.flags &= ~CameraCtrlFlags::SnapNextStep;
        } else {
            const f32 dzW = ctrl.DeadzoneHalfW();
            const f32 dzH = ctrl.DeadzoneHalfH();
            Spring1D(ctrl.anchorX, ctrl.velX, std::clamp(ctrl.anchorX, focusX - dzW, focusX + dzW), 10.0f, dt);
            Spring1D(ctrl.anchorY, ctrl.velY, std::clamp(ctrl.anchorY, focusY - dzH, focusY + dzH), 12.0f, dt);
        }

        CameraRig2D* camRig = nullptr;
        if (hasRigs && (world.GetComponentMaskByIndex(camEntity.index) & (1ULL << CameraRig2D::kTypeId)) != 0ULL) {
            camRig = &world.GetPool<CameraRig2D>().DirectAtEntityIndex(camEntity.index);
        }

        if (HasFlag(ctrl.flags, CameraCtrlFlags::ConfineToRoom) && !ctrl.activeRoomBounds.IsEmpty()) {
            f32 halfViewW = 0.0f;
            f32 halfViewH = 0.0f;
            if (camRig) {
                const f32 invZoom = (camRig->zoom > 1e-4f) ? (1.0f / camRig->zoom) : 1.0f;
                if (camRig->viewport.w <= 1.0f && camRig->viewport.h <= 1.0f) {
                    const f32 invPpm = (camRig->pixelsPerMeter > 1e-4f)
                        ? (1.0f / camRig->pixelsPerMeter) : (1.0f / 48.0f);
                    halfViewW = 640.0f * camRig->viewport.w * invPpm * invZoom;
                    halfViewH = 360.0f * camRig->viewport.h * invPpm * invZoom;
                } else {
                    halfViewW = 0.5f * camRig->viewport.w * invZoom;
                    halfViewH = 0.5f * camRig->viewport.h * invZoom;
                }
            }
            const f32 minX = ctrl.activeRoomBounds.x + halfViewW;
            const f32 maxX = ctrl.activeRoomBounds.Right() - halfViewW;
            const f32 minY = ctrl.activeRoomBounds.y + halfViewH;
            const f32 maxY = ctrl.activeRoomBounds.Bottom() - halfViewH;
            ctrl.anchorX = (minX <= maxX) ? std::clamp(ctrl.anchorX, minX, maxX)
                                          : ctrl.activeRoomBounds.Center().x;
            ctrl.anchorY = (minY <= maxY) ? std::clamp(ctrl.anchorY, minY, maxY)
                                          : ctrl.activeRoomBounds.Center().y;
        }

        f32 kvx = ctrl.KickVelX();
        f32 kvy = ctrl.KickVelY();
        Spring1D(ctrl.kickOffsetX, kvx, 0.0f, 18.0f, dt);
        Spring1D(ctrl.kickOffsetY, kvy, 0.0f, 18.0f, dt);
        ctrl.SetKickVel(kvx, kvy);

        f32 shakeX = ctrl.kickOffsetX;
        f32 shakeY = ctrl.kickOffsetY;
        const f32 trauma = ctrl.Trauma();
        if (trauma > 0.0f) {
            const f32 mag = trauma * trauma * 0.45f;
            shakeX += HashNoise((tickCount << 2U) ^ static_cast<u64>(camEntity.index)) * mag;
            shakeY += HashNoise((tickCount << 2U) ^ static_cast<u64>(camEntity.index) ^ 0x55AA55AAULL) * mag;
            ctrl.trauma_q16 = static_cast<u16>(std::max(0.0f, trauma - 1.5f * dt) * 65535.0f + 0.5f);
        }

        LocalTransform2D& camLoc = world.ModifyLocalTransformByIndex(camEntity.index);
        camLoc.x = ctrl.anchorX + shakeX;
        camLoc.y = ctrl.anchorY + shakeY;
        anyUpdated = true;

        if (camRig) camRig->trauma = ctrl.Trauma();
    }
    if (anyUpdated) world.PropagateTransforms();
}

} // namespace en

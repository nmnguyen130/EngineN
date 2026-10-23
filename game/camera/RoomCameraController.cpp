#include "game/camera/RoomCameraController.h"

#include <algorithm>
#include <cmath>

namespace vots {

namespace {

struct RoomCameraLimits {
    f32 minX{kRoomCamCenterMinX};
    f32 maxX{kRoomCamCenterMaxX};
    f32 minY{kRoomCamCenterMinY};
    f32 maxY{kRoomCamCenterMaxY};

    [[nodiscard]] constexpr en::Rect ToEngineConfinerRect() const noexcept {
        return en::Rect{
            minX - 0.5f,
            minY - 0.5f,
            (maxX - minX) + 1.0f,
            (maxY - minY) + 1.0f,
        };
    }
};

[[nodiscard]] constexpr RoomCameraLimits ComputeLimitsForRoom(u8 roomId) noexcept {
    const RoomTopologySpec& spec = GetRoomTopology(roomId);
    const f32 minX = spec.MinWorldX() + kCameraHalfViewportMarginX;
    const f32 maxX = std::max(minX, spec.MaxWorldX() - kCameraHalfViewportMarginX);
    const f32 minY = kRoomCamCenterMinY;
    const f32 maxY = std::max(minY, spec.MaxWorldY() - kCameraHalfViewportMarginY);
    return {minX, maxX, minY, maxY};
}

} // namespace

EntityId RoomCameraController::ConfigureRoomCamera(
    EntityId existingCamId,
    EntityId playerId,
    EntityId optionalBossId,
    u8 roomId,
    Vec2 initialFocusPos,
    en::World& world) noexcept {
    const RoomCameraLimits lim = ComputeLimitsForRoom(roomId);
    const f32 clampedX = std::clamp(initialFocusPos.x, lim.minX, lim.maxX);
    const f32 clampedY = std::clamp(initialFocusPos.y + kCameraVerticalBiasWu, lim.minY, lim.maxY);

    EntityId camId = existingCamId;
    if (!world.IsAlive(camId)) {
        en::EntitySpawnDesc camSp{};
        camSp.local.x = clampedX;
        camSp.local.y = clampedY;
        camSp.timeChannel = en::TimeChannel::Camera;
        camId = world.Spawn(camSp).value_or(kNullEntity);
        if (!camId.IsValid()) return kNullEntity;

        en::CameraRig2D rig{};
        rig.pixelsPerMeter = kCameraPixelsPerMeter;
        rig.zoom = 1.0f;
        rig.active = true;
        (void)world.Emplace<en::CameraRig2D>(camId, rig);

        en::CameraController2D ctrl{};
        ctrl.primaryTarget = playerId;
        ctrl.secondaryTarget = optionalBossId;
        ctrl.secondaryWeight_q8 = optionalBossId.IsValid() ? kCameraBossWeightQ8 : 0U;
        ctrl.anchorX = clampedX;
        ctrl.anchorY = clampedY;
        ctrl.latchedGroundY = clampedY;
        ctrl.deadzoneHalfW_q4 = kCameraDeadzoneHalfWQ4;
        ctrl.deadzoneHalfH_q4 = kCameraDeadzoneHalfHQ4;
        ctrl.activeRoomBounds = lim.ToEngineConfinerRect();
        ctrl.flags = en::CameraCtrlFlags::EnableLookahead |
                     en::CameraCtrlFlags::LatchGroundY |
                     en::CameraCtrlFlags::ConfineToRoom |
                     en::CameraCtrlFlags::SnapNextStep;
        (void)world.Emplace<en::CameraController2D>(camId, ctrl);
    } else {
        if (auto* rig = world.TryGet<en::CameraRig2D>(camId)) {
            rig->pixelsPerMeter = kCameraPixelsPerMeter;
            rig->active = true;
        }
        if (auto* ctrl = world.TryGet<en::CameraController2D>(camId)) {
            ctrl->primaryTarget = playerId;
            ctrl->secondaryTarget = optionalBossId;
            ctrl->secondaryWeight_q8 = optionalBossId.IsValid() ? kCameraBossWeightQ8 : 0U;
            ctrl->anchorX = clampedX;
            ctrl->anchorY = clampedY;
            ctrl->latchedGroundY = clampedY;
            ctrl->velX = 0.0f;
            ctrl->velY = 0.0f;
            ctrl->deadzoneHalfW_q4 = kCameraDeadzoneHalfWQ4;
            ctrl->deadzoneHalfH_q4 = kCameraDeadzoneHalfHQ4;
            ctrl->activeRoomBounds = lim.ToEngineConfinerRect();
            ctrl->flags = en::CameraCtrlFlags::EnableLookahead |
                          en::CameraCtrlFlags::LatchGroundY |
                          en::CameraCtrlFlags::ConfineToRoom |
                          en::CameraCtrlFlags::SnapNextStep;
        }
        en::LocalTransform2D loc = world.GetLocalTransform(camId);
        loc.x = clampedX;
        loc.y = clampedY;
        world.Teleport(camId, loc);
    }
    return camId;
}

void RoomCameraController::StepCameraFraming(
    EntityId camId,
    EntityId playerId,
    EntityId activeBossId,
    u8 roomId,
    en::World& world) noexcept {
    if (!world.IsAlive(camId) || !world.IsAlive(playerId)) return;
    auto* ctrl = world.TryGet<en::CameraController2D>(camId);
    if (ctrl == nullptr) return;

    const RoomCameraLimits lim = ComputeLimitsForRoom(roomId);
    ctrl->primaryTarget = playerId;
    ctrl->activeRoomBounds = lim.ToEngineConfinerRect();

    const en::WorldTransform2D& pxf = world.GetWorldByIndex(playerId.index);
    const auto* motor = world.TryGet<en::CharacterMotor2D>(playerId);
    const auto* motion = world.TryGet<en::MotionStack2D>(playerId);
    const auto* ps = world.TryGet<PlayerStateComp>(playerId);

    const f32 desiredGroundCenterY = std::clamp(pxf.y + kCameraVerticalBiasWu, lim.minY, lim.maxY);
    if (motor != nullptr && motor->IsGrounded()) {
        ctrl->latchedGroundY += (desiredGroundCenterY - ctrl->latchedGroundY) * 0.22f;
    } else {
        const f32 dyAbove = desiredGroundCenterY - ctrl->latchedGroundY;
        if (dyAbove > kCameraAirRiseDeadbandWu) {
            ctrl->latchedGroundY += (dyAbove - kCameraAirRiseDeadbandWu) * 0.35f;
        } else if (dyAbove < -kCameraAirFallDeadbandWu) {
            ctrl->latchedGroundY += (dyAbove + kCameraAirFallDeadbandWu) * 0.50f;
        }
    }
    ctrl->latchedGroundY = std::clamp(ctrl->latchedGroundY, lim.minY, lim.maxY);

    if (motion != nullptr) {
        const f32 facing = static_cast<f32>(motion->FacingSign());
        const bool isDashing = (ps != nullptr && ps->IsDashing());
        const bool isMoving = isDashing || (motor != nullptr && std::abs(motor->vx) > 0.85f);
        const f32 targetLook = isMoving
                                   ? facing * (isDashing ? kCameraDashLookaheadWu : kCameraWalkLookaheadWu)
                                   : (static_cast<f32>(ctrl->lookaheadX_q8) * (1.0f / 256.0f) * 0.92f);
        const f32 curLook = static_cast<f32>(ctrl->lookaheadX_q8) * (1.0f / 256.0f);
        const f32 nextLook = curLook + (targetLook - curLook) * (kCameraLookaheadDamping * en::kFixedDtSec);
        ctrl->lookaheadX_q8 = en::CompactAabb2D::QuantizeS16(nextLook);
    }

    if (world.IsAlive(activeBossId)) {
        ctrl->secondaryTarget = activeBossId;
        ctrl->secondaryWeight_q8 = kCameraBossWeightQ8;
    } else {
        EntityId nearestThreat = kNullEntity;
        f32 bestDistSq = kCameraCombatThreatRangeWu * kCameraCombatThreatRangeWu;
        for (auto [eid, ai, recv] : world.View<EnemyAiComp, en::DamageReceiver2D>()) {
            if (recv.HasFlag(en::ReceiverFlags::Dead) || recv.hp <= 0) continue;
            const en::WorldTransform2D& exf = world.GetWorldByIndex(eid.index);
            const f32 dx = exf.x - pxf.x;
            const f32 dy = exf.y - pxf.y;
            if (std::abs(dy) > kEnemyVerticalAggroRangeWu) continue;
            const f32 dSq = dx * dx + dy * dy;
            if (dSq < bestDistSq && (ai.actionTimerTicks > 0U || dSq < 36.0f)) {
                bestDistSq = dSq;
                nearestThreat = eid;
            }
        }
        ctrl->secondaryTarget = nearestThreat;
        ctrl->secondaryWeight_q8 = nearestThreat.IsValid() ? 42U : 0U;
    }

    ctrl->anchorX = std::clamp(ctrl->anchorX, lim.minX, lim.maxX);
    ctrl->anchorY = std::clamp(ctrl->anchorY, lim.minY, lim.maxY);
}

void RoomCameraController::TriggerCombatImpulse(
    EntityId camId,
    Vec2 kickVelocity,
    f32 traumaAmount,
    en::World& world,
    ScreenShakeMode mode) noexcept {
    if (!world.IsAlive(camId) || mode == ScreenShakeMode::Off) return;
    auto* ctrl = world.TryGet<en::CameraController2D>(camId);
    if (ctrl == nullptr) return;

    const f32 scale = (mode == ScreenShakeMode::Reduced) ? 0.45f : 1.0f;
    ctrl->ApplyDirectionalKick(kickVelocity.x * scale, kickVelocity.y * scale);
    if (traumaAmount > 0.0f) {
        ctrl->AddTrauma(traumaAmount * scale);
    }
}

void RoomCameraController::SnapToTarget(
    EntityId camId,
    EntityId targetId,
    u8 roomId,
    en::World& world) noexcept {
    if (!world.IsAlive(camId) || !world.IsAlive(targetId)) return;
    const RoomCameraLimits lim = ComputeLimitsForRoom(roomId);
    const en::WorldTransform2D& txf = world.GetWorldByIndex(targetId.index);
    const f32 cx = std::clamp(txf.x, lim.minX, lim.maxX);
    const f32 cy = std::clamp(txf.y + kCameraVerticalBiasWu, lim.minY, lim.maxY);

    if (auto* ctrl = world.TryGet<en::CameraController2D>(camId)) {
        ctrl->primaryTarget = targetId;
        ctrl->anchorX = cx;
        ctrl->anchorY = cy;
        ctrl->latchedGroundY = cy;
        ctrl->velX = 0.0f;
        ctrl->velY = 0.0f;
        ctrl->activeRoomBounds = lim.ToEngineConfinerRect();
        ctrl->flags |= en::CameraCtrlFlags::SnapNextStep;
    }
    en::LocalTransform2D loc = world.GetLocalTransform(camId);
    loc.x = cx;
    loc.y = cy;
    world.Teleport(camId, loc);
}

} // namespace vots

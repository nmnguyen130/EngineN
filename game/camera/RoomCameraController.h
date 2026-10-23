#pragma once

#include "game/components/GameComponents.h"
#include "gameplay/CameraDirector2D.h"
#include "world/World.h"

namespace vots {

// 2D Metroidvania room camera controller supporting variable room dimensions, platform-snap vertical framing,
// velocity/dash lookahead, dynamic threat framing, and directional combat kicks.
// Stateless utility configuring and stepping CameraController2D and CameraRig2D in World.
// Invariant: camera center is strictly confined within [MinWorldX + HalfViewW, MaxWorldX - HalfViewW].
class RoomCameraController {
public:
    [[nodiscard]] static EntityId ConfigureRoomCamera(
        EntityId existingCamId,
        EntityId playerId,
        EntityId optionalBossId,
        u8 roomId,
        Vec2 initialFocusPos,
        en::World& world) noexcept;

    [[nodiscard]] static EntityId Configure(
        EntityId existingCamId,
        EntityId playerId,
        EntityId optionalBossId,
        u8 roomId,
        Vec2 initialFocusPos,
        en::World& world) noexcept {
        return ConfigureRoomCamera(existingCamId, playerId, optionalBossId, roomId, initialFocusPos, world);
    }

    static void StepCameraFraming(
        EntityId camId,
        EntityId playerId,
        EntityId activeBossId,
        u8 roomId,
        en::World& world) noexcept;

    static void Step(
        EntityId camId,
        EntityId playerId,
        EntityId activeBossId,
        u8 roomId,
        en::World& world) noexcept {
        StepCameraFraming(camId, playerId, activeBossId, roomId, world);
    }

    static void TriggerCombatImpulse(
        EntityId camId,
        Vec2 kickVelocity,
        f32 traumaAmount,
        en::World& world,
        ScreenShakeMode mode = ScreenShakeMode::Full) noexcept;

    static void SnapToTarget(
        EntityId camId,
        EntityId targetId,
        u8 roomId,
        en::World& world) noexcept;
};

} // namespace vots

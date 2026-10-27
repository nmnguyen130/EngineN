#pragma once

#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

// Stateless 2nd-order critically damped spring 2D camera director with dual-target framing, deadzones, room confiner, and trauma shake.
// Non-owning simulation stage helper invoked by GameplayPipeline::Tick.
// Invariant: camera anchor remains confined within activeRoomBounds when ConfineToRoom is set.
class CameraDirector2D {
public:
    static constexpr void StepCriticallyDampedSpring1D(
        f32& ioPos,
        f32& ioVel,
        f32 targetPos,
        f32 omega,
        f32 dt) noexcept {
        if (omega <= 0.0f || dt <= 0.0f) {
            ioPos = targetPos;
            ioVel = 0.0f;
            return;
        }
        const f32 x = omega * dt;
        const f32 expFactor = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const f32 change = ioPos - targetPos;
        const f32 temp = (ioVel + omega * change) * dt;
        ioVel = (ioVel - omega * temp) * expFactor;
        ioPos = targetPos + (change + temp) * expFactor;
    }

    static void StepCameras(
        World& world,
        const SimStepView& step,
        const GameplayConfig& cfg,
        u64 tickCount) noexcept;
};

} // namespace en

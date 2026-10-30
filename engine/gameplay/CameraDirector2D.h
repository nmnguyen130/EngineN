#pragma once

#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

class CameraDirector2D {
public:
    static constexpr void Spring1D(f32& pos, f32& vel, f32 target, f32 omega, f32 dt) noexcept {
        if (omega <= 0.0f || dt <= 0.0f) {
            pos = target;
            vel = 0.0f;
            return;
        }
        const f32 x = omega * dt;
        const f32 expF = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const f32 change = pos - target;
        const f32 temp = (vel + omega * change) * dt;
        vel = (vel - omega * temp) * expF;
        pos = target + (change + temp) * expF;
    }

    static void Step(World& world, const SimStepView& step, const GameplayConfig& cfg,
                     u64 tickCount) noexcept;
};

} // namespace en

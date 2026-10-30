#pragma once

#include "gameplay/GameplayTypes.h"
#include "physics/PhysicsSystem.h"

namespace en {

class MotionSolver2D {
public:
    static void Sync(World& world, const PhysicsSystem& physics, const SimStepView& step,
                     u8 coyoteTicks = 8U) noexcept;

    static void Blend(World& world, const SimStepView& step, const GameplayConfig& cfg) noexcept;

    // Soft separation. O(n²) — keep ≤12 entities per pushboxGroup.
    static void Separate(World& world, const PhysicsSystem& physics, const SimStepView& step,
                         const GameplayConfig& cfg) noexcept;
};

} // namespace en

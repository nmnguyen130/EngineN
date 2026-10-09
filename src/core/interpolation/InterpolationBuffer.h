#pragma once

#include "math/Vec2.h"
#include <algorithm>
#include <cstdint>

namespace EngineN {

enum class InterpMode : uint8_t {
    Lerp    = 0,
    Hermite = 1
};

struct TransformSnapshot {
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
};

class InterpolationBuffer {
public:
    TransformSnapshot previous{};
    TransformSnapshot current{};
    InterpMode mode = InterpMode::Lerp;

    void SetInitial(const Vec2& pos) noexcept {
        previous.position = pos;
        previous.velocity = {0.0f, 0.0f};
        current = previous;
    }

    // Prevents 1-frame smear artifact during teleportation, respawns, or room changes
    void Snap(const Vec2& pos) noexcept {
        SetInitial(pos);
    }

    void Snap(const TransformSnapshot& snapshot) noexcept {
        previous = snapshot;
        current = snapshot;
    }

    // Called at simulation tick boundary when advancing transform state
    void PushState(const TransformSnapshot& newState) noexcept {
        previous = current;
        current = newState;
    }

    [[nodiscard]] Vec2 GetVisualPosition(double alpha, double fixedDtSec = 1.0 / 120.0) const noexcept {
        const float t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
        if (mode == InterpMode::Hermite) {
            const float t2 = t * t;
            const float t3 = t2 * t;
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const float h10 = t3 - 2.0f * t2 + t;
            const float h01 = -2.0f * t3 + 3.0f * t2;
            const float h11 = t3 - t2;

            const Vec2 v0 = previous.velocity * static_cast<float>(fixedDtSec);
            const Vec2 v1 = current.velocity * static_cast<float>(fixedDtSec);

            return {
                h00 * previous.position.x + h10 * v0.x + h01 * current.position.x + h11 * v1.x,
                h00 * previous.position.y + h10 * v0.y + h01 * current.position.y + h11 * v1.y
            };
        }

        // Standard linear interpolation
        return Lerp(previous.position, current.position, t);
    }
};

} // namespace EngineN

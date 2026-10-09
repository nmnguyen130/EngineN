#pragma once

#include "../../math/Vec2.h"
#include <cstdint>
#include <algorithm>

namespace EngineN {

enum class InterpMode : uint8_t {
    Lerp = 0,
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

    void SetInitial(const Vec2& pos) {
        previous.position = pos;
        previous.velocity = {0.0f, 0.0f};
        current = previous;
    }

    // Called at tick boundary before or after updating current state
    void PushState(const TransformSnapshot& newState) {
        previous = current;
        current = newState;
    }

    [[nodiscard]] Vec2 GetVisualPosition(double alpha, double fixedDtSec = 1.0 / 120.0) const {
        float t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
        if (mode == InterpMode::Hermite) {
            float t2 = t * t;
            float t3 = t2 * t;
            float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            float h10 = t3 - 2.0f * t2 + t;
            float h01 = -2.0f * t3 + 3.0f * t2;
            float h11 = t3 - t2;

            Vec2 v0 = previous.velocity * static_cast<float>(fixedDtSec);
            Vec2 v1 = current.velocity * static_cast<float>(fixedDtSec);

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

#pragma once

#include "core/Types.h"
#include "math/Vec2.h"

namespace EngineN {

/**
 * Concept for values that can be linearly interpolated with a floating-point alpha in [0, 1].
 */
template <class T>
concept Interpolatable = requires(const T& a, const T& b, f32 t) {
    Lerp(a, b, t);
};

inline constexpr f32 Lerp(f32 a, f32 b, f32 t) noexcept {
    return a + (b - a) * t;
}

/**
 * Interpolated: Engine-owned container for values with sub-frame presentation interpolation.
 * Calling BeginTick() unconditionally at every simulation tick guarantees prev == cur when
 * frozen, permanently preventing visual smearing or oscillation.
 */
template <Interpolatable T>
class Interpolated {
public:
    constexpr Interpolated() = default;
    constexpr explicit Interpolated(const T& val) noexcept : m_prev(val), m_cur(val) {}

    // Called unconditionally at the start of EVERY tick (even when frozen)
    constexpr void BeginTick() noexcept { m_prev = m_cur; }

    // Instant teleport, spawn, or camera cut (no visual smearing)
    constexpr void Snap(const T& val) noexcept { m_prev = m_cur = val; }

    [[nodiscard]] constexpr T&       Cur()       noexcept { return m_cur; }
    [[nodiscard]] constexpr const T& Cur() const noexcept { return m_cur; }
    [[nodiscard]] constexpr const T& Prev() const noexcept { return m_prev; }

    [[nodiscard]] constexpr T Get(f32 alpha) const noexcept {
        const f32 t = (alpha < 0.0f) ? 0.0f : ((alpha > 1.0f) ? 1.0f : alpha);
        return Lerp(m_prev, m_cur, t);
    }

private:
    T m_prev{};
    T m_cur{};
};

} // namespace EngineN

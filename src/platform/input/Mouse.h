#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

struct SDL_MouseButtonEvent;
struct SDL_MouseMotionEvent;
struct SDL_MouseWheelEvent;

namespace EngineN {

/// Low-Level Hardware Abstraction: Physical Mouse Device.
/// Tracks physical mouse buttons (edge-detection), absolute window cursor position,
/// accumulated sub-frame relative movement deltas, and scroll wheel ticks.
/// Implemented with zero heap allocations.
class Mouse {
public:
    Mouse() = default;
    ~Mouse() = default;

    /// Snapshots button states and clears relative frame deltas (motion and wheel).
    void BeginFrame() noexcept;

    void ProcessButtonEvent(const SDL_MouseButtonEvent& event) noexcept;
    void ProcessMotionEvent(const SDL_MouseMotionEvent& event) noexcept;
    void ProcessWheelEvent(const SDL_MouseWheelEvent& event) noexcept;

    [[nodiscard]] bool IsButtonDown(u8 button) const noexcept;
    [[nodiscard]] bool IsButtonJustPressed(u8 button) const noexcept;
    [[nodiscard]] bool IsButtonJustReleased(u8 button) const noexcept;

    [[nodiscard]] i32 GetX() const noexcept { return m_x; }
    [[nodiscard]] i32 GetY() const noexcept { return m_y; }
    [[nodiscard]] i32 GetDeltaX() const noexcept { return m_deltaX; }
    [[nodiscard]] i32 GetDeltaY() const noexcept { return m_deltaY; }
    [[nodiscard]] f32 GetWheelX() const noexcept { return m_wheelX; }
    [[nodiscard]] f32 GetWheelY() const noexcept { return m_wheelY; }

    /// Clears mouse buttons and motion state (e.g., on focus loss).
    void Reset() noexcept;

private:
    std::array<bool, kMaxMouseButtons> m_buttons{};
    std::array<bool, kMaxMouseButtons> m_prevButtons{};
    i32 m_x = 0;
    i32 m_y = 0;
    i32 m_deltaX = 0;
    i32 m_deltaY = 0;
    f32 m_wheelX = 0.0f;
    f32 m_wheelY = 0.0f;
};

} // namespace EngineN

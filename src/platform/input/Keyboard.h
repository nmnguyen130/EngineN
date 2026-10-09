#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

struct SDL_KeyboardEvent;

namespace EngineN {

/// Low-Level Hardware Abstraction: Physical Keyboard Device.
/// Tracks raw scancode states, performs frame-accurate edge detection (JustPressed / JustReleased),
/// and filters out unwanted OS key-repeat events.
/// Implemented with zero heap allocations.
class Keyboard {
public:
    Keyboard() = default;
    ~Keyboard() = default;

    /// Snapshots current key states to previous states for edge detection.
    void BeginFrame() noexcept;

    /// Processes an SDL keyboard event.
    /// Returns true if the event represents a valid physical state transition (repeat events ignored).
    bool ProcessEvent(const SDL_KeyboardEvent& event) noexcept;

    [[nodiscard]] bool IsDown(u32 scancode) const noexcept;
    [[nodiscard]] bool IsUp(u32 scancode) const noexcept;
    [[nodiscard]] bool IsJustPressed(u32 scancode) const noexcept;
    [[nodiscard]] bool IsJustReleased(u32 scancode) const noexcept;

    /// Clears all keys to unpressed state (e.g., on window focus loss).
    void Reset() noexcept;

private:
    std::array<bool, kMaxKeyboardKeys> m_currentState{};
    std::array<bool, kMaxKeyboardKeys> m_previousState{};
};

} // namespace EngineN

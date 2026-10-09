#pragma once

#include "core/Types.h"

namespace EngineN {

class Window;
class InputSystem;

/// Strictly enforces "Single Source of Truth" for SDL_PollEvent.
/// SDL_PollEvent is called in exactly ONE loop per frame inside PollAll().
class EventPump {
public:
    EventPump() = default;
    ~EventPump() = default;

    /// Drain the entire SDL event queue for this frame and route to window and input.
    /// Returns true if a quit was requested.
    bool PollAll(Window& window, InputSystem& input);

    [[nodiscard]] bool WasQuitRequested() const noexcept { return m_quitRequested; }
    [[nodiscard]] bool WasLowMemory() const noexcept { return m_lowMemory; }
    [[nodiscard]] bool WasTerminating() const noexcept { return m_terminating; }

    void BeginFrame() noexcept;

private:
    bool m_quitRequested = false;
    bool m_lowMemory = false;
    bool m_terminating = false;
};

} // namespace EngineN

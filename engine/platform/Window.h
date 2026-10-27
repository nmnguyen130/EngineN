#pragma once

#include "platform/PlatformTypes.h"

#include <utility>

struct SDL_Window;

namespace en {

// Move-only OS window wrapper isolating SDL3 window management to its implementation unit.
// Invariant: State().pixelDensity > 0.0f and RefreshMetrics() executes at most once per frame.
class alignas(16) Window {
public:
    Window() noexcept = default;
    ~Window() noexcept { Destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept
        : m_state(other.m_state),
          m_window(std::exchange(other.m_window, nullptr)),
          m_windowId(std::exchange(other.m_windowId, 0U)),
          m_cursorVisible(other.m_cursorVisible),
          m_textInputActive(other.m_textInputActive) {}

    Window& operator=(Window&& other) noexcept {
        if (this != &other) {
            Destroy();
            m_state = other.m_state;
            m_window = std::exchange(other.m_window, nullptr);
            m_windowId = std::exchange(other.m_windowId, 0U);
            m_cursorVisible = other.m_cursorVisible;
            m_textInputActive = other.m_textInputActive;
        }
        return *this;
    }

    [[nodiscard]] Result<void, PlatformErrc> Create(
        const WindowConfig& cfg = {}, bool headless = false) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_window != nullptr || m_windowId != 0U; }
    [[nodiscard]] SDL_Window* Native() const noexcept { return m_window; }
    [[nodiscard]] constexpr u32 Id() const noexcept { return m_windowId; }
    [[nodiscard]] constexpr const WindowState& State() const noexcept { return m_state; }

    [[nodiscard]] Result<void, PlatformErrc> SetMode(
        WindowMode mode, f32 targetRefreshHz = 0.0f) noexcept;
    void SetTitle(std::string_view title) noexcept;
    void SetRelativeMouse(bool enabled) noexcept;
    void SetCursorVisible(bool visible) noexcept;
    void SetTextInput(bool enabled) noexcept;
    void WarpMouse(Vec2 pixelPos) noexcept;

    [[nodiscard]] constexpr bool IsCursorVisible() const noexcept { return m_cursorVisible; }
    [[nodiscard]] constexpr bool IsTextInputActive() const noexcept { return m_textInputActive; }

    constexpr void ApplyLifecycleEvent(
        WindowFlags eventFlag, WindowFlags& ioNetFlags, bool& outNeedRefresh) noexcept {
        m_state.ApplyLifecycleEvent(eventFlag, ioNetFlags, outNeedRefresh);
    }

    WindowFlags RefreshMetrics() noexcept;

private:
    WindowState m_state{};
    SDL_Window* m_window{nullptr};
    u32 m_windowId{0};
    bool m_cursorVisible{true};
    bool m_textInputActive{false};
};
static_assert(sizeof(Window) == 48);

} // namespace en

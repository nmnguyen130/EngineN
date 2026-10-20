#pragma once

#include "platform/PlatformTypes.h"

struct SDL_Window;
union SDL_Event;

namespace en {

// Move-only RAII OS window wrapper isolating SDL3 window management to its implementation unit.
// Places WindowState at offset 0 of Cache Line 0.
// Invariant: State().pixelDensity >= 1.0f and RefreshMetrics() executes at most once per frame.
class alignas(64) Window {
public:
    Window() noexcept = default;
    ~Window() noexcept { Destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept
        : m_state(other.m_state),
          m_window(std::exchange(other.m_window, nullptr)),
          m_windowId(std::exchange(other.m_windowId, 0)),
          m_cursorVisible(other.m_cursorVisible),
          m_textInputActive(other.m_textInputActive) {}

    Window& operator=(Window&& other) noexcept {
        if (this != &other) {
            Destroy();
            m_state = other.m_state;
            m_window = std::exchange(other.m_window, nullptr);
            m_windowId = std::exchange(other.m_windowId, 0);
            m_cursorVisible = other.m_cursorVisible;
            m_textInputActive = other.m_textInputActive;
        }
        return *this;
    }

    [[nodiscard]] Result<void, PlatformErrc> Create(const WindowConfig& cfg = {}) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_window != nullptr; }
    [[nodiscard]] SDL_Window* Native() const noexcept { return m_window; }
    [[nodiscard]] constexpr u32 Id() const noexcept { return m_windowId; }
    [[nodiscard]] constexpr const WindowState& State() const noexcept { return m_state; }

    [[nodiscard]] Result<void, PlatformErrc> SetMode(WindowMode mode) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetTitle(std::string_view title) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetRelativeMouse(bool enabled) noexcept;
    void SetCursorVisible(bool visible) noexcept;
    void SetTextInput(bool enabled) noexcept;

    using DpiScaleCallbackFn = void (*)(f32 newDpiScale, Extent2i logicalSize, Extent2i pixelSize, void* userCtx) noexcept;

    constexpr void SetDpiChangeCallback(DpiScaleCallbackFn fn, void* userCtx = nullptr) noexcept {
        m_dpiCallback = fn;
        m_dpiUserCtx = userCtx;
    }
    void SimulateDpiMigration(f32 newPixelDensity, Extent2i newLogicalSize) noexcept;
    [[nodiscard]] constexpr u32 DpiMigrationCount() const noexcept { return m_dpiMigrationCount; }

    [[nodiscard]] WindowFlags ProcessEvent(const SDL_Event& ev, bool& outNeedRefresh) noexcept;
    void RefreshMetrics() noexcept;
    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept { return m_state.ComputeAuditHash(); }

private:
    WindowState m_state{};
    SDL_Window* m_window{nullptr};
    DpiScaleCallbackFn m_dpiCallback{nullptr};
    void* m_dpiUserCtx{nullptr};
    u32 m_windowId{0};
    u32 m_dpiMigrationCount{0};
    bool m_cursorVisible{true};
    bool m_textInputActive{false};
};

} // namespace en

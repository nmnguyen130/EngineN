#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/PlatformTypes.h"

struct SDL_Window;
union SDL_Event;

namespace EngineN {

struct WindowConfig {
    const char* title = "EngineN";
    i32 width = 1280;
    i32 height = 720;
    WindowMode mode = WindowMode::Windowed;
    bool highDPI = true;      // SDL_WINDOW_HIGH_PIXEL_DENSITY
    bool resizable = true;
};

/// RAII C++20 wrapper around SDL_Window*.
/// Handles display modes, High-DPI, and window lifecycle events.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    /// Create the SDL window. Platform must be initialized first.
    [[nodiscard]] Result<void> Create(const WindowConfig& config = {});

    /// Destroy the window. Safe to call multiple times.
    void Destroy();

    [[nodiscard]] bool IsValid() const noexcept { return m_window != nullptr; }

    // Display modes
    [[nodiscard]] Result<void> SetMode(WindowMode mode);
    [[nodiscard]] WindowMode GetMode() const noexcept { return m_mode; }

    // Sizes
    [[nodiscard]] i32 GetWidth() const noexcept { return m_logicalWidth; }
    [[nodiscard]] i32 GetHeight() const noexcept { return m_logicalHeight; }
    [[nodiscard]] i32 GetPixelWidth() const noexcept { return m_pixelWidth; }
    [[nodiscard]] i32 GetPixelHeight() const noexcept { return m_pixelHeight; }
    [[nodiscard]] f32 GetDPIScale() const noexcept;

    // Window states
    [[nodiscard]] bool HasFocus() const noexcept { return m_hasFocus; }
    [[nodiscard]] bool IsMinimized() const noexcept { return m_isMinimized; }

    void SetTitle(const char* title);

    // Raw handle access
    [[nodiscard]] SDL_Window* GetRawHandle() noexcept { return m_window; }
    [[nodiscard]] const SDL_Window* GetRawHandle() const noexcept { return m_window; }

    // Event handling (called by EventPump)
    /// Returns true if a window close was requested.
    bool HandleEvent(const SDL_Event& event);

private:
    void RefreshSizes();

    SDL_Window* m_window = nullptr;
    WindowMode m_mode = WindowMode::Windowed;
    i32 m_logicalWidth = 0;
    i32 m_logicalHeight = 0;
    i32 m_pixelWidth = 0;
    i32 m_pixelHeight = 0;
    bool m_hasFocus = true;
    bool m_isMinimized = false;
};

} // namespace EngineN

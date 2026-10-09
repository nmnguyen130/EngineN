#include "platform/Window.h"
#include "core/Log.h"
#include <SDL3/SDL.h>

namespace EngineN {

Window::~Window() {
    Destroy();
}

Window::Window(Window&& other) noexcept
    : m_window(other.m_window),
      m_mode(other.m_mode),
      m_logicalWidth(other.m_logicalWidth),
      m_logicalHeight(other.m_logicalHeight),
      m_pixelWidth(other.m_pixelWidth),
      m_pixelHeight(other.m_pixelHeight),
      m_hasFocus(other.m_hasFocus),
      m_isMinimized(other.m_isMinimized) {
    other.m_window = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_window = other.m_window;
        m_mode = other.m_mode;
        m_logicalWidth = other.m_logicalWidth;
        m_logicalHeight = other.m_logicalHeight;
        m_pixelWidth = other.m_pixelWidth;
        m_pixelHeight = other.m_pixelHeight;
        m_hasFocus = other.m_hasFocus;
        m_isMinimized = other.m_isMinimized;
        other.m_window = nullptr;
    }
    return *this;
}

Result<void> Window::Create(const WindowConfig& config) {
    if (m_window) {
        Destroy();
    }

    SDL_WindowFlags flags = 0;
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.highDPI) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }

    m_window = SDL_CreateWindow(config.title, config.width, config.height, flags);
    if (!m_window) {
        ENGINE_LOG(Error, Platform, "SDL_CreateWindow failed: {}", SDL_GetError());
        return Err(EngineError(SDL_GetError()));
    }

    m_mode = WindowMode::Windowed;
    RefreshSizes();

    if (config.mode != WindowMode::Windowed) {
        auto modeRes = SetMode(config.mode);
        if (!modeRes) {
            ENGINE_LOG(Warn, Platform, "Failed to set requested window mode, falling back to windowed");
        }
    }

    ENGINE_LOG(Info, Platform, "Window created: {} (Logical: {}x{}, Pixel: {}x{}, Scale: {:.2f})",
               config.title, m_logicalWidth, m_logicalHeight, m_pixelWidth, m_pixelHeight, GetDisplayScale());
    return Ok;
}

void Window::Destroy() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        m_logicalWidth = 0;
        m_logicalHeight = 0;
        m_pixelWidth = 0;
        m_pixelHeight = 0;
        m_mode = WindowMode::Windowed;
    }
}

Result<void> Window::SetMode(WindowMode mode) {
    if (!m_window) {
        return Err(EngineError("Window not created"));
    }

    bool success = true;
    switch (mode) {
        case WindowMode::Windowed: {
            // Revert fullscreen flag to return to standard windowed mode
            success = SDL_SetWindowFullscreen(m_window, false);
            break;
        }

        case WindowMode::BorderlessFullscreen: {
            // According to SDL3 specifications, mode == nullptr sets borderless desktop fullscreen
            if (!SDL_SetWindowFullscreenMode(m_window, nullptr)) {
                ENGINE_LOG(Warn, Platform, "SDL_SetWindowFullscreenMode(null) failed: {}", SDL_GetError());
            }
            success = SDL_SetWindowFullscreen(m_window, true);
            break;
        }

        case WindowMode::ExclusiveFullscreen: {
            // Query the current display that contains this window
            SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_window);
            if (displayId == 0) {
                displayId = SDL_GetPrimaryDisplay();
            }

            // Find closest matching exclusive display mode
            SDL_DisplayMode closest{};
            bool found = SDL_GetClosestFullscreenDisplayMode(
                displayId,
                m_logicalWidth,
                m_logicalHeight,
                0.0f,
                true,
                &closest);

            if (found) {
                if (!SDL_SetWindowFullscreenMode(m_window, &closest)) {
                    ENGINE_LOG(Warn, Platform, "Failed to set exclusive mode, falling back to desktop mode");
                    SDL_SetWindowFullscreenMode(m_window, nullptr);
                }
            } else {
                // Desktop fallback if no exact mode matches
                ENGINE_LOG(Warn, Platform, "No closest exclusive mode found; using desktop mode fallback");
                const SDL_DisplayMode* desktopMode = SDL_GetDesktopDisplayMode(displayId);
                if (!SDL_SetWindowFullscreenMode(m_window, desktopMode)) {
                    SDL_SetWindowFullscreenMode(m_window, nullptr);
                }
            }

            success = SDL_SetWindowFullscreen(m_window, true);
            if (!success) {
                // Desktop fallback if hardware mode switch fails (e.g. unsupported resolution or driver rejection)
                ENGINE_LOG(Warn, Platform, "Exclusive mode switch failed ({}); falling back to borderless fullscreen", SDL_GetError());
                SDL_SetWindowFullscreenMode(m_window, nullptr);
                success = SDL_SetWindowFullscreen(m_window, true);
            }
            break;
        }
    }

    if (!success) {
        ENGINE_LOG(Error, Platform, "SDL_SetWindowFullscreen failed: {}", SDL_GetError());
        return Err(EngineError(SDL_GetError()));
    }

    // Synchronize window state with OS compositor
    SDL_SyncWindow(m_window);

    m_mode = mode;
    RefreshSizes();
    return Ok;
}

Result<void> Window::SetExclusiveDisplayMode(i32 width, i32 height, f32 refreshRate) {
    if (!m_window) {
        return Err(EngineError("Window not created"));
    }

    SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_window);
    if (displayId == 0) {
        displayId = SDL_GetPrimaryDisplay();
    }

    SDL_DisplayMode closest{};
    if (!SDL_GetClosestFullscreenDisplayMode(displayId, width, height, refreshRate, true, &closest)) {
        return Err(EngineError("No matching display mode found for requested parameters"));
    }

    if (!SDL_SetWindowFullscreenMode(m_window, &closest)) {
        return Err(EngineError(SDL_GetError()));
    }

    if (m_mode == WindowMode::ExclusiveFullscreen) {
        SDL_SyncWindow(m_window);
        RefreshSizes();
    }

    return Ok;
}

const SDL_DisplayMode* Window::GetFullscreenMode() const {
    if (!m_window) return nullptr;
    return SDL_GetWindowFullscreenMode(m_window);
}

f32 Window::GetDisplayScale() const noexcept {
    if (!m_window) return 1.0f;
    float scale = SDL_GetWindowDisplayScale(m_window);
    return (scale > 0.0f) ? scale : 1.0f;
}

f32 Window::GetPixelDensity() const noexcept {
    if (m_logicalWidth <= 0) return 1.0f;
    return static_cast<f32>(m_pixelWidth) / static_cast<f32>(m_logicalWidth);
}

u32 Window::GetDisplayID() const {
    if (!m_window) return 0;
    return SDL_GetDisplayForWindow(m_window);
}

f32 Window::GetRefreshRate() const {
    if (!m_window) return 60.0f;
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_window);
    if (displayId == 0) displayId = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
    if (!mode) mode = SDL_GetDesktopDisplayMode(displayId);
    return (mode && mode->refresh_rate > 0.0f) ? mode->refresh_rate : 60.0f;
}

void Window::SetTitle(const char* title) {
    if (m_window && title) {
        SDL_SetWindowTitle(m_window, title);
    }
}

void Window::RefreshSizes() {
    if (!m_window) return;
    SDL_GetWindowSize(m_window, &m_logicalWidth, &m_logicalHeight);
    SDL_GetWindowSizeInPixels(m_window, &m_pixelWidth, &m_pixelHeight);
}

void Window::ProcessEvent(const SDL_Event& event) {
    (void)HandleEvent(event);
}

bool Window::HandleEvent(const SDL_Event& event) {
    if (!m_window) return false;

    switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_hasFocus = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_hasFocus = false;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            m_isMinimized = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            m_isMinimized = false;
            RefreshSizes();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            RefreshSizes();
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            RefreshSizes();
            break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            m_mode = WindowMode::Windowed;
            RefreshSizes();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return true;
        default:
            break;
    }

    return false;
}

} // namespace EngineN

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

    ENGINE_LOG(Info, Platform, "Window created: {} (Logical: {}x{}, Pixel: {}x{})",
               config.title, m_logicalWidth, m_logicalHeight, m_pixelWidth, m_pixelHeight);
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
    }
}

Result<void> Window::SetMode(WindowMode mode) {
    if (!m_window) {
        return Err(EngineError("Window not created"));
    }

    bool success = true;
    switch (mode) {
        case WindowMode::Windowed:
            success = SDL_SetWindowFullscreen(m_window, false);
            break;
        case WindowMode::BorderlessFullscreen:
            success = SDL_SetWindowFullscreen(m_window, true);
            break;
        case WindowMode::ExclusiveFullscreen:
            success = SDL_SetWindowFullscreen(m_window, true);
            break;
    }

    if (!success) {
        ENGINE_LOG(Error, Platform, "SDL_SetWindowFullscreen failed: {}", SDL_GetError());
        return Err(EngineError(SDL_GetError()));
    }

    m_mode = mode;
    RefreshSizes();
    return Ok;
}

f32 Window::GetDPIScale() const noexcept {
    if (m_logicalWidth <= 0) return 1.0f;
    return static_cast<f32>(m_pixelWidth) / static_cast<f32>(m_logicalWidth);
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

#include "platform/Window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <utility>

namespace en::platform {

Window::~Window() noexcept {
    Destroy();
}

Window::Window(Window&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr))
    , m_id(std::exchange(other.m_id, 0))
    , m_state(other.m_state) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_id = std::exchange(other.m_id, 0);
        m_state = other.m_state;
    }
    return *this;
}

Result<void, EngineError> Window::Create(const WindowConfig& cfg) noexcept {
    Destroy();

    SDL_WindowFlags flags = 0;
    if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.highDpi)   flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

    switch (cfg.api) {
        case GraphicsApi::OpenGL: flags |= SDL_WINDOW_OPENGL; break;
        case GraphicsApi::Vulkan: flags |= SDL_WINDOW_VULKAN; break;
        case GraphicsApi::Metal:  flags |= SDL_WINDOW_METAL;  break;
        case GraphicsApi::None:   break;
    }

    FixedString<256> title(cfg.title);
    const i32 w = cfg.size.w > 0 ? cfg.size.w : 1280;
    const i32 h = cfg.size.h > 0 ? cfg.size.h : 720;

    m_handle = SDL_CreateWindow(title.CStr(), w, h, flags);
    if (!m_handle) {
        return Unexpected(MakePlatformError(PlatformErrc::WindowCreateFailed));
    }

    if (cfg.minSize.w > 0 && cfg.minSize.h > 0) {
        (void)SDL_SetWindowMinimumSize(m_handle, cfg.minSize.w, cfg.minSize.h);
    }

    m_id = SDL_GetWindowID(m_handle);
    m_state.mode = WindowMode::Windowed;
    RefreshMetrics();

    if (cfg.mode != WindowMode::Windowed) {
        return SetMode(cfg.mode);
    }
    return Ok;
}

void Window::Destroy() noexcept {
    if (m_handle) {
        SDL_DestroyWindow(m_handle);
        m_handle = nullptr;
        m_id = 0;
        m_state = {};
    }
}

Result<void, EngineError> Window::SetMode(WindowMode mode) noexcept {
    if (!m_handle) return Unexpected(MakePlatformError(PlatformErrc::InvalidState));

    if (mode == WindowMode::Windowed) {
        if (!SDL_SetWindowFullscreen(m_handle, false)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
    } else if (mode == WindowMode::Borderless) {
        (void)SDL_SetWindowFullscreenMode(m_handle, nullptr);
        if (!SDL_SetWindowFullscreen(m_handle, true)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
    } else if (mode == WindowMode::ExclusiveFullscreen) {
        const SDL_DisplayID disp = SDL_GetDisplayForWindow(m_handle);
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(disp);
        const i32 targetW = desktop ? desktop->w : m_state.pixelSize.w;
        const i32 targetH = desktop ? desktop->h : m_state.pixelSize.h;

        SDL_DisplayMode closest{};
        if (SDL_GetClosestFullscreenDisplayMode(disp, targetW, targetH, 0.0f, true, &closest)) {
            (void)SDL_SetWindowFullscreenMode(m_handle, &closest);
        }
        if (!SDL_SetWindowFullscreen(m_handle, true)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
    }

    (void)SDL_SyncWindow(m_handle);
    m_state.mode = mode;
    RefreshMetrics();
    return Ok;
}

Result<void, EngineError> Window::SetTitle(std::string_view title) noexcept {
    if (!m_handle) return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    FixedString<256> buf(title);
    if (!SDL_SetWindowTitle(m_handle, buf.CStr())) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    return Ok;
}

Result<void, EngineError> Window::SetRelativeMouse(bool enabled) noexcept {
    if (!m_handle || !SDL_SetWindowRelativeMouseMode(m_handle, enabled)) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    return Ok;
}

void Window::SetCursorVisible(bool visible) noexcept {
    if (visible) (void)SDL_ShowCursor();
    else         (void)SDL_HideCursor();
}

void Window::SetTextInput(bool enabled) noexcept {
    if (!m_handle) return;
    if (enabled) (void)SDL_StartTextInput(m_handle);
    else         (void)SDL_StopTextInput(m_handle);
}

void Window::RefreshMetrics() noexcept {
    if (!m_handle) return;

    SDL_GetWindowSize(m_handle, &m_state.logicalSize.w, &m_state.logicalSize.h);
    SDL_GetWindowSizeInPixels(m_handle, &m_state.pixelSize.w, &m_state.pixelSize.h);

    m_state.pixelDensity = SDL_GetWindowPixelDensity(m_handle);
    if (m_state.pixelDensity <= 0.0f) m_state.pixelDensity = 1.0f;

    m_state.refreshHz = 60.0f;
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(m_handle);
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(disp)) {
        if (dm->refresh_rate > 0.0f) {
            m_state.refreshHz = dm->refresh_rate;
        }
    }

    const SDL_WindowFlags f = SDL_GetWindowFlags(m_handle);
    m_state.focused   = (f & SDL_WINDOW_INPUT_FOCUS) != 0;
    m_state.suspended = (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) != 0;
}

WindowFlags Window::ProcessEvent(const SDL_Event& ev) noexcept {
    if (!m_handle || ev.window.windowID != m_id) {
        return WindowFlags::None;
    }

    switch (ev.type) {
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            RefreshMetrics();
            return WindowFlags::Resized;

        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            m_state.mode = WindowMode::Windowed;
            RefreshMetrics();
            return WindowFlags::Resized;

        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            RefreshMetrics();
            return WindowFlags::DisplayChanged | WindowFlags::Resized;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_state.focused = true;
            return WindowFlags::FocusGained;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_state.focused = false;
            return WindowFlags::FocusLost;

        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_OCCLUDED:
        case SDL_EVENT_WINDOW_HIDDEN:
            m_state.suspended = true;
            return WindowFlags::None;

        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN: {
            const SDL_WindowFlags f = SDL_GetWindowFlags(m_handle);
            m_state.suspended = (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) != 0;
            return WindowFlags::None;
        }

        default:
            return WindowFlags::None;
    }
}

} // namespace en::platform
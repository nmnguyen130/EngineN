#include "platform/Window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <utility>

namespace en::platform {

namespace {

[[nodiscard]] bool ApplyWindowMode(SDL_Window* handle, WindowMode mode, Extent2i fallbackSize) noexcept {
    if (mode != WindowMode::ExclusiveFullscreen) {
        (void)SDL_SetWindowFullscreenMode(handle, nullptr);
        return SDL_SetWindowFullscreen(handle, mode == WindowMode::Borderless);
    }

    const SDL_DisplayID disp = SDL_GetDisplayForWindow(handle);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(disp);
    const i32 targetW = desktop ? desktop->w : fallbackSize.w;
    const i32 targetH = desktop ? desktop->h : fallbackSize.h;

    SDL_DisplayMode closest{};
    if (SDL_GetClosestFullscreenDisplayMode(disp, targetW, targetH, 0.0f, true, &closest)) {
        (void)SDL_SetWindowFullscreenMode(handle, &closest);
    }
    return SDL_SetWindowFullscreen(handle, true);
}

} // namespace

Window::~Window() noexcept {
    Destroy();
}

Window::Window(Window&& other) noexcept
    : m_state(std::exchange(other.m_state, WindowState{}))
    , m_handle(std::exchange(other.m_handle, nullptr))
    , m_id(std::exchange(other.m_id, 0)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_state  = std::exchange(other.m_state, WindowState{});
        m_handle = std::exchange(other.m_handle, nullptr);
        m_id     = std::exchange(other.m_id, 0);
    }
    return *this;
}

Result<void, PlatformErrc> Window::Create(const WindowConfig& cfg) noexcept {
    Destroy();

    SDL_WindowFlags flags = 0;
    if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.highDpi)   flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.mode == WindowMode::Borderless) flags |= SDL_WINDOW_FULLSCREEN;

    switch (cfg.api) {
        case GraphicsApi::OpenGL: flags |= SDL_WINDOW_OPENGL; break;
        case GraphicsApi::Vulkan: flags |= SDL_WINDOW_VULKAN; break;
        case GraphicsApi::Metal:  flags |= SDL_WINDOW_METAL;  break;
        case GraphicsApi::None:   break;
    }

    const FixedString<256> title(cfg.title);
    const i32 w = cfg.size.w > 0 ? cfg.size.w : 1280;
    const i32 h = cfg.size.h > 0 ? cfg.size.h : 720;

    m_handle = SDL_CreateWindow(title.CStr(), w, h, flags);
    if (!m_handle) {
        return Err(PlatformErrc::WindowCreateFailed);
    }

    if (cfg.minSize.IsValid()) {
        (void)SDL_SetWindowMinimumSize(m_handle, cfg.minSize.w, cfg.minSize.h);
    }

    m_id = SDL_GetWindowID(m_handle);
    m_state.mode = (cfg.mode == WindowMode::Borderless) ? WindowMode::Borderless : WindowMode::Windowed;

    if (cfg.mode == WindowMode::ExclusiveFullscreen) {
        if (!ApplyWindowMode(m_handle, WindowMode::ExclusiveFullscreen, Extent2i{w, h})) {
            Destroy();
            return Err(PlatformErrc::FullscreenFailed);
        }
        (void)SDL_SyncWindow(m_handle);
        m_state.mode = WindowMode::ExclusiveFullscreen;
    }

    RefreshMetrics();
    return {};
}

void Window::Destroy() noexcept {
    if (m_handle) {
        SDL_DestroyWindow(m_handle);
        m_handle = nullptr;
        m_id = 0;
        m_state = {};
    }
}

Result<void, PlatformErrc> Window::SetMode(WindowMode mode) noexcept {
    if (!m_handle) return Err(PlatformErrc::InvalidState);
    if (m_state.mode == mode) return {};

    if (!ApplyWindowMode(m_handle, mode, m_state.pixelSize)) {
        return Err(PlatformErrc::FullscreenFailed);
    }

    (void)SDL_SyncWindow(m_handle);
    m_state.mode = mode;
    RefreshMetrics();
    return {};
}

Result<void, PlatformErrc> Window::SetTitle(std::string_view title) noexcept {
    if (!m_handle) return Err(PlatformErrc::InvalidState);
    const FixedString<256> buf(title);
    if (!SDL_SetWindowTitle(m_handle, buf.CStr())) {
        return Err(PlatformErrc::InvalidState);
    }
    return {};
}

Result<void, PlatformErrc> Window::SetRelativeMouse(bool enabled) noexcept {
    if (!m_handle) return Err(PlatformErrc::InvalidState);
    if (m_state.relativeMouse == enabled) return {};
    if (!SDL_SetWindowRelativeMouseMode(m_handle, enabled)) {
        return Err(PlatformErrc::InvalidState);
    }
    m_state.relativeMouse = enabled;
    return {};
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

    const f32 density = SDL_GetWindowPixelDensity(m_handle);
    m_state.pixelDensity = (density > 0.0f) ? density : 1.0f;

    const f32 dispScale = SDL_GetWindowDisplayScale(m_handle);
    m_state.displayScale = (dispScale > 0.0f) ? dispScale : m_state.pixelDensity;

    m_state.refreshHz = 60.0f;
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_handle))) {
        if (dm->refresh_rate > 0.0f) {
            m_state.refreshHz = dm->refresh_rate;
        }
    }

    const SDL_WindowFlags f = SDL_GetWindowFlags(m_handle);
    m_state.focused       = (f & SDL_WINDOW_INPUT_FOCUS) != 0;
    m_state.suspended     = (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) != 0;
    m_state.relativeMouse = SDL_GetWindowRelativeMouseMode(m_handle);
}

WindowFlags Window::ProcessEvent(const SDL_Event& ev, bool& outNeedRefresh) noexcept {
    if (!m_handle || ev.window.windowID != m_id) {
        return WindowFlags::None;
    }

    switch (ev.type) {
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            if (m_state.mode == WindowMode::Windowed) m_state.mode = WindowMode::Borderless;
            outNeedRefresh = true;
            return WindowFlags::Resized;

        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            m_state.mode = WindowMode::Windowed;
            outNeedRefresh = true;
            return WindowFlags::Resized;

        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            outNeedRefresh = true;
            return WindowFlags::Resized;

        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            outNeedRefresh = true;
            return WindowFlags::DisplayChanged | WindowFlags::Resized;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_state.focused = true;
            outNeedRefresh = true;
            return WindowFlags::FocusGained;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_state.focused = false;
            outNeedRefresh = true;
            return WindowFlags::FocusLost;

        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_OCCLUDED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN:
            outNeedRefresh = true;
            return WindowFlags::None;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return WindowFlags::CloseRequested;

        default:
            return WindowFlags::None;
    }
}

} // namespace en::platform

#include "platform/Window.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <utility>

namespace EngineN {

Window::~Window() {
    Destroy();
}

Window::Window(Window&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr))
    , m_id(std::exchange(other.m_id, {}))
    , m_state(other.m_state)
    , m_dirty(other.m_dirty)
{
    other.m_dirty = WindowChange::None;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_id = std::exchange(other.m_id, {});
        m_state = other.m_state;
        m_dirty = other.m_dirty;
        other.m_dirty = WindowChange::None;
    }
    return *this;
}

Result<Window, EngineError> Window::Create(
    const WindowConfig& config,
    const DisplayTopology& displays) noexcept
{
    Window win;
    auto res = win.Create(config);
    if (!res) {
        return Unexpected(res.Error());
    }

    if (config.mode != WindowMode::Windowed) {
        (void)win.SetMode(WindowModeRequest{.mode = config.mode}, displays);
    }

    return win;
}

Result<void, EngineError> Window::Create(const WindowConfig& config) noexcept {
    Destroy();

    const i32 width = (config.size.w > 0) ? config.size.w : 1280;
    const i32 height = (config.size.h > 0) ? config.size.h : 720;

    SDL_WindowFlags flags = 0;
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.highDPI) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (config.startHidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (config.startMaximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }

    switch (config.graphics) {
        case WindowGraphicsHint::OpenGL: flags |= SDL_WINDOW_OPENGL; break;
        case WindowGraphicsHint::Vulkan: flags |= SDL_WINDOW_VULKAN; break;
        case WindowGraphicsHint::Metal:  flags |= SDL_WINDOW_METAL;  break;
        case WindowGraphicsHint::None:   break;
    }

    FixedString<256> titleBuf(config.title);
    m_handle = SDL_CreateWindow(titleBuf.CStr(), width, height, flags);
    if (!m_handle) {
        return Unexpected(MakePlatformError(PlatformErrc::WindowCreateFailed));
    }

    if (config.minSize.w > 0 && config.minSize.h > 0) {
        (void)SDL_SetWindowMinimumSize(m_handle, config.minSize.w, config.minSize.h);
    }

    m_id = WindowId{SDL_GetWindowID(m_handle)};
    m_state.mode = WindowMode::Windowed;
    m_dirty = WindowChange::Size | WindowChange::PixelSize | WindowChange::DisplayScale;

    Resync();
    return Ok;
}

void Window::Destroy() noexcept {
    if (m_handle) {
        SDL_DestroyWindow(m_handle);
        m_handle = nullptr;
        m_id = {};
        m_state = {};
        m_dirty = WindowChange::None;
    }
}

Result<void, EngineError> Window::SetMode(
    const WindowModeRequest& request,
    const DisplayTopology& displays) noexcept
{
    if (!m_handle) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState, "Window handle is null"));
    }

    if (request.mode == WindowMode::Windowed) {
        if (!SDL_SetWindowFullscreen(m_handle, false)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
        m_state.mode = WindowMode::Windowed;
    } else if (request.mode == WindowMode::BorderlessFullscreen) {
        (void)SDL_SetWindowFullscreenMode(m_handle, nullptr);
        if (!SDL_SetWindowFullscreen(m_handle, true)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
        m_state.mode = WindowMode::BorderlessFullscreen;
    } else if (request.mode == WindowMode::ExclusiveFullscreen) {
        auto closest = displays.FindClosestMode(m_state.display, request.exclusive);
        if (closest) {
            SDL_DisplayMode sdlMode{
                .displayID = m_state.display.value,
                .format = static_cast<SDL_PixelFormat>(closest->sdlPixelFormat),
                .w = closest->width,
                .h = closest->height,
                .pixel_density = closest->pixelDensity,
                .refresh_rate = closest->refreshHz,
                .refresh_rate_numerator = static_cast<int>(closest->refreshNumerator),
                .refresh_rate_denominator = static_cast<int>(closest->refreshDenominator),
                .internal = nullptr
            };
            (void)SDL_SetWindowFullscreenMode(m_handle, &sdlMode);
        }
        if (!SDL_SetWindowFullscreen(m_handle, true)) {
            return Unexpected(MakePlatformError(PlatformErrc::FullscreenFailed));
        }
        m_state.mode = WindowMode::ExclusiveFullscreen;
    }

    m_state.transitionPending = true;
    m_dirty |= WindowChange::Mode;
    return Ok;
}

Result<void, EngineError> Window::SetMode(WindowMode mode) noexcept {
    static const DisplayTopology emptyTopology{};
    return SetMode(WindowModeRequest{.mode = mode}, emptyTopology);
}

Result<void, EngineError> Window::SetTitle(std::string_view title) noexcept {
    if (!m_handle) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    FixedString<256> buf(title);
    if (!SDL_SetWindowTitle(m_handle, buf.CStr())) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    return Ok;
}

Result<void, EngineError> Window::SetMinimumSize(Extent2i size) noexcept {
    if (!m_handle) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    if (!SDL_SetWindowMinimumSize(m_handle, size.w, size.h)) {
        return Unexpected(MakePlatformError(PlatformErrc::InvalidState));
    }
    return Ok;
}

void Window::Show() noexcept {
    if (m_handle) {
        (void)SDL_ShowWindow(m_handle);
    }
}

void Window::RequestAttention() noexcept {
    if (m_handle) {
        (void)SDL_FlashWindow(m_handle, SDL_FLASH_UNTIL_FOCUSED);
    }
}

void Window::SetCursorVisible(bool visible) noexcept {
    if (visible) {
        (void)SDL_ShowCursor();
    } else {
        (void)SDL_HideCursor();
    }
}

void Window::Resync() noexcept {
    if (!m_handle) return;

    int w = 0, h = 0;
    (void)SDL_GetWindowSize(m_handle, &w, &h);
    m_state.metrics.logicalSize = {w, h};

    int pw = 0, ph = 0;
    (void)SDL_GetWindowSizeInPixels(m_handle, &pw, &ph);
    m_state.metrics.pixelSize = {pw, ph};

    m_state.metrics.pixelDensity = SDL_GetWindowPixelDensity(m_handle);
    m_state.metrics.displayScale = SDL_GetWindowDisplayScale(m_handle);

    int x = 0, y = 0;
    (void)SDL_GetWindowPosition(m_handle, &x, &y);
    m_state.position = {x, y};

    m_state.display = DisplayId{SDL_GetDisplayForWindow(m_handle)};

    const SDL_WindowFlags flags = SDL_GetWindowFlags(m_handle);
    m_state.focused   = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    m_state.minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
    m_state.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    m_state.occluded  = (flags & SDL_WINDOW_OCCLUDED) != 0;
    m_state.visible   = (flags & SDL_WINDOW_HIDDEN) == 0;
}

void Window::ApplyEvent(const SDL_Event& event) noexcept {
    if (!m_handle || event.window.windowID != m_id.value) return;

    switch (event.type) {
        case SDL_EVENT_WINDOW_RESIZED:
            m_state.metrics.logicalSize = {event.window.data1, event.window.data2};
            m_dirty |= WindowChange::Size;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_state.metrics.pixelSize = {event.window.data1, event.window.data2};
            m_dirty |= WindowChange::PixelSize;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            m_state.metrics.displayScale = SDL_GetWindowDisplayScale(m_handle);
            m_state.metrics.pixelDensity = SDL_GetWindowPixelDensity(m_handle);
            m_dirty |= (WindowChange::DisplayScale | WindowChange::PixelDensity);
            break;
        case SDL_EVENT_WINDOW_MOVED:
            m_state.position = {event.window.data1, event.window.data2};
            m_dirty |= WindowChange::Position;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            m_state.display = DisplayId{static_cast<u32>(event.window.data1)};
            m_dirty |= WindowChange::Display;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_state.focused = true;
            m_dirty |= WindowChange::Focus;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_state.focused = false;
            m_dirty |= WindowChange::Focus;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            m_state.minimized = true;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
            m_state.maximized = true;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            m_state.minimized = false;
            m_state.maximized = false;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_SHOWN:
            m_state.visible = true;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
            m_state.visible = false;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_OCCLUDED:
            m_state.occluded = true;
            m_dirty |= WindowChange::Visibility;
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            m_state.transitionPending = false;
            m_dirty |= WindowChange::Mode;
            break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            m_state.transitionPending = false;
            m_state.mode = WindowMode::Windowed;
            m_dirty |= WindowChange::Mode;
            break;
        default:
            break;
    }
}

} // namespace EngineN

#include "platform/Window.h"
#include <SDL3/SDL.h>

namespace en {

Result<void, PlatformErrc> Window::Create(const WindowConfig& cfg) noexcept {
    Destroy();
    if (!cfg.size.IsValid()) return Err(PlatformErrc::InvalidState);

    u64 sdlFlags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.resizable) sdlFlags |= SDL_WINDOW_RESIZABLE;
    if (cfg.mode != WindowMode::Windowed) sdlFlags |= SDL_WINDOW_FULLSCREEN;

    FixedString<128> titleBuf(cfg.title);
    m_window = SDL_CreateWindow(titleBuf.CStr(), cfg.size.w, cfg.size.h, sdlFlags);
    if (!m_window) return Err(PlatformErrc::WindowCreateFailed);

    m_windowId = SDL_GetWindowID(m_window);
    m_state.logicalSize = cfg.size;
    m_state.pixelSize = cfg.size;
    m_state.mode = cfg.mode;
    m_state.focused = true;
    m_state.suspended = false;
    RefreshMetrics();
    return {};
}

void Window::Destroy() noexcept {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    m_windowId = 0;
}

Result<void, PlatformErrc> Window::SetMode(WindowMode mode) noexcept {
    if (m_state.mode == mode) return {};
    if (m_window) {
        const bool full = (mode != WindowMode::Windowed);
        if (!SDL_SetWindowFullscreen(m_window, full)) {
            return Err(PlatformErrc::FullscreenFailed);
        }
    }
    m_state.mode = mode;
    RefreshMetrics();
    return {};
}

Result<void, PlatformErrc> Window::SetTitle(std::string_view title) noexcept {
    if (!m_window) return Err(PlatformErrc::InvalidState);
    FixedString<128> titleBuf(title);
    SDL_SetWindowTitle(m_window, titleBuf.CStr());
    return {};
}

Result<void, PlatformErrc> Window::SetRelativeMouse(bool enabled) noexcept {
    if (m_state.relativeMouse == enabled) return {};
    if (m_window) {
        SDL_SetWindowRelativeMouseMode(m_window, enabled);
    }
    m_state.relativeMouse = enabled;
    return {};
}

void Window::SetCursorVisible(bool visible) noexcept {
    if (m_cursorVisible == visible) return;
    m_cursorVisible = visible;
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

void Window::SetTextInput(bool enabled) noexcept {
    if (m_textInputActive == enabled) return;
    m_textInputActive = enabled;
    if (!m_window) return;
    if (enabled) SDL_StartTextInput(m_window);
    else SDL_StopTextInput(m_window);
}

WindowFlags Window::ProcessEvent(const SDL_Event& ev, bool& outNeedRefresh) noexcept {
    WindowFlags flags = WindowFlags::None;
    switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            flags |= WindowFlags::CloseRequested;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            flags |= WindowFlags::Resized;
            outNeedRefresh = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_state.focused = true;
            flags |= WindowFlags::FocusGained;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_state.focused = false;
            flags |= WindowFlags::FocusLost;
            break;
        default:
            break;
    }
    return flags;
}

void Window::RefreshMetrics() noexcept {
    if (!m_window) return;
    int lw = m_state.logicalSize.w;
    int lh = m_state.logicalSize.h;
    int pw = m_state.pixelSize.w;
    int ph = m_state.pixelSize.h;
    SDL_GetWindowSize(m_window, &lw, &lh);
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
    if (lw > 0 && lh > 0) m_state.logicalSize = {lw, lh};
    if (pw > 0 && ph > 0) m_state.pixelSize = {pw, ph};
    const f32 density = SDL_GetWindowPixelDensity(m_window);
    const f32 prevDensity = m_state.pixelDensity;
    m_state.pixelDensity = density > 0.0f ? density : 1.0f;
    if (std::abs(m_state.pixelDensity - prevDensity) > 1e-3f) {
        ++m_dpiMigrationCount;
        if (m_dpiCallback) {
            m_dpiCallback(m_state.pixelDensity, m_state.logicalSize, m_state.pixelSize, m_dpiUserCtx);
        }
    }
}

void Window::SimulateDpiMigration(f32 newPixelDensity, Extent2i newLogicalSize) noexcept {
    m_state.logicalSize = newLogicalSize;
    m_state.pixelDensity = std::max(0.5f, newPixelDensity);
    m_state.pixelSize = {
        static_cast<i32>(static_cast<f32>(newLogicalSize.w) * m_state.pixelDensity + 0.5f),
        static_cast<i32>(static_cast<f32>(newLogicalSize.h) * m_state.pixelDensity + 0.5f),
    };
    ++m_dpiMigrationCount;
    if (m_dpiCallback) {
        m_dpiCallback(m_state.pixelDensity, m_state.logicalSize, m_state.pixelSize, m_dpiUserCtx);
    }
}

} // namespace en

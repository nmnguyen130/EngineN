#include "platform/Window.h"

#include <SDL3/SDL.h>

namespace en {

Result<void, PlatformErrc> Window::Create(const WindowConfig& cfg, bool headless) noexcept {
    Destroy();
    if (!cfg.size.IsValid()) return Err(PlatformErrc::InvalidState);

    m_state = WindowState{
        .logicalSize = cfg.size,
        .pixelSize = cfg.size,
        .refreshHz = cfg.targetRefreshHz > 0.0f ? cfg.targetRefreshHz : 60.0f,
        .mode = cfg.mode,
    };
    m_cursorVisible = true;
    m_textInputActive = false;

    if (headless) {
        m_windowId = 1U;
        return {};
    }

    SDL_WindowFlags sdlFlags = 0ULL;
    if (cfg.highDpi) sdlFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.resizable) sdlFlags |= SDL_WINDOW_RESIZABLE;
    if (cfg.mode != WindowMode::Windowed) sdlFlags |= SDL_WINDOW_FULLSCREEN;

    const FixedString<128> titleBuf(cfg.title);
    m_window = SDL_CreateWindow(titleBuf.CStr(), cfg.size.w, cfg.size.h, sdlFlags);
    if (!m_window) return Err(PlatformErrc::WindowCreateFailed);

    const u32 winId = static_cast<u32>(SDL_GetWindowID(m_window));
    m_windowId = winId != 0U ? winId : 1U;

    if (cfg.mode == WindowMode::ExclusiveFullscreen) {
        if (const auto res = SetMode(WindowMode::ExclusiveFullscreen, cfg.targetRefreshHz); !res.has_value()) {
            Destroy();
            return res;
        }
    }
    (void)RefreshMetrics();
    return {};
}

void Window::Destroy() noexcept {
    if (m_window) {
        if (m_textInputActive) (void)SDL_StopTextInput(m_window);
        if (m_state.relativeMouse) (void)SDL_SetWindowRelativeMouseMode(m_window, false);
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    m_windowId = 0U;
    m_cursorVisible = true;
    m_textInputActive = false;
    m_state.relativeMouse = false;
}

Result<void, PlatformErrc> Window::SetMode(WindowMode mode, f32 targetRefreshHz) noexcept {
    if (!IsValid()) return Err(PlatformErrc::InvalidState);
    if (m_state.mode == mode && targetRefreshHz <= 0.0f) return {};

    if (m_window) {
        const SDL_DisplayMode* fsMode = nullptr;
        SDL_DisplayMode closest{};
        if (mode == WindowMode::ExclusiveFullscreen) {
            const SDL_DisplayID dispId = SDL_GetDisplayForWindow(m_window);
            const f32 reqHz = targetRefreshHz > 0.0f ? targetRefreshHz : m_state.refreshHz;
            if (dispId != 0U && SDL_GetClosestFullscreenDisplayMode(
                    dispId, m_state.pixelSize.w, m_state.pixelSize.h, reqHz, true, &closest)) {
                fsMode = &closest;
            }
        }
        if (!SDL_SetWindowFullscreenMode(m_window, fsMode) ||
            !SDL_SetWindowFullscreen(m_window, mode != WindowMode::Windowed)) {
            return Err(PlatformErrc::FullscreenFailed);
        }
    }
    m_state.mode = mode;
    if (targetRefreshHz > 0.0f) m_state.refreshHz = targetRefreshHz;
    (void)RefreshMetrics();
    return {};
}

void Window::SetTitle(std::string_view title) noexcept {
    if (m_window) {
        const FixedString<128> titleBuf(title);
        (void)SDL_SetWindowTitle(m_window, titleBuf.CStr());
    }
}

void Window::SetRelativeMouse(bool enabled) noexcept {
    if (!IsValid() || m_state.relativeMouse == enabled) return;
    if (!m_window || SDL_SetWindowRelativeMouseMode(m_window, enabled)) {
        m_state.relativeMouse = enabled;
    }
}

void Window::SetCursorVisible(bool visible) noexcept {
    if (!IsValid() || m_cursorVisible == visible) return;
    if (!m_window || (visible ? SDL_ShowCursor() : SDL_HideCursor())) {
        m_cursorVisible = visible;
    }
}

void Window::SetTextInput(bool enabled) noexcept {
    if (!IsValid() || m_textInputActive == enabled) return;
    if (!m_window || (enabled ? SDL_StartTextInput(m_window) : SDL_StopTextInput(m_window))) {
        m_textInputActive = enabled;
    }
}

void Window::WarpMouse(Vec2 pixelPos) noexcept {
    if (m_window) {
        const Vec2 winPos = m_state.PixelToWindow(pixelPos);
        SDL_WarpMouseInWindow(m_window, winPos.x, winPos.y);
    }
}

WindowFlags Window::RefreshMetrics() noexcept {
    if (!m_window) return WindowFlags::None;

    int lw = m_state.logicalSize.w;
    int lh = m_state.logicalSize.h;
    int pw = m_state.pixelSize.w;
    int ph = m_state.pixelSize.h;
    SDL_GetWindowSize(m_window, &lw, &lh);
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);

    const f32 density = SDL_GetWindowPixelDensity(m_window);
    const f32 displayScale = SDL_GetWindowDisplayScale(m_window);
    f32 refreshHz = m_state.refreshHz;

    if (const SDL_DisplayID dispId = SDL_GetDisplayForWindow(m_window); dispId != 0U) {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(dispId);
        if (!mode) mode = SDL_GetDesktopDisplayMode(dispId);
        if (mode && mode->refresh_rate > 0.0f) refreshHz = mode->refresh_rate;
    }

    return m_state.ApplyMetrics({lw, lh}, {pw, ph}, density, displayScale, refreshHz);
}

} // namespace en

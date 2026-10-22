#include "platform/Window.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

namespace en {

namespace {

SDL_Cursor* g_systemCursors[7]{};

void DestroyCachedCursors() noexcept {
    for (SDL_Cursor*& cur : g_systemCursors) {
        if (cur != nullptr) {
            SDL_DestroyCursor(cur);
            cur = nullptr;
        }
    }
}

} // namespace

Result<void, PlatformErrc> Window::Create(const WindowConfig& cfg, bool headless) noexcept {
    Destroy();
    if (!cfg.size.IsValid()) return Err(PlatformErrc::InvalidState);

    m_state = WindowState{};
    m_state.logicalSize = cfg.size;
    m_state.pixelSize = cfg.size;
    m_state.refreshHz = cfg.targetRefreshHz > 0.0f ? cfg.targetRefreshHz : 60.0f;
    m_state.mode = cfg.mode;
    m_cursorShape = CursorShape::Arrow;
    m_optionBits = kBitCursorVisible;

    if (headless) {
        m_windowId = 1U;
        return {};
    }

    SDL_WindowFlags sdlFlags = 0ULL;
    if (cfg.highDpi) sdlFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.resizable) sdlFlags |= SDL_WINDOW_RESIZABLE;
    if (cfg.mode != WindowMode::Windowed) sdlFlags |= SDL_WINDOW_FULLSCREEN;

    switch (cfg.api) {
        case GraphicsApi::OpenGL: sdlFlags |= SDL_WINDOW_OPENGL; break;
        case GraphicsApi::Vulkan: sdlFlags |= SDL_WINDOW_VULKAN; break;
        case GraphicsApi::Metal:  sdlFlags |= SDL_WINDOW_METAL;  break;
        case GraphicsApi::Auto:   break;
    }

    FixedString<128> titleBuf(cfg.title);
    m_window = SDL_CreateWindow(titleBuf.CStr(), cfg.size.w, cfg.size.h, sdlFlags);
    if (!m_window) return Err(PlatformErrc::WindowCreateFailed);

    m_windowId = static_cast<u32>(SDL_GetWindowID(m_window));
    if (m_windowId == 0U) m_windowId = 1U;

    if (cfg.minSize.IsValid()) {
        (void)SDL_SetWindowMinimumSize(m_window, cfg.minSize.w, cfg.minSize.h);
    }
    if (cfg.mode == WindowMode::ExclusiveFullscreen) {
        (void)SetMode(WindowMode::ExclusiveFullscreen, cfg.targetRefreshHz);
    }
    (void)RefreshMetrics();
    return {};
}

void Window::Destroy() noexcept {
    if (m_window) {
        if (IsTextInputActive()) {
            (void)SDL_StopTextInput(m_window);
        }
        if (m_state.relativeMouse) {
            (void)SDL_SetWindowRelativeMouseMode(m_window, false);
        }
        DestroyCachedCursors();
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    m_windowId = 0U;
    m_optionBits = kBitCursorVisible;
    m_state.relativeMouse = false;
}

Result<void, PlatformErrc> Window::SetMode(WindowMode mode, f32 targetRefreshHz) noexcept {
    if (!IsValid()) return Err(PlatformErrc::InvalidState);
    if (m_state.mode == mode && targetRefreshHz <= 0.0f) return {};

    if (m_window) {
        if (mode == WindowMode::Windowed) {
            (void)SDL_SetWindowFullscreenMode(m_window, nullptr);
            if (!SDL_SetWindowFullscreen(m_window, false)) {
                return Err(PlatformErrc::FullscreenFailed);
            }
        } else if (mode == WindowMode::Borderless) {
            (void)SDL_SetWindowFullscreenMode(m_window, nullptr);
            if (!SDL_SetWindowFullscreen(m_window, true)) {
                return Err(PlatformErrc::FullscreenFailed);
            }
        } else if (mode == WindowMode::ExclusiveFullscreen) {
            const SDL_DisplayID dispId = SDL_GetDisplayForWindow(m_window);
            SDL_DisplayMode closest{};
            const f32 reqHz = targetRefreshHz > 0.0f ? targetRefreshHz : m_state.refreshHz;
            if (dispId != 0U && SDL_GetClosestFullscreenDisplayMode(
                    dispId, m_state.pixelSize.w, m_state.pixelSize.h, reqHz, true, &closest)) {
                (void)SDL_SetWindowFullscreenMode(m_window, &closest);
            }
            if (!SDL_SetWindowFullscreen(m_window, true)) {
                return Err(PlatformErrc::FullscreenFailed);
            }
        }
    }
    m_state.mode = mode;
    if (targetRefreshHz > 0.0f) {
        m_state.refreshHz = targetRefreshHz;
    }
    (void)RefreshMetrics();
    return {};
}

Result<void, PlatformErrc> Window::SetTitle(std::string_view title) noexcept {
    if (!IsValid()) return Err(PlatformErrc::InvalidState);
    if (m_window) {
        FixedString<128> titleBuf(title);
        if (!SDL_SetWindowTitle(m_window, titleBuf.CStr())) {
            return Err(PlatformErrc::InvalidState);
        }
    }
    return {};
}

Result<void, PlatformErrc> Window::SetRelativeMouse(bool enabled) noexcept {
    if (!IsValid()) return Err(PlatformErrc::InvalidState);
    if (m_state.relativeMouse == enabled) return {};
    if (m_window) {
        if (!SDL_SetWindowRelativeMouseMode(m_window, enabled)) {
            return Err(PlatformErrc::InvalidState);
        }
    }
    m_state.relativeMouse = enabled;
    return {};
}

Result<void, PlatformErrc> Window::SetAspectRatioLock(f32 minAspect, f32 maxAspect) noexcept {
    if (!IsValid() || minAspect < 0.0f || maxAspect < 0.0f ||
        (maxAspect > 0.0f && minAspect > maxAspect)) {
        return Err(PlatformErrc::InvalidState);
    }
    if (m_window) {
        if (!SDL_SetWindowAspectRatio(m_window, minAspect, maxAspect)) {
            return Err(PlatformErrc::InvalidState);
        }
    }
    if (minAspect > 0.0f || maxAspect > 0.0f) {
        m_optionBits |= kBitAspectLocked;
    } else {
        m_optionBits &= static_cast<u8>(~kBitAspectLocked);
    }
    return {};
}

void Window::SetCursorVisible(bool visible) noexcept {
    if (IsCursorVisible() == visible) return;
    if (visible) m_optionBits |= kBitCursorVisible;
    else m_optionBits &= static_cast<u8>(~kBitCursorVisible);
    if (m_window) {
        if (visible) (void)SDL_ShowCursor();
        else (void)SDL_HideCursor();
    }
}

void Window::SetCursorShape(CursorShape shape) noexcept {
    if (m_cursorShape == shape) return;
    m_cursorShape = shape;
    if (!m_window) return;
    const u8 idx = static_cast<u8>(shape);
    if (idx < 7U) {
        if (!g_systemCursors[idx]) {
            constexpr SDL_SystemCursor kMap[7] = {
                SDL_SYSTEM_CURSOR_DEFAULT,
                SDL_SYSTEM_CURSOR_TEXT,
                SDL_SYSTEM_CURSOR_CROSSHAIR,
                SDL_SYSTEM_CURSOR_POINTER,
                SDL_SYSTEM_CURSOR_EW_RESIZE,
                SDL_SYSTEM_CURSOR_NS_RESIZE,
                SDL_SYSTEM_CURSOR_NOT_ALLOWED,
            };
            g_systemCursors[idx] = SDL_CreateSystemCursor(kMap[idx]);
        }
        if (g_systemCursors[idx]) {
            (void)SDL_SetCursor(g_systemCursors[idx]);
        }
    }
}

void Window::SetTextInput(bool enabled) noexcept {
    if (IsTextInputActive() == enabled) return;
    if (enabled) m_optionBits |= kBitTextInputActive;
    else m_optionBits &= static_cast<u8>(~kBitTextInputActive);
    if (m_window) {
        if (enabled) (void)SDL_StartTextInput(m_window);
        else (void)SDL_StopTextInput(m_window);
    }
}

void Window::WarpMouse(Vec2 pixelPos) noexcept {
    if (!IsValid() || !m_window) return;
    const Vec2 winPos = m_state.PixelToWindow(pixelPos);
    SDL_WarpMouseInWindow(m_window, winPos.x, winPos.y);
}

void Window::Flash(WindowFlashOp op) noexcept {
    if (!IsValid() || !m_window) return;
    SDL_FlashOperation sdlOp = SDL_FLASH_CANCEL;
    if (op == WindowFlashOp::Briefly) sdlOp = SDL_FLASH_BRIEFLY;
    else if (op == WindowFlashOp::UntilFocused) sdlOp = SDL_FLASH_UNTIL_FOCUSED;
    (void)SDL_FlashWindow(m_window, sdlOp);
}

Rect Window::SafeAreaPixels() const noexcept {
    Rect fallback{
        0.0f,
        0.0f,
        static_cast<f32>(m_state.pixelSize.w),
        static_cast<f32>(m_state.pixelSize.h),
    };
    if (m_window) {
        SDL_Rect safe{};
        if (SDL_GetWindowSafeArea(m_window, &safe) && safe.w > 0 && safe.h > 0) {
            const f32 d = m_state.pixelDensity;
            return {
                static_cast<f32>(safe.x) * d,
                static_cast<f32>(safe.y) * d,
                static_cast<f32>(safe.w) * d,
                static_cast<f32>(safe.h) * d,
            };
        }
    }
    return fallback;
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

    const SDL_DisplayID dispId = SDL_GetDisplayForWindow(m_window);
    if (dispId != 0U) {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(dispId);
        if (!mode) mode = SDL_GetDesktopDisplayMode(dispId);
        if (mode && mode->refresh_rate > 0.0f) {
            refreshHz = mode->refresh_rate;
        }
    }

    const f32 prevDensity = m_state.pixelDensity;
    const f32 prevScale = m_state.displayScale;
    const WindowFlags flags = m_state.ApplyMetrics(
        {lw, lh}, {pw, ph}, density, displayScale, refreshHz);

    if (std::abs(m_state.pixelDensity - prevDensity) > 1e-3f ||
        std::abs(m_state.displayScale - prevScale) > 1e-3f) {
        NotifyDpiMigration();
    }
    return flags;
}

void Window::SimulateDpiMigration(
    f32 newPixelDensity, Extent2i newLogicalSize, f32 newDisplayScale) noexcept {
    const f32 clampedDensity = std::max(0.5f, newPixelDensity);
    const f32 clampedScale = newDisplayScale > 0.0f ? std::max(0.5f, newDisplayScale) : clampedDensity;
    const Extent2i validLogical = newLogicalSize.IsValid() ? newLogicalSize : m_state.logicalSize;
    const Extent2i computedPixel{
        static_cast<i32>(static_cast<f32>(validLogical.w) * clampedDensity + 0.5f),
        static_cast<i32>(static_cast<f32>(validLogical.h) * clampedDensity + 0.5f),
    };

    (void)m_state.ApplyMetrics(
        validLogical, computedPixel, clampedDensity, clampedScale, m_state.refreshHz);
    NotifyDpiMigration();
}

} // namespace en

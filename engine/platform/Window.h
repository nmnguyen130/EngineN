#pragma once

#include "platform/PlatformTypes.h"

#include <utility>

struct SDL_Window;

namespace en {

// Move-only 64-byte RAII OS window wrapper isolating SDL3 window management to its implementation unit.
// Places WindowState at offset 0 of Cache Line 0 for zero-offset state access.
// Invariant: State().pixelDensity > 0.0f and RefreshMetrics() executes at most once per frame.
class alignas(64) Window {
public:
    using DpiScaleCallbackFn = void (*)(
        f32 newDpiScale, Extent2i logicalSize, Extent2i pixelSize, void* userCtx) noexcept;

    Window() noexcept = default;
    ~Window() noexcept { Destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept
        : m_state(other.m_state),
          m_window(std::exchange(other.m_window, nullptr)),
          m_dpiCallback(std::exchange(other.m_dpiCallback, nullptr)),
          m_dpiUserCtx(std::exchange(other.m_dpiUserCtx, nullptr)),
          m_windowId(std::exchange(other.m_windowId, 0U)),
          m_dpiMigrationCount(std::exchange(other.m_dpiMigrationCount, 0U)),
          m_cursorShape(other.m_cursorShape),
          m_optionBits(other.m_optionBits) {}

    Window& operator=(Window&& other) noexcept {
        if (this != &other) {
            Destroy();
            m_state = other.m_state;
            m_window = std::exchange(other.m_window, nullptr);
            m_dpiCallback = std::exchange(other.m_dpiCallback, nullptr);
            m_dpiUserCtx = std::exchange(other.m_dpiUserCtx, nullptr);
            m_windowId = std::exchange(other.m_windowId, 0U);
            m_dpiMigrationCount = std::exchange(other.m_dpiMigrationCount, 0U);
            m_cursorShape = other.m_cursorShape;
            m_optionBits = other.m_optionBits;
        }
        return *this;
    }

    [[nodiscard]] Result<void, PlatformErrc> Create(
        const WindowConfig& cfg = {}, bool headless = false) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_window != nullptr || m_windowId != 0U; }
    [[nodiscard]] SDL_Window* Native() const noexcept { return m_window; }
    [[nodiscard]] constexpr u32 Id() const noexcept { return m_windowId; }
    [[nodiscard]] constexpr const WindowState& State() const noexcept { return m_state; }

    [[nodiscard]] Result<void, PlatformErrc> SetMode(
        WindowMode mode, f32 targetRefreshHz = 0.0f) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetTitle(std::string_view title) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetRelativeMouse(bool enabled) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetAspectRatioLock(f32 minAspect, f32 maxAspect) noexcept;

    void SetCursorVisible(bool visible) noexcept;
    void SetCursorShape(CursorShape shape) noexcept;
    void SetTextInput(bool enabled) noexcept;
    void WarpMouse(Vec2 pixelPos) noexcept;
    void Flash(WindowFlashOp op = WindowFlashOp::UntilFocused) noexcept;

    [[nodiscard]] Rect SafeAreaPixels() const noexcept;

    [[nodiscard]] constexpr bool IsCursorVisible() const noexcept {
        return (m_optionBits & kBitCursorVisible) != 0U;
    }
    [[nodiscard]] constexpr bool IsTextInputActive() const noexcept {
        return (m_optionBits & kBitTextInputActive) != 0U;
    }
    [[nodiscard]] constexpr bool IsAspectRatioLocked() const noexcept {
        return (m_optionBits & kBitAspectLocked) != 0U;
    }
    [[nodiscard]] constexpr CursorShape GetCursorShape() const noexcept { return m_cursorShape; }

    constexpr void SetDpiChangeCallback(DpiScaleCallbackFn fn, void* userCtx = nullptr) noexcept {
        m_dpiCallback = fn;
        m_dpiUserCtx = userCtx;
    }

    void SimulateDpiMigration(
        f32 newPixelDensity, Extent2i newLogicalSize, f32 newDisplayScale = 0.0f) noexcept;
    [[nodiscard]] constexpr u16 DpiMigrationCount() const noexcept { return m_dpiMigrationCount; }

    constexpr void ApplyLifecycleEvent(
        WindowFlags eventFlag, WindowFlags& ioNetFlags, bool& outNeedRefresh) noexcept {
        m_state.ApplyLifecycleEvent(eventFlag, ioNetFlags, outNeedRefresh);
    }

    WindowFlags RefreshMetrics() noexcept;

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        return m_state.ComputeAuditHash();
    }

private:
    static constexpr u8 kBitCursorVisible = 1U << 0;
    static constexpr u8 kBitTextInputActive = 1U << 1;
    static constexpr u8 kBitAspectLocked = 1U << 2;

    void NotifyDpiMigration() noexcept {
        ++m_dpiMigrationCount;
        if (m_dpiCallback) {
            m_dpiCallback(m_state.pixelDensity, m_state.logicalSize, m_state.pixelSize, m_dpiUserCtx);
        }
    }

    WindowState m_state{};
    SDL_Window* m_window{nullptr};
    DpiScaleCallbackFn m_dpiCallback{nullptr};
    void* m_dpiUserCtx{nullptr};
    u32 m_windowId{0};
    u16 m_dpiMigrationCount{0};
    CursorShape m_cursorShape{CursorShape::Arrow};
    u8 m_optionBits{kBitCursorVisible};
};
static_assert(sizeof(Window) == 64);

} // namespace en

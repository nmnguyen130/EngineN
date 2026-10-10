#pragma once

#include "platform/Display.h"
#include <string_view>

struct SDL_Window;
union SDL_Event;

namespace EngineN {

enum class WindowMode : u8 {
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen
};

enum class WindowGraphicsHint : u8 {
    None,
    OpenGL,
    Vulkan,
    Metal
};

struct WindowModeRequest {
    WindowMode mode{WindowMode::Windowed};
    DisplayId display{};
    ModeQuery exclusive{};
};

struct WindowConfig {
    std::string_view title{"EngineN"};
    Extent2i size{1280, 720};
    Extent2i minSize{640, 360};
    WindowMode mode{WindowMode::Windowed};
    WindowGraphicsHint graphics{WindowGraphicsHint::None};
    bool highDPI{true};
    bool resizable{true};
    bool startMaximized{false};
    bool startHidden{false};
};

enum class WindowChange : u16 {
    None         = 0,
    Size         = 1u << 0,
    PixelSize    = 1u << 1,
    DisplayScale = 1u << 2,
    PixelDensity = 1u << 3,
    Display      = 1u << 4,
    Position     = 1u << 5,
    Mode         = 1u << 6,
    Focus        = 1u << 7,
    Visibility   = 1u << 8,
};

[[nodiscard]] constexpr WindowChange operator|(WindowChange a, WindowChange b) noexcept {
    return static_cast<WindowChange>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr WindowChange operator&(WindowChange a, WindowChange b) noexcept {
    return static_cast<WindowChange>(static_cast<u16>(a) & static_cast<u16>(b));
}
[[nodiscard]] constexpr WindowChange operator~(WindowChange a) noexcept {
    return static_cast<WindowChange>(~static_cast<u16>(a));
}
constexpr WindowChange& operator|=(WindowChange& a, WindowChange b) noexcept { return a = a | b; }
constexpr WindowChange& operator&=(WindowChange& a, WindowChange b) noexcept { return a = a & b; }
[[nodiscard]] constexpr bool HasAny(WindowChange val, WindowChange mask) noexcept { return (val & mask) != WindowChange::None; }

struct WindowMetrics {
    Extent2i logicalSize{};
    Extent2i pixelSize{};
    f32 pixelDensity{1.0f};
    f32 displayScale{1.0f};
};

struct WindowState {
    WindowMetrics metrics{};
    IPoint position{};
    DisplayId display{};
    WindowMode mode{WindowMode::Windowed};
    bool transitionPending{false};
    bool focused{false};
    bool minimized{false};
    bool maximized{false};
    bool occluded{false};
    bool visible{true};

    [[nodiscard]] constexpr bool IsPresentable() const noexcept { return visible && !minimized; }
    [[nodiscard]] constexpr bool IsBackgroundLike() const noexcept { return !focused || occluded; }
};

/**
 * Window: RAII SDL_Window wrapper with mirrored, non-allocating WindowState.
 * Updates state on events and tracks dirty flags via WindowChange.
 */
class Window {
public:
    Window() noexcept = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    [[nodiscard]] static Result<Window, EngineError> Create(
        const WindowConfig& config,
        const DisplayTopology& displays) noexcept;

    [[nodiscard]] Result<void, EngineError> Create(const WindowConfig& config) noexcept;

    void Destroy() noexcept;

    [[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] SDL_Window* Native() const noexcept { return m_handle; }
    [[nodiscard]] WindowId Id() const noexcept { return m_id; }

    [[nodiscard]] const WindowState& State() const noexcept { return m_state; }
    [[nodiscard]] WindowChange TakeChanges() noexcept {
        const WindowChange c = m_dirty;
        m_dirty = WindowChange::None;
        return c;
    }

    [[nodiscard]] Result<void, EngineError> SetMode(
        const WindowModeRequest& request,
        const DisplayTopology& displays) noexcept;
    [[nodiscard]] Result<void, EngineError> SetMode(WindowMode mode) noexcept;

    [[nodiscard]] Result<void, EngineError> SetTitle(std::string_view title) noexcept;
    [[nodiscard]] Result<void, EngineError> SetMinimumSize(Extent2i size) noexcept;
    void Show() noexcept;
    void RequestAttention() noexcept;
    void SetCursorVisible(bool visible) noexcept;

    void Resync() noexcept;

    // Direct metric queries
    [[nodiscard]] i32 GetWidth() const noexcept { return m_state.metrics.logicalSize.w; }
    [[nodiscard]] i32 GetHeight() const noexcept { return m_state.metrics.logicalSize.h; }
    [[nodiscard]] i32 GetPixelWidth() const noexcept { return m_state.metrics.pixelSize.w; }
    [[nodiscard]] i32 GetPixelHeight() const noexcept { return m_state.metrics.pixelSize.h; }
    [[nodiscard]] f32 GetDisplayScale() const noexcept { return m_state.metrics.displayScale; }
    [[nodiscard]] f32 GetPixelDensity() const noexcept { return m_state.metrics.pixelDensity; }
    [[nodiscard]] WindowMode GetMode() const noexcept { return m_state.mode; }
    [[nodiscard]] bool IsFullscreen() const noexcept {
        return m_state.mode == WindowMode::BorderlessFullscreen ||
               m_state.mode == WindowMode::ExclusiveFullscreen;
    }

private:
    friend class Platform;
    void ApplyEvent(const SDL_Event& event) noexcept;

    SDL_Window* m_handle{nullptr};
    WindowId m_id{};
    WindowState m_state{};
    WindowChange m_dirty{WindowChange::None};
};

} // namespace EngineN

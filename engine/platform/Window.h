#pragma once

#include "platform/PlatformTypes.h"

struct SDL_Window;
union SDL_Event;

namespace en::platform {

enum class WindowMode : u8 {
    Windowed,
    Borderless,
    ExclusiveFullscreen,
};

enum class GraphicsApi : u8 {
    None,
    OpenGL,
    Vulkan,
    Metal,
};

struct WindowConfig {
    std::string_view title{"EngineN"};
    Extent2i size{1280, 720};
    Extent2i minSize{640, 360};
    WindowMode mode{WindowMode::Windowed};
    GraphicsApi api{GraphicsApi::None};
    bool resizable{true};
    bool highDpi{true};
};

enum class WindowFlags : u8 {
    None           = 0,
    Resized        = 1u << 0,
    DisplayChanged = 1u << 1,
    FocusGained    = 1u << 2,
    FocusLost      = 1u << 3,
};

[[nodiscard]] constexpr WindowFlags operator|(WindowFlags a, WindowFlags b) noexcept {
    return static_cast<WindowFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr WindowFlags operator&(WindowFlags a, WindowFlags b) noexcept {
    return static_cast<WindowFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr WindowFlags& operator|=(WindowFlags& a, WindowFlags b) noexcept { return a = a | b; }
[[nodiscard]] constexpr bool HasFlag(WindowFlags val, WindowFlags mask) noexcept {
    return (val & mask) != WindowFlags::None;
}

struct WindowState {
    Extent2i logicalSize{};
    Extent2i pixelSize{};
    f32 pixelDensity{1.0f};
    f32 refreshHz{60.0f};
    WindowMode mode{WindowMode::Windowed};
    bool focused{true};
    bool suspended{false};

    [[nodiscard]] constexpr bool IsPresentable() const noexcept {
        return !suspended && pixelSize.w > 0 && pixelSize.h > 0;
    }

    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return pixelSize.h > 0 ? static_cast<f32>(pixelSize.w) / static_cast<f32>(pixelSize.h) : 1.0f;
    }
};

// Move-only RAII handle for SDL_Window with synchronous metric tracking.
// Invariant: m_id != 0 iff m_handle != nullptr.
class Window {
public:
    constexpr Window() noexcept = default;
    ~Window() noexcept;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    [[nodiscard]] Result<void, Error> Create(const WindowConfig& cfg = {}) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] constexpr SDL_Window* Native() const noexcept { return m_handle; }
    [[nodiscard]] constexpr u32 Id() const noexcept { return m_id; }
    [[nodiscard]] constexpr const WindowState& State() const noexcept { return m_state; }

    [[nodiscard]] Result<void, Error> SetMode(WindowMode mode) noexcept;
    [[nodiscard]] Result<void, Error> SetTitle(std::string_view title) noexcept;
    [[nodiscard]] Result<void, Error> SetRelativeMouse(bool enabled) noexcept;
    void SetCursorVisible(bool visible) noexcept;
    void SetTextInput(bool enabled) noexcept;

    [[nodiscard]] WindowFlags ProcessEvent(const SDL_Event& ev) noexcept;
    void RefreshMetrics() noexcept;

private:
    SDL_Window* m_handle{nullptr};
    u32 m_id{0};
    WindowState m_state{};
};

} // namespace en::platform
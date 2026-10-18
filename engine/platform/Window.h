#pragma once

#include "platform/PlatformTypes.h"

struct SDL_Window;
union SDL_Event;

namespace en::platform {

// Move-only RAII handle for SDL_Window with deferred single-pass metric synchronization.
// Places 32-byte WindowState at offset 0 so State() resolves with zero pointer offset math.
// Invariant: m_id != 0 iff m_handle != nullptr; sizeof(Window) == 64 (1 L1 cache line).
class alignas(64) Window {
public:
    constexpr Window() noexcept = default;
    ~Window() noexcept;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    [[nodiscard]] Result<void, PlatformErrc> Create(const WindowConfig& cfg = {}) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] constexpr SDL_Window* Native() const noexcept { return m_handle; }
    [[nodiscard]] constexpr u32 Id() const noexcept { return m_id; }
    [[nodiscard]] constexpr const WindowState& State() const noexcept { return m_state; }

    [[nodiscard]] Result<void, PlatformErrc> SetMode(WindowMode mode) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetTitle(std::string_view title) noexcept;
    [[nodiscard]] Result<void, PlatformErrc> SetRelativeMouse(bool enabled) noexcept;
    void SetCursorVisible(bool visible) noexcept;
    void SetTextInput(bool enabled) noexcept;

    // Accumulates window event flags in registers and sets dirty-metric bit without querying SDL.
    // RefreshMetrics() is invoked at most once per frame by Platform::Pump() when outNeedRefresh is true.
    [[nodiscard]] WindowFlags ProcessEvent(const SDL_Event& ev, bool& outNeedRefresh) noexcept;
    void RefreshMetrics() noexcept;

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        return m_state.ComputeAuditHash();
    }

private:
    WindowState m_state{};
    SDL_Window* m_handle{nullptr};
    u32 m_id{0};
};
static_assert(sizeof(Window) == 64);

} // namespace en::platform

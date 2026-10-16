#pragma once

#include "platform/PlatformTypes.h"
#include "platform/Window.h"

#include <span>

struct SDL_Gamepad;

namespace en::input { class InputPipeline; }

namespace en::platform {

// Bitmask returned by EventTap to block Press/Motion events from reaching InputPipeline
// while allowing Release events through to guarantee zero stuck keys.
enum class EventCapture : u8 {
    None     = 0,
    Keyboard = 1u << 0,
    Mouse    = 1u << 1,
    Gamepad  = 1u << 2,
};

[[nodiscard]] constexpr EventCapture operator|(EventCapture a, EventCapture b) noexcept {
    return static_cast<EventCapture>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr EventCapture operator&(EventCapture a, EventCapture b) noexcept {
    return static_cast<EventCapture>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr bool HasCapture(EventCapture val, EventCapture mask) noexcept {
    return (val & mask) != EventCapture::None;
}

struct EventTap {
    using Fn = EventCapture (*)(void* user, const SDL_Event& ev) noexcept;
    void* user{nullptr};
    Fn callback{nullptr};

    template <auto Method, typename T>
    [[nodiscard]] static constexpr EventTap Bind(T& instance) noexcept {
        return EventTap{
            .user = &instance,
            .callback = [](void* self, const SDL_Event& e) noexcept -> EventCapture {
                return (static_cast<T*>(self)->*Method)(e);
            }
        };
    }
};

// Owns SDL3 subsystem lifecycle, gamepad handles, sandboxed filesystem paths, and frame event pumping.
// Single-instance OS coordinator; non-copyable and non-movable.
// Invariant: All active SDL_Gamepad handles are closed prior to SDL_Quit().
class Platform {
public:
    static constexpr usize kMaxGamepads = 4;

    constexpr Platform() noexcept = default;
    ~Platform() noexcept;

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] Result<void, Error> Init(
        std::string_view appName = "EngineN",
        std::string_view orgName = "EngineN") noexcept;
    void Shutdown() noexcept;

    WindowFlags Pump(Window& window, input::InputPipeline& input, EventTap tap = {}) noexcept;

    [[nodiscard]] constexpr bool ShouldQuit() const noexcept { return m_quit; }
    constexpr void RequestQuit() noexcept { m_quit = true; }

    bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept;
    void StopAllRumble() noexcept;

    [[nodiscard]] Result<FilePath, Error> ResolveAsset(std::string_view relative) const noexcept;
    [[nodiscard]] Result<FilePath, Error> ResolveUser(std::string_view relative) const noexcept;

    [[nodiscard]] static std::string_view GetClipboard(std::span<char> outBuf) noexcept;
    [[nodiscard]] static Result<void, Error> SetClipboard(std::string_view utf8) noexcept;
    static void ShowErrorBox(std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept;

private:
    struct GamepadSlot {
        u32 id{0};
        SDL_Gamepad* handle{nullptr};
    };

    void OpenGamepad(u32 id) noexcept;
    void CloseGamepad(u32 id) noexcept;

    bool m_initialized{false};
    bool m_quit{false};
    std::array<GamepadSlot, kMaxGamepads> m_gamepads{};
    FilePath m_basePath{};
    FilePath m_prefPath{};
};

} // namespace en::platform
#pragma once

#include "platform/PlatformTypes.h"
#include "platform/Display.h"
#include "platform/Window.h"
#include "platform/Devices.h"
#include "platform/Services.h"
#include "platform/EventRouting.h"

namespace EngineN {

class InputSystem;

/**
 * Platform: Central platform coordinator.
 * Owns OS subsystems, coordinates single-source SDL event pumping,
 * and feeds events through prioritized EventRouter stages.
 */
class Platform {
public:
    Platform() noexcept;
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] Result<void, EngineError> Init() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

    // Unified single-source pump driving EventRouter, Window, and InputSystem
    const PlatformEventQueue& Pump(Window& window, InputSystem& input) noexcept;
    const PlatformEventQueue& Pump() noexcept;

    // Subsystem accessors
    [[nodiscard]] const DisplayTopology& Displays() const noexcept { return m_displays; }
    [[nodiscard]] DisplayTopology& Displays() noexcept { return m_displays; }

    [[nodiscard]] const GamepadRegistry& Gamepads() const noexcept { return m_gamepads; }
    [[nodiscard]] GamepadRegistry& Gamepads() noexcept { return m_gamepads; }

    [[nodiscard]] const AudioDeviceDirectory& AudioDevices() const noexcept { return m_audioDevices; }
    [[nodiscard]] AudioDeviceDirectory& AudioDevices() noexcept { return m_audioDevices; }

    [[nodiscard]] const PathService& Paths() const noexcept { return m_paths; }
    [[nodiscard]] const Clipboard& ClipboardService() const noexcept { return m_clipboard; }
    [[nodiscard]] Clipboard& ClipboardService() noexcept { return m_clipboard; }

    [[nodiscard]] const SystemSpecs& Specs() const noexcept { return m_specs; }

    [[nodiscard]] EventRouter& Router() noexcept { return m_router; }

    [[nodiscard]] bool WasQuitRequested() const noexcept { return m_quitRequested; }
    [[nodiscard]] bool IsTerminating() const noexcept { return m_terminating; }
    void RequestQuit() noexcept { m_quitRequested = true; }

private:
    SinkVerdict OnWindowEvent(const SDL_Event& event) noexcept;
    SinkVerdict OnInputEvent(const SDL_Event& event) noexcept;
    SinkVerdict OnSystemEvent(const SDL_Event& event) noexcept;

    bool m_initialized{false};
    bool m_quitRequested{false};
    bool m_terminating{false};

    DisplayTopology m_displays{};
    GamepadRegistry m_gamepads{};
    AudioDeviceDirectory m_audioDevices{};
    PathService m_paths{};
    Clipboard m_clipboard{};
    SystemSpecs m_specs{};
    EventRouter m_router{};
    PlatformEventQueue m_events{};

    Window* m_activeWindow{nullptr};
    InputSystem* m_activeInput{nullptr};
};

} // namespace EngineN

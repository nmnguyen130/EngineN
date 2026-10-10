#include "platform/Platform.h"
#include "platform/input/InputSystem.h"
#include <SDL3/SDL.h>

namespace EngineN {

Platform::Platform() noexcept {
    (void)m_router.Add(SinkStage::Window, EventSinkRef::Bind<&Platform::OnWindowEvent>(*this));
    (void)m_router.Add(SinkStage::Input,  EventSinkRef::Bind<&Platform::OnInputEvent>(*this));
    (void)m_router.Add(SinkStage::App,    EventSinkRef::Bind<&Platform::OnSystemEvent>(*this));
}

Platform::~Platform() {
    Shutdown();
}

Result<void, EngineError> Platform::Init() noexcept {
    if (m_initialized) return Ok;

    // Initialize ONLY video and gamepad subsystems.
    // Audio is strictly omitted to prevent failures in headless/CI/virtual environments.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return Unexpected(MakePlatformError(PlatformErrc::SdlInitFailed));
    }

    auto dispRes = m_displays.Refresh();
    if (!dispRes) {
        SDL_Quit();
        return Unexpected(dispRes.Error());
    }

    (void)m_audioDevices.Seed();
    (void)m_paths.Init("EngineN", "EngineN");
    m_specs = QuerySystemSpecs();

    m_initialized = true;
    m_quitRequested = false;
    m_terminating = false;
    return Ok;
}

void Platform::Shutdown() noexcept {
    if (!m_initialized) return;

    m_gamepads.CloseAll();
    SDL_Quit();
    m_initialized = false;
}

const PlatformEventQueue& Platform::Pump(Window& window, InputSystem& input) noexcept {
    m_activeWindow = &window;
    m_activeInput = &input;
    return Pump();
}

const PlatformEventQueue& Platform::Pump() noexcept {
    m_events.Clear();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        m_router.Dispatch(event);
    }

    m_activeWindow = nullptr;
    m_activeInput = nullptr;
    return m_events;
}

SinkVerdict Platform::OnWindowEvent(const SDL_Event& event) noexcept {
    if (m_activeWindow) {
        m_activeWindow->ApplyEvent(event);
    }
    return SinkVerdict::Continue;
}

SinkVerdict Platform::OnInputEvent(const SDL_Event& event) noexcept {
    if (m_activeInput) {
        m_activeInput->Ingest(event);
    }
    return SinkVerdict::Continue;
}

SinkVerdict Platform::OnSystemEvent(const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_QUIT: {
            m_quitRequested = true;
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::QuitRequested,
                .timestampNs = event.quit.timestamp
            });
            break;
        }

        case SDL_EVENT_TERMINATING: {
            m_terminating = true;
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::Terminating,
                .timestampNs = event.common.timestamp
            });
            break;
        }

        case SDL_EVENT_GAMEPAD_ADDED: {
            auto slotRes = m_gamepads.OnAdded(GamepadId{static_cast<u32>(event.gdevice.which)});
            const u8 slot = slotRes ? *slotRes : 0;
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::GamepadConnected,
                .slot = slot,
                .id = static_cast<u32>(event.gdevice.which),
                .timestampNs = event.gdevice.timestamp
            });
            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED: {
            auto slotOpt = m_gamepads.OnRemoved(GamepadId{static_cast<u32>(event.gdevice.which)});
            const u8 slot = slotOpt.value_or(0);
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::GamepadDisconnected,
                .slot = slot,
                .id = static_cast<u32>(event.gdevice.which),
                .timestampNs = event.gdevice.timestamp
            });
            break;
        }

        case SDL_EVENT_DISPLAY_ADDED:
        case SDL_EVENT_DISPLAY_REMOVED:
        case SDL_EVENT_DISPLAY_MOVED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
        case SDL_EVENT_DISPLAY_ORIENTATION: {
            (void)m_displays.Refresh();
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::DisplayTopologyChanged,
                .id = m_displays.Revision(),
                .timestampNs = event.display.timestamp
            });
            break;
        }

        case SDL_EVENT_AUDIO_DEVICE_ADDED: {
            (void)m_audioDevices.OnAdded(AudioDeviceId{static_cast<u32>(event.adevice.which)}, event.adevice.recording != 0);
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::AudioDeviceAdded,
                .recording = (event.adevice.recording != 0),
                .id = static_cast<u32>(event.adevice.which),
                .timestampNs = event.adevice.timestamp
            });
            break;
        }

        case SDL_EVENT_AUDIO_DEVICE_REMOVED: {
            (void)m_audioDevices.OnRemoved(AudioDeviceId{static_cast<u32>(event.adevice.which)}, event.adevice.recording != 0);
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::AudioDeviceRemoved,
                .recording = (event.adevice.recording != 0),
                .id = static_cast<u32>(event.adevice.which),
                .timestampNs = event.adevice.timestamp
            });
            break;
        }

        case SDL_EVENT_CLIPBOARD_UPDATE: {
            (void)m_events.TryPush(PlatformEvent{
                .type = PlatformEventType::ClipboardChanged,
                .timestampNs = event.clipboard.timestamp
            });
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            m_gamepads.StopAllRumble();
            break;
        }

        default:
            break;
    }

    return SinkVerdict::Continue;
}

} // namespace EngineN

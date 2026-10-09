#pragma once

#include "core/Types.h"
#include "platform/PlatformTypes.h"
#include <functional>
#include <vector>

namespace EngineN {

/// Strictly enforces "Single Source of Truth" for SDL_PollEvent.
/// SDL_PollEvent is drained in exactly ONE loop per frame inside Poll().
/// Fully decoupled from concrete subsystems: dispatches modularly to registered
/// IEventListener implementations, event category masks, and custom callbacks.
class EventPump {
public:
    using EventCallback = std::function<void(const SDL_Event&)>;

    EventPump();
    ~EventPump() = default;

    EventPump(const EventPump&) = delete;
    EventPump& operator=(const EventPump&) = delete;
    EventPump(EventPump&&) noexcept = default;
    EventPump& operator=(EventPump&&) noexcept = default;

    /// Begin a new frame, resetting one-frame transient notification flags.
    void BeginFrame() noexcept;

    // --- Listener Registration (Decoupled Subsystem Routing) ---
    void AddListener(IEventListener* listener, EventCategory mask = EventCategory::All);
    void RemoveListener(IEventListener* listener);
    [[nodiscard]] bool HasListener(const IEventListener* listener) const noexcept;
    void ClearListeners() noexcept;

    // --- Callback Registration (Lambdas / Sinks) ---
    u32 AddCallback(EventCallback callback, EventCategory mask = EventCategory::All);
    bool RemoveCallback(u32 callbackId);
    void ClearCallbacks() noexcept;

    // --- Event Draining (Single Source of Truth) ---
    /// Drain the entire SDL event queue for this frame and route to all registered
    /// listeners and callbacks. Returns true if quit was requested.
    bool Poll();

    /// Drain with a custom per-event dispatcher sink in addition to registered listeners.
    template <typename FSink>
    bool PollCustom(FSink&& customSink) {
        return PollInternal([&](const SDL_Event& e, EventCategory) {
            customSink(e);
        });
    }

    /// Convenience forwarder for backwards compatibility.
    /// Drains events and forwards to window and input without coupling EventPump to their headers.
    template <typename TWindow, typename TInput>
    bool PollAll(TWindow& window, TInput& input) {
        return PollInternal([&](const SDL_Event& e, EventCategory cat) {
            if (static_cast<u32>(cat & EventCategory::Window) != 0) {
                if (window.HandleEvent(e)) {
                    m_quitRequested = true;
                }
            }
            if (static_cast<u32>(cat & EventCategory::Input) != 0) {
                input.ProcessEvent(e);
            }
        });
    }

    [[nodiscard]] bool WasQuitRequested() const noexcept { return m_quitRequested; }
    [[nodiscard]] bool WasLowMemory() const noexcept { return m_lowMemory; }
    [[nodiscard]] bool WasTerminating() const noexcept { return m_terminating; }

    /// Categorizes an SDL_Event into an EventCategory bitmask.
    [[nodiscard]] static EventCategory ClassifyEvent(const SDL_Event& event) noexcept;

private:
    struct ListenerEntry {
        IEventListener* listener = nullptr;
        EventCategory mask = EventCategory::All;
    };

    struct CallbackEntry {
        u32 id = 0;
        EventCallback callback;
        EventCategory mask = EventCategory::All;
    };

    using InternalSinkFn = std::function<void(const SDL_Event&, EventCategory)>;
    bool PollInternal(const InternalSinkFn& extraSink = nullptr);

    std::vector<ListenerEntry> m_listeners;
    std::vector<CallbackEntry> m_callbacks;
    u32 m_nextCallbackId = 1;

    bool m_quitRequested = false;
    bool m_lowMemory = false;
    bool m_terminating = false;
};

} // namespace EngineN

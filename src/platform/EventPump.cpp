#include "platform/EventPump.h"
#include <SDL3/SDL.h>
#include <algorithm>

namespace EngineN {

EventPump::EventPump() {
    m_listeners.reserve(16);
    m_callbacks.reserve(8);
}

void EventPump::BeginFrame() noexcept {
    m_lowMemory = false;
    m_terminating = false;
}

void EventPump::AddListener(IEventListener* listener, EventCategory mask) {
    if (!listener) return;
    for (auto& entry : m_listeners) {
        if (entry.listener == listener) {
            entry.mask = mask;
            return;
        }
    }
    m_listeners.push_back({.listener = listener, .mask = mask});
}

void EventPump::RemoveListener(IEventListener* listener) {
    if (!listener) return;
    auto it = std::remove_if(m_listeners.begin(), m_listeners.end(),
        [listener](const ListenerEntry& entry) { return entry.listener == listener; });
    m_listeners.erase(it, m_listeners.end());
}

bool EventPump::HasListener(const IEventListener* listener) const noexcept {
    if (!listener) return false;
    for (const auto& entry : m_listeners) {
        if (entry.listener == listener) return true;
    }
    return false;
}

void EventPump::ClearListeners() noexcept {
    m_listeners.clear();
}

u32 EventPump::AddCallback(EventCallback callback, EventCategory mask) {
    if (!callback) return 0;
    u32 id = m_nextCallbackId++;
    m_callbacks.push_back({.id = id, .callback = std::move(callback), .mask = mask});
    return id;
}

bool EventPump::RemoveCallback(u32 callbackId) {
    if (callbackId == 0) return false;
    auto it = std::remove_if(m_callbacks.begin(), m_callbacks.end(),
        [callbackId](const CallbackEntry& entry) { return entry.id == callbackId; });
    if (it != m_callbacks.end()) {
        m_callbacks.erase(it, m_callbacks.end());
        return true;
    }
    return false;
}

void EventPump::ClearCallbacks() noexcept {
    m_callbacks.clear();
}

EventCategory EventPump::ClassifyEvent(const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
        case SDL_EVENT_LOW_MEMORY:
            return EventCategory::System;

        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_WINDOW_OCCLUDED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        case SDL_EVENT_WINDOW_DESTROYED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            return EventCategory::Window;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            return EventCategory::Keyboard;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
            return EventCategory::Mouse;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_REMAPPED:
            return EventCategory::Gamepad;

        case SDL_EVENT_AUDIO_DEVICE_ADDED:
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
            return EventCategory::Audio;

        default:
            return EventCategory::System;
    }
}

bool EventPump::Poll() {
    return PollInternal(nullptr);
}

bool EventPump::PollInternal(const InternalSinkFn& extraSink) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        // Track pump flags
        switch (e.type) {
            case SDL_EVENT_QUIT:
                m_quitRequested = true;
                break;
            case SDL_EVENT_TERMINATING:
                m_terminating = true;
                m_quitRequested = true;
                break;
            case SDL_EVENT_LOW_MEMORY:
                m_lowMemory = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                m_quitRequested = true;
                break;
            default:
                break;
        }

        EventCategory cat = ClassifyEvent(e);

        // Dispatch to registered IEventListener instances
        for (const auto& entry : m_listeners) {
            if (entry.listener && (static_cast<u32>(entry.mask & cat) != 0)) {
                entry.listener->ProcessEvent(e);
            }
        }

        // Dispatch to registered functional callbacks
        for (const auto& cbEntry : m_callbacks) {
            if (cbEntry.callback && (static_cast<u32>(cbEntry.mask & cat) != 0)) {
                cbEntry.callback(e);
            }
        }

        // Forward to extra sink if present
        if (extraSink) {
            extraSink(e, cat);
        }
    }

    return m_quitRequested;
}

} // namespace EngineN

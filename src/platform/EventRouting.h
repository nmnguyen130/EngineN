#pragma once

#include "platform/PlatformTypes.h"
#include <SDL3/SDL_events.h>
#include <concepts>

namespace EngineN {

enum class SinkVerdict : u8 {
    Continue,
    Handled
};

enum class SinkStage : u8 {
    DevTools = 0,
    Window   = 1,
    Input    = 2,
    App      = 3
};

inline constexpr usize kSinkStageCount = 4;
inline constexpr usize kMaxSinksPerStage = 4;

/**
 * EventSinkRef: zero-allocation, non-owning delegate for event consumers.
 */
class EventSinkRef {
public:
    using Fn = SinkVerdict (*)(void* self, const SDL_Event& event) noexcept;

    constexpr EventSinkRef() noexcept = default;

    template <auto Method, typename T>
    [[nodiscard]] static constexpr EventSinkRef Bind(T& object) noexcept {
        return EventSinkRef{&object, [](void* self, const SDL_Event& e) noexcept -> SinkVerdict {
            return (static_cast<T*>(self)->*Method)(e);
        }};
    }

    [[nodiscard]] SinkVerdict operator()(const SDL_Event& e) const noexcept {
        return m_fn ? m_fn(m_self, e) : SinkVerdict::Continue;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept { return m_fn != nullptr; }

private:
    constexpr EventSinkRef(void* self, Fn fn) noexcept : m_self(self), m_fn(fn) {}
    void* m_self{nullptr};
    Fn m_fn{nullptr};
};

/**
 * EventRouter: prioritized 4-stage event routing pipeline.
 * DevTools (0) -> Window (1) -> Input (2) -> App (3).
 */
class EventRouter {
public:
    [[nodiscard]] bool Add(SinkStage stage, EventSinkRef sink) noexcept {
        return sink && m_stages[static_cast<usize>(stage)].TryPush(sink);
    }

    void Clear(SinkStage stage) noexcept {
        m_stages[static_cast<usize>(stage)].Clear();
    }

    void Dispatch(const SDL_Event& event) const noexcept {
        for (const auto& stage : m_stages) {
            for (const EventSinkRef& sink : stage.View()) {
                if (sink(event) == SinkVerdict::Handled) {
                    return;
                }
            }
        }
    }

private:
    std::array<FixedVector<EventSinkRef, kMaxSinksPerStage>, kSinkStageCount> m_stages{};
};

enum class InputDevice : u8 {
    None,
    Keyboard,
    Mouse,
    Gamepad,
    Touch
};

enum class EventPhase : u8 {
    Neutral,    // Never suppressed (axes, window focus, system quit)
    Press,      // Edge that starts action: suppressible
    Release,    // Edge that ends action: NEVER suppressed
    Continuous  // Cursor motion / wheel: suppressible
};

struct EventClass {
    InputDevice device{InputDevice::None};
    EventPhase phase{EventPhase::Neutral};
};

[[nodiscard]] constexpr EventClass Classify(u32 t) noexcept {
    switch (t) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_TEXT_EDITING:
            return {InputDevice::Keyboard, EventPhase::Press};
        case SDL_EVENT_KEY_UP:
            return {InputDevice::Keyboard, EventPhase::Release};

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            return {InputDevice::Mouse, EventPhase::Press};
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return {InputDevice::Mouse, EventPhase::Release};
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
            return {InputDevice::Mouse, EventPhase::Continuous};

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            return {InputDevice::Gamepad, EventPhase::Press};
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return {InputDevice::Gamepad, EventPhase::Release};
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return {InputDevice::Gamepad, EventPhase::Neutral};

        case SDL_EVENT_FINGER_DOWN:
            return {InputDevice::Touch, EventPhase::Press};
        case SDL_EVENT_FINGER_UP:
            return {InputDevice::Touch, EventPhase::Release};
        case SDL_EVENT_FINGER_MOTION:
            return {InputDevice::Touch, EventPhase::Continuous};

        default:
            return {};
    }
}

struct DevToolsCapture {
    bool wantsKeyboard{false};
    bool wantsMouse{false};
    bool wantsGamepad{false};

    [[nodiscard]] constexpr bool Wants(InputDevice d) const noexcept {
        switch (d) {
            case InputDevice::Keyboard: return wantsKeyboard;
            case InputDevice::Mouse:
            case InputDevice::Touch:    return wantsMouse;
            case InputDevice::Gamepad:  return wantsGamepad;
            default:                    return false;
        }
    }
};

/**
 * Anti-sticking capture rule:
 * Releases and Neutral (axes / focus) are never suppressed to guarantee no button sticks.
 */
[[nodiscard]] constexpr bool ShouldSuppress(EventClass c, const DevToolsCapture& capture) noexcept {
    if (c.phase == EventPhase::Neutral || c.phase == EventPhase::Release) {
        return false;
    }
    return capture.Wants(c.device);
}

template <typename B>
concept DevToolsBackend = requires(B& b, const SDL_Event& e) {
    { b.ProcessEvent(e) } noexcept;
    { b.Capture() } noexcept -> std::same_as<DevToolsCapture>;
};

template <DevToolsBackend Backend>
class DevToolsSink {
public:
    explicit DevToolsSink(Backend& backend) noexcept : m_backend(backend) {}

    [[nodiscard]] SinkVerdict Consume(const SDL_Event& e) noexcept {
        m_backend.ProcessEvent(e);
        return ShouldSuppress(Classify(e.type), m_backend.Capture())
            ? SinkVerdict::Handled
            : SinkVerdict::Continue;
    }

    [[nodiscard]] bool Attach(EventRouter& router) noexcept {
        return router.Add(SinkStage::DevTools, EventSinkRef::Bind<&DevToolsSink::Consume>(*this));
    }

private:
    Backend& m_backend;
};

} // namespace EngineN

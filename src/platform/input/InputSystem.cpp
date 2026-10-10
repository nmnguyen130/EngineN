#include "platform/input/InputSystem.h"
#include <SDL3/SDL.h>

namespace EngineN {

InputSystem::InputSystem() noexcept {
    m_bindings.SetDefaultBindings();
}

void InputSystem::Ingest(const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
            if (event.key.repeat) {
                return;
            }
            const u16 scancode = static_cast<u16>(event.key.scancode);
            if (scancode == 0 || scancode >= kMaxKeyboardKeys) {
                return;
            }
            m_keysDown[scancode] = true;
            if (m_keysLatched[scancode]) {
                return;
            }
            RouteSource(Source{Device::Keyboard, scancode}, true, event.key.timestamp);
            break;
        }

        case SDL_EVENT_KEY_UP: {
            const u16 scancode = static_cast<u16>(event.key.scancode);
            if (scancode == 0 || scancode >= kMaxKeyboardKeys) {
                return;
            }
            m_keysDown[scancode] = false;
            m_keysLatched[scancode] = false;
            RouteSource(Source{Device::Keyboard, scancode}, false, event.key.timestamp);
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const u16 button = static_cast<u16>(event.button.button);
            if (button == 0 || button >= kMaxMouseButtons) {
                return;
            }
            #ifdef SDL_TOUCH_MOUSEID
            if (event.button.which == SDL_TOUCH_MOUSEID) {
                return;
            }
            #endif
            m_mouseDown[button] = true;
            if (m_mouseLatched[button]) {
                return;
            }
            RouteSource(Source{Device::Mouse, button}, true, event.button.timestamp);
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const u16 button = static_cast<u16>(event.button.button);
            if (button == 0 || button >= kMaxMouseButtons) {
                return;
            }
            #ifdef SDL_TOUCH_MOUSEID
            if (event.button.which == SDL_TOUCH_MOUSEID) {
                return;
            }
            #endif
            m_mouseDown[button] = false;
            m_mouseLatched[button] = false;
            RouteSource(Source{Device::Mouse, button}, false, event.button.timestamp);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            m_currentMousePos = Vec2{event.motion.x, event.motion.y};
            m_accumMouseDelta += Vec2{event.motion.xrel, event.motion.yrel};
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            const float factor = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
            m_accumMouseWheel += Vec2{event.wheel.x * factor, event.wheel.y * factor};
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            // Focus loss: synthesize releases for all active actions and clear queues
            const u64 nowNs = event.window.timestamp;
            for (ActionId a = 0; a < kMaxActions; ++a) {
                if ((m_simHeld & (1u << a)) != 0 || (m_frameHeld & (1u << a)) != 0) {
                    PushTransition(a, nowNs, false);
                    RecordFrameEvent(nowNs, a, Edge::Release, true);
                }
            }
            m_keysDown.fill(false);
            m_keysLatched.fill(false);
            m_mouseDown.fill(false);
            m_mouseLatched.fill(false);
            m_gamepadDown.fill(false);
            m_gamepadLatched.fill(false);
            m_frameHeld = 0;
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_GAINED: {
            // Focus gain: latch currently held physical devices
            m_keysLatched = m_keysDown;
            m_mouseLatched = m_mouseDown;
            m_gamepadLatched = m_gamepadDown;
            break;
        }

        default:
            break;
    }
}

void InputSystem::PushTransition(ActionId action, u64 timestampNs, bool down) noexcept {
    if (action >= kMaxActions) return;

    auto& q = m_queues[action];
    const bool currentHeld = q.Empty() ? ((m_simHeld & (1u << action)) != 0) : q.Back().down;
    if (down == currentHeld) {
        return;
    }

    if (q.Full()) {
        // Drop oldest press+release pair to preserve net state
        q.DropOldestPair();
        ++m_droppedTransitions;
    }
    q.Push(Transition{.tsNs = timestampNs, .down = down});
}

void InputSystem::RecordFrameEvent(u64 timestampNs, ActionId action, Edge edge, bool synthetic) noexcept {
    if (m_frameInput.eventCount < kMaxFrameLogEvents) {
        m_frameInput.events[m_frameInput.eventCount++] = ActionEvent{
            .timestampNs = timestampNs,
            .action = action,
            .edge = edge,
            .synthetic = synthetic
        };
    } else {
        ++m_frameInput.droppedEvents;
    }
}

void InputSystem::RouteSource(Source src, bool down, u64 timestampNs) noexcept {
    // Evaluate active context layers top-down
    const auto actionBindings = m_bindings.GetActionBindings();
    const auto axisBindings = m_bindings.GetAxisBindings();

    auto processInContext = [&](ContextId ctx) -> bool {
        bool matchedAny = false;

        // Route actions
        for (const auto& b : actionBindings) {
            if (b.context == ctx && b.source == src && b.action < kMaxActions) {
                matchedAny = true;
                if (down) {
                    m_frameHeld |= (1u << b.action);
                    m_framePressed |= (1u << b.action);
                    RecordFrameEvent(timestampNs, b.action, Edge::Press, false);
                    PushTransition(b.action, timestampNs, true);
                } else {
                    // Only release action if no other source for it remains physically held
                    bool anyOtherDown = false;
                    for (const auto& other : actionBindings) {
                        if (other.context == ctx && other.action == b.action && !(other.source == src)) {
                            if (IsSourcePhysicallyDown(other.source)) {
                                anyOtherDown = true;
                                break;
                            }
                        }
                    }
                    if (!anyOtherDown) {
                        m_frameHeld &= ~(1u << b.action);
                        m_frameReleased |= (1u << b.action);
                        RecordFrameEvent(timestampNs, b.action, Edge::Release, false);
                        PushTransition(b.action, timestampNs, false);
                    }
                }
            }
        }

        // Route digital axes (TakeNewer timestamp recording)
        for (const auto& b : axisBindings) {
            if (b.context == ctx && b.axis < kMaxAxes) {
                if (b.negativeSource == src) {
                    matchedAny = true;
                    if (down) {
                        m_lastNegTs[b.axis] = timestampNs;
                    }
                }
                if (b.positiveSource == src) {
                    matchedAny = true;
                    if (down) {
                        m_lastPosTs[b.axis] = timestampNs;
                    }
                }
            }
        }

        return matchedAny;
    };

    if (m_contextStackDepth == 0) {
        (void)processInContext(0);
    } else {
        for (isize i = static_cast<isize>(m_contextStackDepth) - 1; i >= 0; --i) {
            const auto& layer = m_contextStack[static_cast<usize>(i)];
            const bool matched = processInContext(layer.context);
            if (matched && layer.consumes) {
                break; // Higher priority layer consumed this physical source
            }
        }
    }
}

TickInput InputSystem::CutTick() noexcept {
    return CutTick(m_autoTickIndex++);
}

TickInput InputSystem::CutTick(u64 tickIndex) noexcept {
    if (m_autoBoundaryNs == 0) {
        m_autoBoundaryNs = SDL_GetTicksNS();
    }
    m_autoBoundaryNs += kDefaultTickDurationNs;
    return CutTick(tickIndex, m_autoBoundaryNs);
}

TickInput InputSystem::CutTick(u64 tickIndex, u64 boundaryNs) noexcept {
    u32 tickPressed = 0;
    u32 tickReleased = 0;

    for (ActionId a = 0; a < kMaxActions; ++a) {
        // Consume AT MOST ONE transition per action per tick
        if (!m_queues[a].Empty() && m_queues[a].Front().tsNs < boundaryNs) {
            const Transition t = m_queues[a].Pop();
            if (t.down) {
                m_simHeld |= (1u << a);
                tickPressed |= (1u << a);
            } else {
                m_simHeld &= ~(1u << a);
                tickReleased |= (1u << a);
            }
        }

        if ((m_simHeld & (1u << a)) != 0) {
            if (m_simHeldTicks[a] < 65535) {
                ++m_simHeldTicks[a];
            }
        } else {
            m_simHeldTicks[a] = 0;
        }
    }

    // Evaluate axes with TakeNewer resolution
    i16 axisValues[kMaxAxes]{};
    for (const auto& b : m_bindings.GetAxisBindings()) {
        if (b.axis >= kMaxAxes || !IsContextActive(b.context)) continue;

        const bool negDown = IsSourcePhysicallyDown(b.negativeSource);
        const bool posDown = IsSourcePhysicallyDown(b.positiveSource);

        if (negDown && posDown) {
            if (m_lastPosTs[b.axis] > m_lastNegTs[b.axis]) {
                axisValues[b.axis] = 32767;
            } else {
                axisValues[b.axis] = -32767;
            }
        } else if (posDown) {
            axisValues[b.axis] = 32767;
        } else if (negDown) {
            axisValues[b.axis] = -32767;
        } else {
            axisValues[b.axis] = 0;
        }
    }

    TickInput result{
        .tick = tickIndex,
        .held = m_simHeld,
        .pressed = tickPressed,
        .released = tickReleased
    };

    for (usize i = 0; i < kMaxAxes; ++i) {
        result.axis[i] = axisValues[i];
    }
    for (usize i = 0; i < kMaxActions; ++i) {
        result.heldTicks[i] = m_simHeldTicks[i];
    }

    return result;
}

void InputSystem::Coalesce(u64 newBoundaryNs) noexcept {
    for (ActionId a = 0; a < kMaxActions; ++a) {
        while (!m_queues[a].Empty() && m_queues[a].Front().tsNs < newBoundaryNs) {
            const Transition t = m_queues[a].Pop();
            if (t.down) {
                m_simHeld |= (1u << a);
            } else {
                m_simHeld &= ~(1u << a);
            }
        }
    }
}

const FrameInput& InputSystem::PublishFrameInput() noexcept {
    m_frameInput.frameIndex = m_frameIndex++;
    m_frameInput.timestampNs = SDL_GetTicksNS();
    m_frameInput.held = m_frameHeld;
    m_frameInput.pressed = m_framePressed;
    m_frameInput.released = m_frameReleased;
    m_frameInput.mousePosition = m_currentMousePos;
    m_frameInput.mouseDelta = m_accumMouseDelta;
    m_frameInput.mouseWheel = m_accumMouseWheel;

    // Reset frame-scoped aggregations for the upcoming frame
    m_accumMouseDelta = Vec2{0.0f, 0.0f};
    m_accumMouseWheel = Vec2{0.0f, 0.0f};
    m_framePressed = 0;
    m_frameReleased = 0;
    m_frameInput.eventCount = 0;
    m_frameInput.droppedEvents = 0;

    return m_frameInput;
}

Result<void, InputError> InputSystem::PushContext(ContextId ctx, bool consumes) noexcept {
    if (m_contextStackDepth >= kMaxContexts) {
        return Err(InputError::ContextStackFull);
    }
    m_contextStack[m_contextStackDepth++] = ContextLayer{
        .context = ctx,
        .consumes = consumes
    };
    ResyncActionStates(SDL_GetTicksNS());
    return Ok;
}

Result<void, InputError> InputSystem::PopContext() noexcept {
    if (m_contextStackDepth == 0) {
        return Err(InputError::ContextStackEmpty);
    }
    --m_contextStackDepth;
    ResyncActionStates(SDL_GetTicksNS());
    return Ok;
}

ContextId InputSystem::GetActiveContext() const noexcept {
    if (m_contextStackDepth == 0) {
        return 0;
    }
    return m_contextStack[m_contextStackDepth - 1].context;
}

bool InputSystem::IsContextActive(ContextId ctx) const noexcept {
    if (m_contextStackDepth == 0) {
        return ctx == 0;
    }
    for (usize c = 0; c < m_contextStackDepth; ++c) {
        if (m_contextStack[c].context == ctx) {
            return true;
        }
    }
    return false;
}

void InputSystem::ResyncActionStates(u64 nowNs) noexcept {
    // Actions that lose all active physical sources get one synthetic release
    for (ActionId a = 0; a < kMaxActions; ++a) {
        bool hasActiveSource = false;
        for (const auto& b : m_bindings.GetActionBindings()) {
            if (b.action == a && IsContextActive(b.context) && IsSourcePhysicallyDown(b.source)) {
                hasActiveSource = true;
                break;
            }
        }

        if (!hasActiveSource && ((m_simHeld & (1u << a)) != 0 || (m_frameHeld & (1u << a)) != 0)) {
            m_frameHeld &= ~(1u << a);
            PushTransition(a, nowNs, false);
            RecordFrameEvent(nowNs, a, Edge::Release, true);
        }
    }

    // Physical keys currently held become latched to prevent synthetic presses
    m_keysLatched = m_keysDown;
    m_mouseLatched = m_mouseDown;
    m_gamepadLatched = m_gamepadDown;
}

bool InputSystem::IsSourcePhysicallyDown(Source src) const noexcept {
    if (!src.IsValid()) return false;
    switch (src.device) {
        case Device::Keyboard:
            return m_keysDown[src.code];
        case Device::Mouse:
            return m_mouseDown[src.code];
        case Device::Gamepad:
            return m_gamepadDown[src.code];
        default:
            return false;
    }
}

} // namespace EngineN

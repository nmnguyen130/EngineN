#include "platform/input/InputSystem.h"
#include "core/time/TimeManager.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <cmath>

namespace EngineN {

InputSystem::InputSystem() noexcept {
    static_assert(
        static_cast<int>(GamepadAxis::LeftX) - 1 == SDL_GAMEPAD_AXIS_LEFTX &&
        static_cast<int>(GamepadAxis::RightTrigger) - 1 == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
        GamepadDevice::kMaxAxes == SDL_GAMEPAD_AXIS_COUNT,
        "GamepadAxis must align with SDL gamepad axis indices"
    );

    m_bindings.SetDefaultBindings();
    m_seenRevision = m_bindings.Revision();
}

void InputSystem::Ingest(const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (!event.key.repeat) {
                SetButton(Source{Device::Keyboard, static_cast<u16>(event.key.scancode)}, event.key.down, event.key.timestamp);
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (event.button.which != SDL_TOUCH_MOUSEID) {
                SetButton(Source{Device::Mouse, event.button.button}, event.button.down, event.button.timestamp);
            }
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            m_accumFrame.mousePosition = Vec2{event.motion.x, event.motion.y};
            m_accumFrame.mouseDelta += Vec2{event.motion.xrel, event.motion.yrel};
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            const f32 flip = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
            m_accumFrame.mouseWheel += Vec2{event.wheel.x * flip, event.wheel.y * flip};
            break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            SetButton(Source{Device::Gamepad, event.gbutton.button, static_cast<u32>(event.gbutton.which)},
                      event.gbutton.down,
                      event.gbutton.timestamp);
            break;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            if (event.gaxis.axis < GamepadDevice::kMaxAxes) {
                const usize idx = FindOrCreateGamepad(static_cast<u32>(event.gaxis.which));
                if (idx < kMaxGamepads) {
                    m_gamepads[idx].axes[event.gaxis.axis] = event.gaxis.value;
                }
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED: {
            RemoveGamepad(static_cast<u32>(event.gdevice.which), event.gdevice.timestamp);
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_LOST: {
            ReleaseAll(event.window.timestamp);
            break;
        }

        default:
            break;
    }
}

TickInput InputSystem::CutTick() noexcept {
    if (m_autoBoundaryNs == 0) {
        m_autoBoundaryNs = SDL_GetTicksNS();
    }
    m_autoBoundaryNs += kFixedDtNs;
    return CutTick(m_autoTickIndex++, m_autoBoundaryNs);
}

TickInput InputSystem::CutTick(u64 tickIndex, u64 boundaryNs) noexcept {
    SyncBindings();

    TickInput out{.tick = tickIndex};
    Drain(boundaryNs, out.pressed, out.released);

    for (usize action = 0; action < kMaxActions; ++action) {
        const u32 bit = 1u << action;
        if ((m_simHeld & bit) == 0) {
            m_simHeldTicks[action] = 0;
        } else if ((out.pressed & bit) != 0) {
            m_simHeldTicks[action] = 1; // First tick of a fresh hold
        } else if (m_simHeldTicks[action] < 0xFFFF) {
            ++m_simHeldTicks[action];
        }
    }

    out.heldTicks = m_simHeldTicks;
    out.held = m_simHeld;
    out.axis = ResolveAxes();
    return out;
}

void InputSystem::Coalesce(u64 newBoundaryNs) noexcept {
    u32 pressed = 0;
    u32 released = 0;
    Drain(newBoundaryNs, pressed, released);
    if (m_autoBoundaryNs != 0) {
        m_autoBoundaryNs = std::max(m_autoBoundaryNs, newBoundaryNs);
    }
}

const FrameInput& InputSystem::PublishFrameInput() noexcept {
    SyncBindings();

    m_accumFrame.frameIndex = m_publishedFrame.frameIndex + 1;
    m_accumFrame.timestampNs = SDL_GetTicksNS();
    m_publishedFrame = m_accumFrame;

    m_accumFrame.pressed = 0;
    m_accumFrame.released = 0;
    m_accumFrame.mouseDelta = {};
    m_accumFrame.mouseWheel = {};
    m_accumFrame.eventCount = 0;
    m_accumFrame.droppedEvents = 0;
    return m_publishedFrame;
}

Result<void, InputError> InputSystem::PushContextImpl(ContextId ctx, bool consumes) noexcept {
    if (m_contextStackDepth >= kMaxContexts) {
        return Err(InputError::StackFull);
    }
    m_contextStack[m_contextStackDepth++] = ContextLayer{.context = ctx, .consumes = consumes};
    ResyncActionStates();
    return Ok;
}

Result<void, InputError> InputSystem::PopContext() noexcept {
    if (m_contextStackDepth == 0) {
        return Err(InputError::StackEmpty);
    }
    --m_contextStackDepth;
    ResyncActionStates();
    return Ok;
}

void InputSystem::SetButton(Source src, bool down, u64 tsNs) noexcept {
    if (!src.IsValid()) {
        return;
    }

    switch (src.device) {
        case Device::Keyboard:
            if (m_keysDown[src.code] == down) return;
            m_keysDown[src.code] = down;
            break;

        case Device::Mouse:
            if (m_mouseDown[src.code] == down) return;
            m_mouseDown[src.code] = down;
            break;

        case Device::Gamepad: {
            const usize idx = down ? FindOrCreateGamepad(src.gamepad) : FindGamepad(src.gamepad);
            if (idx >= kMaxGamepads) return;
            const u32 bit = 1u << src.code;
            if (((m_gamepads[idx].buttonsDown & bit) != 0) == down) return;
            if (down) {
                m_gamepads[idx].buttonsDown |= bit;
            } else {
                m_gamepads[idx].buttonsDown &= ~bit;
            }
            break;
        }

        default:
            return;
    }

    RouteSource(src, down, tsNs);
}

usize InputSystem::FindGamepad(u32 instanceId) const noexcept {
    for (usize i = 0; i < m_gamepads.size(); ++i) {
        if (m_gamepads[i].connected && m_gamepads[i].instanceId == instanceId) {
            return i;
        }
    }
    return kMaxGamepads;
}

usize InputSystem::FindOrCreateGamepad(u32 instanceId) noexcept {
    usize freeSlot = kMaxGamepads;
    for (usize i = 0; i < m_gamepads.size(); ++i) {
        if (m_gamepads[i].connected) {
            if (m_gamepads[i].instanceId == instanceId) {
                return i;
            }
        } else if (freeSlot == kMaxGamepads) {
            freeSlot = i;
        }
    }

    if (freeSlot < kMaxGamepads) {
        m_gamepads[freeSlot] = {.instanceId = instanceId, .connected = true};
    }
    return freeSlot;
}

void InputSystem::RemoveGamepad(u32 instanceId, u64 tsNs) noexcept {
    const usize idx = FindGamepad(instanceId);
    if (idx >= kMaxGamepads) {
        return;
    }

    // Drop the pad first so release checks cannot see its stale buttons
    const u32 down = m_gamepads[idx].buttonsDown;
    m_gamepads[idx] = GamepadDevice{};

    for (u32 m = down; m != 0; m &= m - 1) {
        const u16 button = static_cast<u16>(std::countr_zero(m));
        RouteSource(Source{Device::Gamepad, button, instanceId}, false, tsNs);
    }
}

void InputSystem::ReleaseAll(u64 tsNs) noexcept {
    for (ActionId action = 0; action < kMaxActions; ++action) {
        // Unconsumed sim transitions are discarded; SetAction re-queues a release if sim is held
        m_queues[action].Clear();
        SetAction(action, false, tsNs, true);
    }

    m_keysDown.fill(false);
    m_mouseDown.fill(false);
    m_lastAxisDir.fill(0);
    for (auto& pad : m_gamepads) {
        pad.buttonsDown = 0;
        pad.axes.fill(0);
    }
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

bool InputSystem::ActionHasActiveSource(ActionId action) const noexcept {
    for (const auto& b : m_bindings.GetActionBindings()) {
        if (b.action == action && IsContextActive(b.context) && IsSourcePhysicallyDown(b.source)) {
            return true;
        }
    }
    return false;
}

void InputSystem::RouteSource(Source src, bool down, u64 tsNs) noexcept {
    if (m_contextStackDepth == 0) {
        (void)RouteInContext(0, src, down, tsNs);
        return;
    }

    // Presses stop at consuming layers; releases propagate to all layers to prevent sticking.
    for (usize i = m_contextStackDepth; i-- > 0;) {
        const auto& layer = m_contextStack[i];
        const bool matched = RouteInContext(layer.context, src, down, tsNs);
        if (down && matched && layer.consumes) {
            break;
        }
    }
}

bool InputSystem::RouteInContext(ContextId ctx, Source src, bool down, u64 tsNs) noexcept {
    bool matched = false;

    for (const auto& b : m_bindings.GetActionBindings()) {
        if (b.context == ctx && b.source.Matches(src)) {
            matched = true;
            if (down) {
                SetAction(b.action, true, tsNs, false);
            } else if (!ActionHasActiveSource(b.action)) {
                SetAction(b.action, false, tsNs, false);
            }
        }
    }

    if (!down) return matched;

    for (const auto& b : m_bindings.GetAxisBindings()) {
        if (b.context != ctx) continue;
        if (b.negativeSource.Matches(src)) {
            matched = true;
            m_lastAxisDir[b.axis] = -1;
        }
        if (b.positiveSource.Matches(src)) {
            matched = true;
            m_lastAxisDir[b.axis] = 1;
        }
    }

    return matched;
}

void InputSystem::SyncBindings() noexcept {
    if (m_bindings.Revision() == m_seenRevision) return;
    m_seenRevision = m_bindings.Revision();
    ResyncActionStates();
}

void InputSystem::SetAction(ActionId action, bool down, u64 tsNs, bool synthetic) noexcept {
    const u32 bit = 1u << action;
    const bool wasHeld = (m_accumFrame.held & bit) != 0;
    if (wasHeld != down) {
        if (down) {
            m_accumFrame.held |= bit;
            m_accumFrame.pressed |= bit;
        } else {
            m_accumFrame.held &= ~bit;
            m_accumFrame.released |= bit;
        }
        if (m_accumFrame.eventCount < FrameInput::kMaxEvents) {
            m_accumFrame.events[m_accumFrame.eventCount++] = ActionEvent{
                .timestampNs = tsNs,
                .action = action,
                .edge = down ? Edge::Press : Edge::Release,
                .synthetic = synthetic
            };
        } else {
            ++m_accumFrame.droppedEvents;
        }
    }
    PushTransition(action, tsNs, down);
}

void InputSystem::PushTransition(ActionId action, u64 tsNs, bool down) noexcept {
    auto& q = m_queues[action];
    const bool current = q.Empty() ? ((m_simHeld & (1u << action)) != 0) : q.Back().down;
    if (down == current) {
        return;
    }

    if (q.Push(Transition{.tsNs = tsNs, .down = down})) {
        ++m_droppedTransitions;
    }
}

void InputSystem::Drain(u64 boundaryNs, u32& pressed, u32& released) noexcept {
    for (usize action = 0; action < kMaxActions; ++action) {
        const u32 bit = 1u << action;
        auto& q = m_queues[action];
        while (!q.Empty() && q.Front().tsNs < boundaryNs) {
            if (q.Pop().down) {
                m_simHeld |= bit;
                pressed |= bit;
            } else {
                m_simHeld &= ~bit;
                released |= bit;
            }
        }
    }
}

void InputSystem::ResyncActionStates(u64 nowNs) noexcept {
    if (nowNs == 0) {
        nowNs = SDL_GetTicksNS();
    }
    for (ActionId action = 0; action < kMaxActions; ++action) {
        if (!ActionHasActiveSource(action)) {
            SetAction(action, false, nowNs, true);
        }
    }
}

void InputSystem::ResolveContextAxes(ContextId ctx, u32& resolved, std::array<i16, kMaxAxes>& out) const noexcept {
    for (const auto& b : m_bindings.GetAxisBindings()) {
        const u32 bit = 1u << b.axis;
        if (b.context != ctx || (resolved & bit) != 0) {
            continue;
        }
        resolved |= bit;
        // |value| <= 1 by construction, so quantized result is within [-32767, 32767]
        out[b.axis] = static_cast<i16>(std::lround(EvaluateAxis(b) * 32767.0f));
    }
}

std::array<i16, kMaxAxes> InputSystem::ResolveAxes() const noexcept {
    std::array<i16, kMaxAxes> out{};
    u32 resolved = 0;
    if (m_contextStackDepth == 0) {
        ResolveContextAxes(0, resolved, out);
        return out;
    }
    for (usize i = m_contextStackDepth; i-- > 0;) {
        ResolveContextAxes(m_contextStack[i].context, resolved, out);
        if (resolved == ((1u << kMaxAxes) - 1)) break;
    }
    return out;
}

f32 InputSystem::EvaluateAxis(const AxisBinding& b) const noexcept {
    const bool neg = IsSourcePhysicallyDown(b.negativeSource);
    const bool pos = IsSourcePhysicallyDown(b.positiveSource);

    f32 digital = 0.0f;
    if (neg && pos) {
        digital = static_cast<f32>(m_lastAxisDir[b.axis]); // Last pressed wins
    } else if (pos) {
        digital = 1.0f;
    } else if (neg) {
        digital = -1.0f;
    }

    f32 analog = 0.0f;
    if (b.gamepadAxis != GamepadAxis::None) {
        const usize axisIdx = static_cast<usize>(b.gamepadAxis) - 1;
        for (const auto& pad : m_gamepads) {
            if (!pad.connected || (b.gamepad != Source::kAnyGamepad && b.gamepad != pad.instanceId)) {
                continue;
            }

            const i16 raw = pad.axes[axisIdx];
            const f32 norm = (raw < 0) ? (static_cast<f32>(raw) / 32768.0f) : (static_cast<f32>(raw) / 32767.0f);
            f32 v = ApplyAxialDeadzone(norm, b.deadzone);
            if (b.inverted) {
                v = -v;
            }
            v = std::clamp(v * b.scale, -1.0f, 1.0f);
            if (std::abs(v) > std::abs(analog)) {
                analog = v;
            }
        }
    }

    return (std::abs(analog) > std::abs(digital)) ? analog : digital;
}

bool InputSystem::IsSourcePhysicallyDown(Source src) const noexcept {
    if (!src.IsValid()) {
        return false;
    }
    switch (src.device) {
        case Device::Keyboard:
            return m_keysDown[src.code];

        case Device::Mouse:
            return m_mouseDown[src.code];

        case Device::Gamepad: {
            const u32 bit = 1u << src.code;
            if (src.gamepad == Source::kAnyGamepad) {
                for (const auto& pad : m_gamepads) {
                    if (pad.connected && ((pad.buttonsDown & bit) != 0)) {
                        return true;
                    }
                }
                return false;
            }
            const usize idx = FindGamepad(src.gamepad);
            return (idx < kMaxGamepads) && ((m_gamepads[idx].buttonsDown & bit) != 0);
        }

        default:
            return false;
    }
}

} // namespace EngineN

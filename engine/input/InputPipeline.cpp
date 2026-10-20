#include "input/InputPipeline.h"
#include <cmath>

namespace en {

void InputPipeline::ClearPhysicalState() noexcept {
    m_anyPadButtons = 0;
    m_tickTap = 0;
    m_frameTap = 0;
    m_syntheticHeld = 0;
    m_syntheticTap = 0;
    m_keyWordMask = 0;
    m_mouseBits = 0;
    m_lastAxisDir.fill(0);
    m_syntheticAxes.fill(0.0f);
    m_keyBits.fill(0);
    m_pads.fill(GamepadState{});
}

bool InputPipeline::IsPhysicallyDown(InputSource src) const noexcept {
    switch (src.device) {
        case InputDevice::Keyboard: {
            if (src.code >= 512U) return false;
            return (m_keyBits[src.code >> 6U] & (1ULL << (src.code & 63U))) != 0ULL;
        }
        case InputDevice::Mouse:
            return src.code < 8U && ((m_mouseBits & (1U << src.code)) != 0);
        case InputDevice::Gamepad: {
            if (src.code >= 32U) return false;
            const u32 bit = (1U << src.code);
            if (src.gamepadIndex >= kMaxGamepads) return (m_anyPadButtons & bit) != 0;
            return (m_pads[src.gamepadIndex].buttons & bit) != 0;
        }
        default:
            return false;
    }
}

void InputPipeline::RecordTapAndSocd(InputSource src, bool down) noexcept {
    if (down) {
        if (m_rebindCapturing) {
            m_map.BindSlot(m_rebindTargetAction, m_rebindTargetSlot, src);
            m_rebindCapturing = false;
            return;
        }
        m_frame.activeDevice = src.device;
        m_frame.lastPressed = src;
        for (u8 a = 0; a < kMaxActions; ++a) {
            if ((m_actionMask & (1U << a)) == 0) continue;
            for (const InputSource& b : m_map.ActionBindings(a)) {
                if (b.device == src.device && b.code == src.code &&
                    (b.gamepadIndex == 0xFFU || b.gamepadIndex == src.gamepadIndex)) {
                    const u16 bit = static_cast<u16>(1U << a);
                    m_tickTap |= bit;
                    m_frameTap |= bit;
                    break;
                }
            }
        }
    }
    for (u8 ax = 0; ax < kMaxAxes; ++ax) {
        const AxisBinding& ab = m_map.Axis(ax);
        if (ab.negSource.device == src.device && ab.negSource.code == src.code) {
            if (down) m_lastAxisDir[ax] = -1;
            else if (IsPhysicallyDown(ab.posSource)) m_lastAxisDir[ax] = 1;
            else m_lastAxisDir[ax] = 0;
        } else if (ab.posSource.device == src.device && ab.posSource.code == src.code) {
            if (down) m_lastAxisDir[ax] = 1;
            else if (IsPhysicallyDown(ab.negSource)) m_lastAxisDir[ax] = -1;
            else m_lastAxisDir[ax] = 0;
        }
    }
}

void InputPipeline::FeedButton(InputSource src, bool down) noexcept {
    if (!src.IsValid()) return;
    switch (src.device) {
        case InputDevice::Keyboard: {
            const u8 w = static_cast<u8>(src.code >> 6U);
            const u64 bit = 1ULL << (src.code & 63U);
            if (down) {
                m_keyBits[w] |= bit;
                m_keyWordMask |= static_cast<u8>(1U << w);
            } else {
                m_keyBits[w] &= ~bit;
                if (m_keyBits[w] == 0ULL) m_keyWordMask &= static_cast<u8>(~(1U << w));
            }
            break;
        }
        case InputDevice::Mouse: {
            if (src.code < 8U) {
                const u8 bit = static_cast<u8>(1U << src.code);
                if (down) m_mouseBits |= bit;
                else m_mouseBits &= static_cast<u8>(~bit);
            }
            break;
        }
        case InputDevice::Gamepad: {
            if (src.code < 32U) {
                const u8 slot = src.gamepadIndex < kMaxGamepads ? src.gamepadIndex : 0;
                const u32 bit = (1U << src.code);
                if (down) m_pads[slot].buttons |= bit;
                else m_pads[slot].buttons &= ~bit;
                m_anyPadButtons = m_pads[0].buttons | m_pads[1].buttons |
                                  m_pads[2].buttons | m_pads[3].buttons;
            }
            break;
        }
        default:
            return;
    }
    RecordTapAndSocd(src, down);
}

u16 InputPipeline::EvaluateLiveHeld() const noexcept {
    u16 held = static_cast<u16>(m_syntheticHeld & m_actionMask);
    if ((m_keyWordMask | m_mouseBits | m_anyPadButtons) == 0) return held;
    for (u8 a = 0; a < kMaxActions; ++a) {
        if ((m_actionMask & (1U << a)) == 0) continue;
        for (const InputSource& b : m_map.ActionBindings(a)) {
            if (IsPhysicallyDown(b)) {
                held |= static_cast<u16>(1U << a);
                break;
            }
        }
    }
    return held;
}

f32 InputPipeline::EvaluateAxis(u8 axisIdx) const noexcept {
    axisIdx &= 3U;
    if (std::abs(m_syntheticAxes[axisIdx]) > 1e-4f) {
        return m_syntheticAxes[axisIdx];
    }
    const AxisBinding& ab = m_map.Axis(axisIdx);
    const bool neg = IsPhysicallyDown(ab.negSource);
    const bool pos = IsPhysicallyDown(ab.posSource);
    f32 digital = 0.0f;
    if (neg && pos) {
        if (ab.socd == SocdPolicy::LastInputWins) digital = static_cast<f32>(m_lastAxisDir[axisIdx]);
        else if (ab.socd == SocdPolicy::FirstInputWins) digital = static_cast<f32>(-m_lastAxisDir[axisIdx]);
    } else if (neg) {
        digital = -1.0f;
    } else if (pos) {
        digital = 1.0f;
    }

    f32 analog = 0.0f;
    if (ab.gamepadAxis < 6U) {
        const u8 slot = ab.padSlot < kMaxGamepads ? ab.padSlot : 0;
        const f32 raw = static_cast<f32>(m_pads[slot].axes[ab.gamepadAxis]) * kInvAxisMax;
        if (std::abs(raw) > ab.deadzone) {
            const f32 sign = raw >= 0.0f ? 1.0f : -1.0f;
            analog = sign * std::min((std::abs(raw) - ab.deadzone) / (1.0f - ab.deadzone), 1.0f);
        }
    }
    f32 val = (std::abs(digital) >= std::abs(analog)) ? digital : analog;
    if (ab.invert) val = -val;
    return std::clamp(val * ab.sensitivity, -1.0f, 1.0f);
}

void InputPipeline::BeginFrame() noexcept {
    m_frame.mouseDelta = {0.0f, 0.0f};
    m_frame.mouseWheel = {0.0f, 0.0f};
}

const FrameInput& InputPipeline::IngestPlatformFrame(const PlatformFrameEvents& batch) noexcept {
    BeginFrame();
    if (batch.focusCleared != 0) {
        ClearPhysicalState();
        m_frame.hasFocus = false;
    } else {
        m_frame.hasFocus = true;
    }
    if (batch.hasMouseMotion != 0) {
        m_frame.mousePos = {batch.mouseX, batch.mouseY};
        m_frame.mouseDelta = {batch.mouseDx, batch.mouseDy};
        m_frame.activeDevice = InputDevice::Mouse;
    }
    m_frame.mouseWheel = {batch.wheelX, batch.wheelY};

    for (const PlatformInputEvent& ev : batch.Events()) {
        switch (ev.Kind()) {
            case InputEventKind::Key:
                FeedButton(InputSource::Key(ev.Code()), ev.Down());
                break;
            case InputEventKind::MouseButton:
                FeedButton(InputSource::Mouse(ev.Code()), ev.Down());
                break;
            case InputEventKind::GamepadButton:
                FeedButton(InputSource::Pad(ev.Code(), ev.PadSlot()), ev.Down());
                break;
            case InputEventKind::GamepadAxis:
                if (ev.PadSlot() < kMaxGamepads && ev.Code() < 6U) {
                    m_pads[ev.PadSlot()].axes[ev.Code()] = ev.value;
                }
                break;
        }
    }
    return EndFrame();
}

const FrameInput& InputPipeline::EndFrame() noexcept {
    ++m_frame.frameIndex;
    const u16 live = EvaluateLiveHeld();
    const u16 frameHeld = static_cast<u16>(live | m_frameTap);
    m_frame.held = frameHeld;
    m_frame.pressed = static_cast<u16>((frameHeld & ~m_prevFrameHeld) | m_frameTap);
    m_frame.released = static_cast<u16>(~frameHeld & m_prevFrameHeld);
    m_prevFrameHeld = frameHeld;
    m_frameTap = 0;
    return m_frame;
}

TickInput InputPipeline::PollTick(u64 tickIndex) noexcept {
    const u16 live = EvaluateLiveHeld();
    const u16 tickHeld = static_cast<u16>(live | m_tickTap | m_syntheticTap);
    const u16 pressed = static_cast<u16>((tickHeld & ~m_prevTickHeld) | m_tickTap | m_syntheticTap);
    const u16 released = static_cast<u16>(~tickHeld & m_prevTickHeld);
    m_prevTickHeld = tickHeld;
    m_tickTap = 0;
    m_syntheticTap = 0;

    u16 relMask = released;
    while (relMask != 0) {
        const u8 idx = static_cast<u8>(std::countr_zero(relMask));
        relMask &= static_cast<u16>(relMask - 1U);
        m_heldTicks[idx] = 0;
    }
    u16 heldMask = tickHeld;
    while (heldMask != 0) {
        const u8 idx = static_cast<u8>(std::countr_zero(heldMask));
        heldMask &= static_cast<u16>(heldMask - 1U);
        if (m_heldTicks[idx] < 0xFFFFU) ++m_heldTicks[idx];
    }

    TickInput out{};
    out.tick = tickIndex;
    out.held = tickHeld;
    out.pressed = pressed;
    out.released = released;
    out.heldTicks = m_heldTicks;
    for (u8 ax = 0; ax < kMaxAxes; ++ax) {
        out.axis[ax] = EvaluateAxis(ax);
    }

    for (u8 c = 0; c < 4U; ++c) {
        const InputChord& chord = m_map.Chord(c);
        if (chord.requiredMask != 0 && (tickHeld & chord.requiredMask) == chord.requiredMask &&
            (pressed & chord.requiredMask) != 0) {
            out.chordMask |= static_cast<u16>(1U << c);
            const u16 outBit = static_cast<u16>(1U << (chord.outputAction & 0x0FU));
            out.held |= outBit;
            out.pressed |= outBit;
        }
    }

    for (u8 s = 0; s < 4U; ++s) {
        InputSequence& seq = m_map.Sequence(s);
        if (seq.stepCount < 2 || seq.stepCount > 4) continue;
        if (seq.currentStep > 0) {
            if (++seq.ticksSinceLastStep > seq.maxTicksBetweenSteps) {
                seq.currentStep = 0;
                seq.ticksSinceLastStep = 0;
            }
        }
        const u8 nextAction = seq.steps[seq.currentStep] & 0x0FU;
        if ((pressed & (1U << nextAction)) != 0) {
            seq.ticksSinceLastStep = 0;
            if (++seq.currentStep >= seq.stepCount) {
                seq.currentStep = 0;
                const u16 outBit = static_cast<u16>(1U << (seq.outputAction & 0x0FU));
                out.held |= outBit;
                out.pressed |= outBit;
            }
        }
    }
    return out;
}

u64 InputPipeline::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_prevTickHeld) | (static_cast<u64>(m_actionMask) << 16U));
    for (u16 ht : m_heldTicks) h = HashCombine64(h, ht);
    return h;
}

} // namespace en

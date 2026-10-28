#pragma once

#include "input/ActionBuffer.h"

namespace en {

// Translates platform events into FrameInput and deterministic 120 Hz TickInput snapshots.
// Handles bindings, SOCD, and sub-frame tap preservation with zero runtime allocations.
class alignas(16) InputPipeline {
public:
    constexpr void Reset() noexcept {
        ClearPhysical();
        m_heldTicks.fill(0U);
        m_actionMask = 0xFFFFU;
        m_prevTickHeld = 0U;
        m_prevFrameHeld = 0U;
        m_rebind = 0U;
        m_frame = {};
    }

    constexpr void ClearPhysical() noexcept {
        m_padButtons = 0U;
        m_padAxes.fill(0);
        m_tickTap = 0U;
        m_frameTap = 0U;
        m_axisState.fill(0U);
        m_mouseBits = 0U;
        m_keyBits.fill(0ULL);
    }

    template <typename BatchT>
    constexpr const FrameInput& Ingest(const BatchT& batch) noexcept {
        m_frame.mouseDelta = {};
        m_frame.mouseWheel = {};
        m_frame.textLen = 0U;
        m_frame.textInput[0] = '\0';

        const bool focused = batch.focused != 0U;
        if (!focused && m_frame.hasFocus) ClearPhysical();
        m_frame.hasFocus = focused;

        if (batch.padDisconnected != 0U) {
            m_padButtons = 0U;
            m_padAxes.fill(0);
        }

        m_frame.mousePos = {batch.mouseX, batch.mouseY};
        m_frame.mouseDelta = {batch.mouseDx, batch.mouseDy};
        m_frame.mouseWheel = {batch.wheelX, batch.wheelY};
        if (m_frame.mouseDelta.LengthSq() > 4.0f ||
            m_frame.mouseWheel.x != 0.0f || m_frame.mouseWheel.y != 0.0f) {
            m_frame.activeDevice = InputDevice::Mouse;
        }

        CopyText(batch.TextView());

        for (const auto& ev : batch.Events()) {
            switch (static_cast<u8>(ev.Kind())) {
                case 0: Feed(InputSource::Key(ev.Code()), ev.Down()); break;
                case 1: Feed(InputSource::Mouse(ev.Code()), ev.Down()); break;
                case 2: Feed(InputSource::Pad(ev.Code()), ev.Down()); break;
                case 3: FeedAxis(static_cast<u8>(ev.Code()), ev.value); break;
                default: break;
            }
        }

        ++m_frame.frameIndex;
        const u16 held = static_cast<u16>(LiveHeld() | m_frameTap);
        const u16 retrig = static_cast<u16>(m_frameTap & m_prevFrameHeld);
        m_frame.held = held;
        m_frame.pressed = static_cast<u16>((held & ~m_prevFrameHeld) | m_frameTap);
        m_frame.released = static_cast<u16>((~held & m_prevFrameHeld) | retrig);
        m_prevFrameHeld = held;
        m_frameTap = 0U;
        return m_frame;
    }

    [[nodiscard]] constexpr const FrameInput& Frame() const noexcept { return m_frame; }

    [[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept {
        const u16 held = static_cast<u16>(LiveHeld() | m_tickTap);
        const u16 retrig = static_cast<u16>(m_tickTap & m_prevTickHeld);
        const u16 pressed = static_cast<u16>((held & ~m_prevTickHeld) | m_tickTap);
        const u16 released = static_cast<u16>((~held & m_prevTickHeld) | retrig);
        m_prevTickHeld = held;
        m_tickTap = 0U;

        TickInput out{};
        out.tick = tickIndex;
        out.held = held;
        out.pressed = pressed;
        out.released = released;
        out.retriggered = retrig;

        // Release keeps final heldTicks for TapReleased / ChargeRatio.
        for (u16 rel = released; rel != 0U;) {
            const u8 i = static_cast<u8>(std::countr_zero(rel));
            rel &= static_cast<u16>(rel - 1U);
            out.heldTicks[i] = m_heldTicks[i];
            m_heldTicks[i] = 0U;
        }
        for (u16 h = held; h != 0U;) {
            const u8 i = static_cast<u8>(std::countr_zero(h));
            h &= static_cast<u16>(h - 1U);
            if (m_heldTicks[i] < 0xFFFFU) ++m_heldTicks[i];
            out.heldTicks[i] = m_heldTicks[i];
        }
        for (u8 a = 0U; a < kMaxAxes; ++a) out.axis[a] = EvalAxis(a);
        return out;
    }

    [[nodiscard]] constexpr InputMap& Map() noexcept { return m_map; }
    [[nodiscard]] constexpr const InputMap& Map() const noexcept { return m_map; }

    constexpr void BeginRebind(u8 action, u8 slot = 0) noexcept {
        m_rebind = static_cast<u8>(0x80U | ((slot & 3U) << 4U) | (action & 0x0FU));
    }
    constexpr void CancelRebind() noexcept { m_rebind = 0U; }
    [[nodiscard]] constexpr bool WaitingRebind() const noexcept { return (m_rebind & 0x80U) != 0U; }

    constexpr void SetActionMask(u16 mask) noexcept {
        m_actionMask = mask;
        m_tickTap &= mask;
        m_frameTap &= mask;
    }
    [[nodiscard]] constexpr u16 ActionMask() const noexcept { return m_actionMask; }

    // Bounds-checked. Safe for any packed value.
    [[nodiscard]] constexpr bool IsDown(InputSource src) const noexcept {
        if (!src.IsValid()) return false;
        const u16 c = src.Code();
        switch (src.Device()) {
            case InputDevice::Keyboard: {
                const usize w = static_cast<usize>(c >> 6U);
                return w < m_keyBits.size() &&
                       (m_keyBits[w] & (1ULL << (c & 63U))) != 0ULL;
            }
            case InputDevice::Mouse:
                return c < 8U && (m_mouseBits & (1U << c)) != 0U;
            case InputDevice::Gamepad:
                return c < 32U && (m_padButtons & (1U << c)) != 0U;
            default:
                return false;
        }
    }

private:
    static constexpr u8 kLastNeg = 1, kLastPos = 2, kTapNeg = 4, kTapPos = 8;

    constexpr void CopyText(std::string_view txt) noexcept {
        usize n = txt.size();
        if (n > FrameInput::kMaxTextBytes) {
            n = FrameInput::kMaxTextBytes;
            // Walk back only when truncated; never index txt[txt.size()].
            while (n > 0U && n < txt.size() &&
                   (static_cast<u8>(txt[n]) & 0xC0U) == 0x80U) {
                --n;
            }
        }
        for (usize i = 0U; i < n; ++i) m_frame.textInput[i] = txt[i];
        m_frame.textLen = static_cast<u8>(n);
        m_frame.textInput[n] = '\0';
    }

    constexpr void Feed(InputSource src, bool down) noexcept {
        if (!src.IsValid()) return;
        // Avoid double work: compute physical state once.
        const bool was = IsDown(src);
        if (was == down) return;

        const u16 c = src.Code();
        switch (src.Device()) {
            case InputDevice::Keyboard: {
                const usize w = static_cast<usize>(c >> 6U);
                if (w >= m_keyBits.size()) return;
                const u64 bit = 1ULL << (c & 63U);
                if (down) m_keyBits[w] |= bit;
                else m_keyBits[w] &= ~bit;
                break;
            }
            case InputDevice::Mouse: {
                if (c >= 8U) return;
                const u8 bit = static_cast<u8>(1U << c);
                if (down) m_mouseBits |= bit;
                else m_mouseBits &= static_cast<u8>(~bit);
                break;
            }
            case InputDevice::Gamepad: {
                if (c >= 32U) return;
                const u32 bit = 1U << c;
                if (down) m_padButtons |= bit;
                else m_padButtons &= ~bit;
                break;
            }
            default:
                return;
        }
        OnEdge(src, down);
    }

    constexpr void FeedAxis(u8 idx, i16 value) noexcept {
        if (idx >= 6U) return;
        const i16 v = (value == static_cast<i16>(-32768)) ? static_cast<i16>(-32767) : value;
        m_padAxes[idx] = v;

        const u16 neg = static_cast<u16>(kPadVirtualButtonBase + idx * 2U);
        const u16 pos = static_cast<u16>(neg + 1U);
        const u32 negBit = 1U << neg;
        const u32 posBit = 1U << pos;
        const bool wasN = (m_padButtons & negBit) != 0U;
        const bool wasP = (m_padButtons & posBit) != 0U;
        const bool nowN = wasN ? (v < -kAxisReleaseThreshold) : (v <= -kAxisPressThreshold);
        const bool nowP = wasP ? (v > kAxisReleaseThreshold) : (v >= kAxisPressThreshold);
        if (nowN != wasN) Feed(InputSource::Pad(neg), nowN);
        if (nowP != wasP) Feed(InputSource::Pad(pos), nowP);
        if (std::abs(static_cast<i32>(v)) > 8192) m_frame.activeDevice = InputDevice::Gamepad;
    }

    [[nodiscard]] constexpr bool DirDown(InputSource a, InputSource b) const noexcept {
        return IsDown(a) || IsDown(b);
    }

    [[nodiscard]] constexpr u16 LiveHeld() const noexcept {
        u16 held = 0U;
        for (u16 en = m_actionMask; en != 0U;) {
            const u8 a = static_cast<u8>(std::countr_zero(en));
            en &= static_cast<u16>(en - 1U);
            for (const InputSource s : m_map.Bindings(a)) {
                if (IsDown(s)) {
                    held |= static_cast<u16>(1U << a);
                    break;
                }
            }
        }
        return held;
    }

    [[nodiscard]] constexpr f32 EvalAxis(u8 idx) noexcept {
        idx &= 3U;
        const u8 st = m_axisState[idx];
        m_axisState[idx] = static_cast<u8>(st & static_cast<u8>(~(kTapNeg | kTapPos)));

        const AxisBinding& ab = m_map.Axis(idx);
        const bool neg = DirDown(ab.neg, ab.altNeg) || (st & kTapNeg) != 0U;
        const bool pos = DirDown(ab.pos, ab.altPos) || (st & kTapPos) != 0U;

        f32 dig = 0.0f;
        if (neg && pos) {
            if (!ab.NeutralSocd())
                dig = (st & kLastPos) != 0U ? 1.0f : ((st & kLastNeg) != 0U ? -1.0f : 0.0f);
        } else if (neg) {
            dig = -1.0f;
        } else if (pos) {
            dig = 1.0f;
        }

        f32 ana = 0.0f;
        if (ab.gamepadAxis < 6U) {
            const f32 raw = static_cast<f32>(m_padAxes[ab.gamepadAxis]) * kInvAxisMax;
            const f32 ar = std::abs(raw);
            const f32 dz = ab.deadzone;
            // dz sanitized to [0, 0.90] -> divisor in [0.10, 1].
            if (ar > dz) {
                ana = (raw >= 0.0f ? 1.0f : -1.0f) *
                      std::clamp((ar - dz) / (1.0f - dz), 0.0f, 1.0f);
            }
        }

        const f32 v = (std::abs(dig) >= std::abs(ana)) ? dig : ana;
        return ab.Invert() ? -v : v;
    }

    constexpr void OnEdge(InputSource src, bool down) noexcept {
        if (down) {
            if ((m_rebind & 0x80U) != 0U) {
                m_map.BindSlot(m_rebind & 0x0FU, static_cast<u8>((m_rebind >> 4U) & 3U), src);
                m_rebind = 0U;
                return;
            }
            m_frame.activeDevice = src.Device();
            m_frame.lastPressed = src;

            for (u16 en = m_actionMask; en != 0U;) {
                const u8 a = static_cast<u8>(std::countr_zero(en));
                en &= static_cast<u16>(en - 1U);
                bool match = false, already = false;
                for (const InputSource s : m_map.Bindings(a)) {
                    if (s == src) match = true;
                    else if (IsDown(s)) already = true;
                }
                if (match && !already) {
                    const u16 b = static_cast<u16>(1U << a);
                    m_tickTap |= b;
                    m_frameTap |= b;
                }
            }
        }

        // SOCD last-input tracking.
        for (u8 ax = 0U; ax < kMaxAxes; ++ax) {
            const AxisBinding& ab = m_map.Axis(ax);
            if (ab.neg == src || ab.altNeg == src) {
                if (down) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & static_cast<u8>(~kLastPos)) | kLastNeg | kTapNeg);
                } else if (!DirDown(ab.neg, ab.altNeg)) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & static_cast<u8>(~(kLastNeg | kLastPos))) |
                        (DirDown(ab.pos, ab.altPos) ? kLastPos : 0U));
                }
            } else if (ab.pos == src || ab.altPos == src) {
                if (down) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & static_cast<u8>(~kLastNeg)) | kLastPos | kTapPos);
                } else if (!DirDown(ab.pos, ab.altPos)) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & static_cast<u8>(~(kLastNeg | kLastPos))) |
                        (DirDown(ab.neg, ab.altNeg) ? kLastNeg : 0U));
                }
            }
        }
    }

    FrameInput m_frame{};
    std::array<u64, 8> m_keyBits{};
    InputMap m_map{};
    std::array<u16, kMaxActions> m_heldTicks{};
    std::array<i16, 6> m_padAxes{};
    u32 m_padButtons{0};
    u16 m_actionMask{0xFFFFU};
    u16 m_tickTap{0};
    u16 m_frameTap{0};
    u16 m_prevTickHeld{0};
    u16 m_prevFrameHeld{0};
    std::array<u8, kMaxAxes> m_axisState{};
    u8 m_mouseBits{0};
    u8 m_rebind{0};
};
static_assert(sizeof(InputPipeline) == 384);

} // namespace en

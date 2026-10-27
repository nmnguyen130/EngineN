#pragma once

#include "input/ActionBuffer.h"

namespace en {

// In-RAM run-length compressed deterministic tick input stream for instant replays and tests.
// Value type; records all 4 axes in Q14 fixed-point and reconstructs heldTicks on playback.
// Invariant: SamplePlaybackTick reproduces held, pressed, released, heldTicks, and 4-axis values.
class alignas(4) InputReplayStream {
public:
    static constexpr usize kMaxSegments = 256;

    struct alignas(4) Segment {
        u16 held{0};
        u16 durationTicks{0};
        std::array<i16, kMaxAxes> axisQ14{};
    };
    static_assert(sizeof(Segment) == 12);

    constexpr void Clear() noexcept {
        m_count = 0U;
        m_playSegment = 0U;
        m_playTickInSeg = 0U;
        m_playHeldTicks.fill(0U);
    }

    constexpr void RecordTick(const TickInput& in) noexcept {
        std::array<i16, kMaxAxes> qAxes{};
        for (usize i = 0U; i < kMaxAxes; ++i) {
            qAxes[i] = static_cast<i16>(std::clamp(in.axis[i], -1.0f, 1.0f) * 16384.0f);
        }

        if (m_count > 0U) {
            Segment& last = m_segments[m_count - 1U];
            if (last.held == in.held && last.axisQ14 == qAxes && last.durationTicks < 0xFFFFU) {
                ++last.durationTicks;
                return;
            }
        }
        if (m_count < kMaxSegments) {
            m_segments[m_count++] = Segment{in.held, 1U, qAxes};
        }
    }

    constexpr void RewindPlayback() noexcept {
        m_playSegment = 0U;
        m_playTickInSeg = 0U;
        m_playHeldTicks.fill(0U);
    }

    [[nodiscard]] constexpr bool SamplePlaybackTick(
        u64 tickIndex, TickInput& outTick, u16& ioPrevHeld) noexcept {
        if (m_playSegment >= m_count) return false;

        const Segment& seg = m_segments[m_playSegment];
        const u16 pressed = static_cast<u16>(seg.held & ~ioPrevHeld);
        const u16 released = static_cast<u16>(~seg.held & ioPrevHeld);

        outTick = TickInput{
            .tick = tickIndex,
            .held = seg.held,
            .pressed = pressed,
            .released = released,
        };

        u16 relMask = released;
        while (relMask != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(relMask));
            relMask &= static_cast<u16>(relMask - 1U);
            outTick.heldTicks[idx] = m_playHeldTicks[idx];
            m_playHeldTicks[idx] = 0U;
        }

        u16 heldMask = seg.held;
        while (heldMask != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(heldMask));
            heldMask &= static_cast<u16>(heldMask - 1U);
            if (m_playHeldTicks[idx] < 0xFFFFU) ++m_playHeldTicks[idx];
            outTick.heldTicks[idx] = m_playHeldTicks[idx];
        }

        for (usize i = 0U; i < kMaxAxes; ++i) {
            outTick.axis[i] = static_cast<f32>(seg.axisQ14[i]) * (1.0f / 16384.0f);
        }
        ioPrevHeld = seg.held;
        if (++m_playTickInSeg >= seg.durationTicks) {
            m_playTickInSeg = 0U;
            ++m_playSegment;
        }
        return true;
    }

    [[nodiscard]] constexpr u16 SegmentCount() const noexcept { return m_count; }
    [[nodiscard]] constexpr bool IsPlaybackExhausted() const noexcept { return m_playSegment >= m_count; }

private:
    std::array<Segment, kMaxSegments> m_segments{};
    std::array<u16, kMaxActions> m_playHeldTicks{};
    u16 m_count{0};
    u16 m_playSegment{0};
    u16 m_playTickInSeg{0};
};
static_assert(sizeof(InputReplayStream) == 3112);

// Pull-evaluated input pipeline translating physical device state into FrameInput and 120 Hz TickInput snapshots.
// Self-contained single-controller value type with zero OS header dependencies.
class alignas(16) InputPipeline {
public:
    constexpr void Reset() noexcept {
        ClearPhysicalState();
        m_heldTicks.fill(0U);
        m_actionMask = 0xFFFFU;
        m_prevTickHeld = 0U;
        m_prevFrameHeld = 0U;
        m_rebindState = 0U;
        m_frame = FrameInput{};
    }

    constexpr void ClearPhysicalState() noexcept {
        m_padButtons = 0U;
        m_padAxes.fill(0);
        m_tickTap = 0U;
        m_frameTap = 0U;
        m_axisState.fill(0U);
        m_mouseBits = 0U;
        m_keyBits.fill(0ULL);
    }

    constexpr void DisconnectGamepad() noexcept {
        m_padButtons = 0U;
        m_padAxes.fill(0);
    }

    constexpr void BeginFrame() noexcept {
        m_frame.mouseDelta = {0.0f, 0.0f};
        m_frame.mouseWheel = {0.0f, 0.0f};
        m_frame.textLen = 0U;
        m_frame.textInput[0] = '\0';
    }

    template <typename FrameBatchT>
    constexpr const FrameInput& IngestPlatformFrame(const FrameBatchT& batch) noexcept {
        BeginFrame();

        const bool focused = batch.focused != 0U;
        if (!focused && m_frame.hasFocus) {
            ClearPhysicalState();
        }
        m_frame.hasFocus = focused;

        if (batch.padDisconnected != 0U) {
            DisconnectGamepad();
        }

        const Vec2 newMouseDelta{batch.mouseDx, batch.mouseDy};
        const Vec2 newWheel{batch.wheelX, batch.wheelY};
        m_frame.mousePos = {batch.mouseX, batch.mouseY};
        m_frame.mouseDelta = newMouseDelta;
        m_frame.mouseWheel = newWheel;
        if (newMouseDelta.LengthSq() > 4.0f || newWheel.x != 0.0f || newWheel.y != 0.0f) {
            m_frame.activeDevice = InputDevice::Mouse;
        }

        const std::string_view txt = batch.TextView();
        usize copyLen = std::min<usize>(txt.size(), FrameInput::kMaxTextBytes);
        if (copyLen < txt.size()) {
            while (copyLen > 0U && (static_cast<u8>(txt[copyLen]) & 0xC0U) == 0x80U) {
                --copyLen;
            }
        }
        for (usize i = 0U; i < copyLen; ++i) m_frame.textInput[i] = txt[i];
        m_frame.textLen = static_cast<u8>(copyLen);
        m_frame.textInput[copyLen] = '\0';

        for (const auto& ev : batch.Events()) {
            switch (ev.Kind()) {
                case InputEventKind::Key:
                    FeedButton(InputSource::Key(ev.Code()), ev.Down());
                    break;
                case InputEventKind::MouseButton:
                    FeedButton(InputSource::Mouse(ev.Code()), ev.Down());
                    break;
                case InputEventKind::GamepadButton:
                    FeedButton(InputSource::Pad(ev.Code()), ev.Down());
                    break;
                case InputEventKind::GamepadAxis:
                    FeedGamepadAxis(static_cast<u8>(ev.Code()), ev.value);
                    break;
                default: break;
            }
        }
        return EndFrame();
    }

    [[nodiscard]] constexpr const FrameInput& EndFrame() noexcept {
        ++m_frame.frameIndex;
        const u16 frameHeld = static_cast<u16>(EvaluateLiveHeld() | m_frameTap);
        const u16 retriggered = static_cast<u16>(m_frameTap & m_prevFrameHeld);
        m_frame.held = frameHeld;
        m_frame.pressed = static_cast<u16>((frameHeld & ~m_prevFrameHeld) | m_frameTap);
        m_frame.released = static_cast<u16>((~frameHeld & m_prevFrameHeld) | retriggered);
        m_prevFrameHeld = frameHeld;
        m_frameTap = 0U;
        return m_frame;
    }

    [[nodiscard]] constexpr const FrameInput& GetFrameView() const noexcept { return m_frame; }

    [[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept {
        const u16 tickHeld = static_cast<u16>(EvaluateLiveHeld() | m_tickTap);
        const u16 retriggered = static_cast<u16>(m_tickTap & m_prevTickHeld);
        const u16 pressed = static_cast<u16>((tickHeld & ~m_prevTickHeld) | m_tickTap);
        const u16 released = static_cast<u16>((~tickHeld & m_prevTickHeld) | retriggered);
        m_prevTickHeld = tickHeld;
        m_tickTap = 0U;

        TickInput out{
            .tick = tickIndex,
            .held = tickHeld,
            .pressed = pressed,
            .released = released,
            .retriggered = retriggered,
        };

        // Retain final hold duration in out.heldTicks on release ticks for TapReleased/WasChargedRelease.
        u16 relMask = released;
        while (relMask != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(relMask));
            relMask &= static_cast<u16>(relMask - 1U);
            out.heldTicks[idx] = m_heldTicks[idx];
            m_heldTicks[idx] = 0U;
        }

        u16 heldMask = tickHeld;
        while (heldMask != 0U) {
            const u8 idx = static_cast<u8>(std::countr_zero(heldMask));
            heldMask &= static_cast<u16>(heldMask - 1U);
            if (m_heldTicks[idx] < 0xFFFFU) ++m_heldTicks[idx];
            out.heldTicks[idx] = m_heldTicks[idx];
        }

        for (u8 ax = 0U; ax < kMaxAxes; ++ax) {
            out.axis[ax] = EvaluateAxis(ax);
        }

        return out;
    }

    [[nodiscard]] constexpr InputMap& Map() noexcept { return m_map; }
    [[nodiscard]] constexpr const InputMap& Map() const noexcept { return m_map; }

    constexpr void BeginRebindCapture(u8 targetAction, u8 targetSlot = 0) noexcept {
        m_rebindState = static_cast<u8>(0x80U | ((targetSlot & 3U) << 4U) | (targetAction & 0x0FU));
    }

    constexpr void CancelRebindCapture() noexcept { m_rebindState = 0U; }
    [[nodiscard]] constexpr bool IsWaitingForRebind() const noexcept { return (m_rebindState & 0x80U) != 0U; }

    constexpr void SetActionMask(u16 mask) noexcept {
        m_actionMask = mask;
        m_tickTap &= mask;
        m_frameTap &= mask;
    }

    [[nodiscard]] constexpr u16 GetActionMask() const noexcept { return m_actionMask; }

    constexpr void FeedButton(InputSource src, bool down) noexcept {
        if (!src.IsValid() || IsPhysicallyDown(src) == down) return;
        const u16 code = src.Code();
        switch (src.Device()) {
            case InputDevice::Keyboard: {
                const u64 bit = 1ULL << (code & 63U);
                if (down) m_keyBits[code >> 6U] |= bit;
                else m_keyBits[code >> 6U] &= ~bit;
                break;
            }
            case InputDevice::Mouse: {
                const u8 bit = static_cast<u8>(1U << code);
                if (down) m_mouseBits |= bit;
                else m_mouseBits &= static_cast<u8>(~bit);
                break;
            }
            case InputDevice::Gamepad: {
                const u32 bit = 1U << code;
                if (down) m_padButtons |= bit;
                else m_padButtons &= ~bit;
                break;
            }
            default:
                return;
        }
        RecordTapAndSocd(src, down);
    }

    constexpr void FeedGamepadAxis(u8 axisIdx, i16 value) noexcept {
        if (axisIdx >= 6U) return;
        const i16 clamped = (value == -32768) ? i16{-32767} : value;
        m_padAxes[axisIdx] = clamped;

        const u16 negBtn = static_cast<u16>(kPadVirtualButtonBase + static_cast<u16>(axisIdx) * 2U);
        const u16 posBtn = static_cast<u16>(negBtn + 1U);
        const bool wasNegDown = (m_padButtons & (1U << negBtn)) != 0U;
        const bool wasPosDown = (m_padButtons & (1U << posBtn)) != 0U;

        const bool nowNegDown = wasNegDown ? (clamped < -kAxisReleaseThreshold) : (clamped <= -kAxisPressThreshold);
        const bool nowPosDown = wasPosDown ? (clamped > kAxisReleaseThreshold) : (clamped >= kAxisPressThreshold);

        if (nowNegDown != wasNegDown) FeedButton(InputSource::Pad(negBtn), nowNegDown);
        if (nowPosDown != wasPosDown) FeedButton(InputSource::Pad(posBtn), nowPosDown);

        if (std::abs(static_cast<i32>(clamped)) > 8192) {
            m_frame.activeDevice = InputDevice::Gamepad;
        }
    }

    [[nodiscard]] constexpr bool IsPhysicallyDown(InputSource src) const noexcept {
        const u16 code = src.Code();
        switch (src.Device()) {
            case InputDevice::Keyboard:
                return (m_keyBits[code >> 6U] & (1ULL << (code & 63U))) != 0ULL;
            case InputDevice::Mouse:
                return (m_mouseBits & (1U << code)) != 0U;
            case InputDevice::Gamepad:
                return (m_padButtons & (1U << code)) != 0U;
            default:
                return false;
        }
    }

private:
    static constexpr u8 kAxisLastNeg = 1U << 0;
    static constexpr u8 kAxisLastPos = 1U << 1;
    static constexpr u8 kAxisTapNeg  = 1U << 2;
    static constexpr u8 kAxisTapPos  = 1U << 3;

    [[nodiscard]] constexpr bool IsAxisDirDown(InputSource primary, InputSource alt) const noexcept {
        return IsPhysicallyDown(primary) || IsPhysicallyDown(alt);
    }

    [[nodiscard]] constexpr u16 EvaluateLiveHeld() const noexcept {
        u16 held = 0U;
        u16 enabled = m_actionMask;
        while (enabled != 0U) {
            const u8 a = static_cast<u8>(std::countr_zero(enabled));
            enabled &= static_cast<u16>(enabled - 1U);
            for (const InputSource b : m_map.ActionBindings(a)) {
                if (IsPhysicallyDown(b)) {
                    held |= static_cast<u16>(1U << a);
                    break;
                }
            }
        }
        return held;
    }

    [[nodiscard]] constexpr f32 EvaluateAxis(u8 axisIdx) noexcept {
        axisIdx &= 3U;
        const u8 st = m_axisState[axisIdx];
        m_axisState[axisIdx] = static_cast<u8>(st & ~(kAxisTapNeg | kAxisTapPos));

        const AxisBinding& ab = m_map.Axis(axisIdx);
        const bool neg = IsAxisDirDown(ab.negSource, ab.altNegSource) || ((st & kAxisTapNeg) != 0U);
        const bool pos = IsAxisDirDown(ab.posSource, ab.altPosSource) || ((st & kAxisTapPos) != 0U);
        f32 digital = 0.0f;
        if (neg && pos) {
            if (!ab.neutralSocd) {
                digital = (st & kAxisLastPos) != 0U ? 1.0f : ((st & kAxisLastNeg) != 0U ? -1.0f : 0.0f);
            }
        } else if (neg) {
            digital = -1.0f;
        } else if (pos) {
            digital = 1.0f;
        }

        f32 analog = 0.0f;
        if (ab.gamepadAxis < 6U) {
            const f32 raw = static_cast<f32>(m_padAxes[ab.gamepadAxis]) * kInvAxisMax;
            const f32 absRaw = std::abs(raw);
            if (absRaw > ab.deadzone) {
                const f32 sign = raw >= 0.0f ? 1.0f : -1.0f;
                analog = sign * std::clamp((absRaw - ab.deadzone) / (1.0f - ab.deadzone), 0.0f, 1.0f);
            }
        }

        const f32 val = (std::abs(digital) >= std::abs(analog)) ? digital : analog;
        return ab.invert ? -val : val;
    }

    constexpr void RecordTapAndSocd(InputSource src, bool down) noexcept {
        if (down) {
            if ((m_rebindState & 0x80U) != 0U) {
                const u8 targetAction = m_rebindState & 0x0FU;
                const u8 targetSlot = static_cast<u8>((m_rebindState >> 4U) & 3U);
                m_map.BindSlot(targetAction, targetSlot, src);
                m_rebindState = 0U;
                return;
            }
            m_frame.activeDevice = src.Device();
            m_frame.lastPressed = src;

            u16 enabled = m_actionMask;
            while (enabled != 0U) {
                const u8 a = static_cast<u8>(std::countr_zero(enabled));
                enabled &= static_cast<u16>(enabled - 1U);
                bool matchedSrc = false;
                bool alreadyDown = false;
                for (const InputSource b : m_map.ActionBindings(a)) {
                    if (b == src) matchedSrc = true;
                    else if (IsPhysicallyDown(b)) alreadyDown = true;
                }
                if (matchedSrc && !alreadyDown) {
                    const u16 bit = static_cast<u16>(1U << a);
                    m_tickTap |= bit;
                    m_frameTap |= bit;
                }
            }
        }

        for (u8 ax = 0U; ax < kMaxAxes; ++ax) {
            const AxisBinding& ab = m_map.Axis(ax);
            if (ab.negSource == src || ab.altNegSource == src) {
                if (down) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~kAxisLastPos) | kAxisLastNeg | kAxisTapNeg);
                } else if (!IsAxisDirDown(ab.negSource, ab.altNegSource)) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~(kAxisLastNeg | kAxisLastPos)) |
                        (IsAxisDirDown(ab.posSource, ab.altPosSource) ? kAxisLastPos : 0U));
                }
            } else if (ab.posSource == src || ab.altPosSource == src) {
                if (down) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~kAxisLastNeg) | kAxisLastPos | kAxisTapPos);
                } else if (!IsAxisDirDown(ab.posSource, ab.altPosSource)) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~(kAxisLastNeg | kAxisLastPos)) |
                        (IsAxisDirDown(ab.negSource, ab.altNegSource) ? kAxisLastNeg : 0U));
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
    u8 m_rebindState{0};
};
static_assert(sizeof(InputPipeline) == 384);

} // namespace en

#pragma once

#include "input/ActionBuffer.h"

namespace en {

// Run-length compressed deterministic tick input stream for automated tests, netcode, and replays.
// Value type; records all 4 axes in Q14 fixed-point and splits segments on discrete press/release edges.
// Invariant: SamplePlaybackTick reproduces exact held, pressed, released, and 4-axis values.
class alignas(8) InputReplayStream {
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
    }

    constexpr void RecordTick(const TickInput& in) noexcept {
        std::array<i16, kMaxAxes> qAxes{};
        for (usize i = 0U; i < kMaxAxes; ++i) {
            qAxes[i] = static_cast<i16>(std::clamp(in.axis[i], -1.0f, 1.0f) * 16384.0f);
        }

        if (m_count > 0U && in.retriggered == 0U) {
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
    }

    [[nodiscard]] constexpr bool SamplePlaybackTick(
        u64 tickIndex, TickInput& outTick, u16& ioPrevHeld) noexcept {
        if (m_playSegment >= m_count) return false;
        const Segment& seg = m_segments[m_playSegment];
        outTick = TickInput{};
        outTick.tick = tickIndex;
        outTick.held = seg.held;
        outTick.pressed = static_cast<u16>(seg.held & ~ioPrevHeld);
        outTick.released = static_cast<u16>(~seg.held & ioPrevHeld);
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

    [[nodiscard]] constexpr u32 SegmentCount() const noexcept { return m_count; }

private:
    std::array<Segment, kMaxSegments> m_segments{};
    u32 m_count{0};
    u32 m_playSegment{0};
    u16 m_playTickInSeg{0};
};

// Pull-evaluated 640-byte (10 cache lines exact, zero padding) header-only input pipeline.
// Packs all hot per-tick state in Cache Line 0 (offset 0..63), FrameInput in Cache Line 1,
// 512-key bitset in Cache Line 2, 4x16B GamepadState in Cache Line 3, and 384B InputMap in Cache Lines 4..9.
// Invariant: Zero platform/ or OS header dependencies; on idle ticks PollTick() touches only Cache Line 0.
class alignas(64) InputPipeline {
public:
    struct alignas(16) GamepadState {
        u32 buttons{0};
        std::array<i16, 6> axes{};
    };
    static_assert(sizeof(GamepadState) == 16);

    constexpr void Reset() noexcept {
        m_heldTicks.fill(0U);
        m_anyPadButtons = 0U;
        m_actionMask = 0xFFFFU;
        m_tickTap = 0U;
        m_frameTap = 0U;
        m_prevTickHeld = 0U;
        m_prevFrameHeld = 0U;
        m_syntheticHeld = 0U;
        m_syntheticAxesQ14.fill(0);
        m_axisState.fill(0U);
        m_keyWordMask = 0U;
        m_mouseBits = 0U;
        m_rebindState = 0U;
        m_activePadAxisMask = 0U;
        m_frame = FrameInput{};
        m_keyBits.fill(0ULL);
        m_pads.fill(GamepadState{});
    }

    constexpr void ClearPhysicalState() noexcept {
        m_anyPadButtons = 0U;
        m_tickTap = 0U;
        m_frameTap = 0U;
        m_syntheticHeld = 0U;
        m_syntheticAxesQ14.fill(0);
        m_axisState.fill(0U);
        m_keyWordMask = 0U;
        m_mouseBits = 0U;
        m_activePadAxisMask = 0U;
        m_keyBits.fill(0ULL);
        m_pads.fill(GamepadState{});
    }

    constexpr void DisconnectGamepad(u8 padSlot) noexcept {
        if (padSlot >= kMaxInputGamepads) return;
        m_pads[padSlot] = GamepadState{};
        m_activePadAxisMask &= static_cast<u8>(~(1U << padSlot));
        m_anyPadButtons = m_pads[0].buttons | m_pads[1].buttons |
                          m_pads[2].buttons | m_pads[3].buttons;
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

        if (batch.focusCleared != 0U) {
            ClearPhysicalState();
            m_frame.hasFocus = false;
        } else {
            m_frame.hasFocus = true;
        }

        u8 discMask = batch.disconnectedPadMask;
        while (discMask != 0U) {
            const u8 slot = static_cast<u8>(std::countr_zero(discMask));
            discMask &= static_cast<u8>(discMask - 1U);
            DisconnectGamepad(slot);
        }

        const Vec2 newMousePos{batch.mouseX, batch.mouseY};
        const Vec2 newMouseDelta{batch.mouseDx, batch.mouseDy};
        const Vec2 newWheel{batch.wheelX, batch.wheelY};
        m_frame.mousePos = newMousePos;
        m_frame.mouseDelta = newMouseDelta;
        m_frame.mouseWheel = newWheel;
        // Require > 2.0px mouse movement hysteresis before switching UI prompts to Mouse
        if ((newMouseDelta.x * newMouseDelta.x + newMouseDelta.y * newMouseDelta.y) > 4.0f ||
            newWheel.x != 0.0f || newWheel.y != 0.0f) {
            m_frame.activeDevice = InputDevice::Mouse;
        }

        if constexpr (requires { batch.TextView(); }) {
            const std::string_view txt = batch.TextView();
            const usize copyLen = std::min<usize>(txt.size(), FrameInput::kMaxTextBytes);
            for (usize i = 0U; i < copyLen; ++i) {
                m_frame.textInput[i] = txt[i];
            }
            m_frame.textLen = static_cast<u8>(copyLen);
            m_frame.textInput[copyLen] = '\0';
        }

        for (const auto& ev : batch.Events()) {
            switch (static_cast<u8>(ev.Kind())) {
                case 0U: // Key
                    FeedButton(InputSource::Key(ev.Code()), ev.Down());
                    break;
                case 1U: // MouseButton
                    FeedButton(InputSource::Mouse(ev.Code()), ev.Down());
                    break;
                case 2U: // GamepadButton
                    FeedButton(InputSource::Pad(ev.Code(), ev.PadSlot()), ev.Down());
                    break;
                case 3U: // GamepadAxis
                    FeedGamepadAxis(ev.PadSlot(), static_cast<u8>(ev.Code()), ev.value);
                    break;
                default:
                    break;
            }
        }
        return EndFrame();
    }

    [[nodiscard]] constexpr const FrameInput& EndFrame() noexcept {
        ++m_frame.frameIndex;
        const u16 live = EvaluateLiveHeld();
        const u16 frameHeld = static_cast<u16>(live | m_frameTap);
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
        const u16 live = EvaluateLiveHeld();
        const u16 tickHeld = static_cast<u16>(live | m_tickTap);
        const u16 retriggered = static_cast<u16>(m_tickTap & m_prevTickHeld);
        const u16 pressed = static_cast<u16>((tickHeld & ~m_prevTickHeld) | m_tickTap);
        const u16 released = static_cast<u16>((~tickHeld & m_prevTickHeld) | retriggered);
        m_prevTickHeld = tickHeld;
        m_tickTap = 0U;

        TickInput out{};
        out.tick = tickIndex;
        out.held = tickHeld;
        out.pressed = pressed;
        out.released = released;
        out.retriggered = retriggered;

        // Preserve final hold duration in out.heldTicks[idx] on pure release ticks for TapReleased/WasChargedRelease,
        // while resetting m_heldTicks[idx] to 0 for subsequent ticks.
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

    constexpr void BeginRebindCapture(InputId auto targetAction, u8 targetSlot = 0) noexcept {
        m_rebindState = static_cast<u8>(
            0x80U | ((targetSlot & 3U) << 4U) | ToInputIndex(targetAction));
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
                const u8 bit = static_cast<u8>(1U << src.code);
                if (down) m_mouseBits |= bit;
                else m_mouseBits &= static_cast<u8>(~bit);
                break;
            }
            case InputDevice::Gamepad: {
                const u8 slot = src.gamepadIndex < kMaxInputGamepads ? src.gamepadIndex : 0U;
                const u32 bit = (1U << src.code);
                if (down) m_pads[slot].buttons |= bit;
                else m_pads[slot].buttons &= ~bit;
                m_anyPadButtons = m_pads[0].buttons | m_pads[1].buttons |
                                  m_pads[2].buttons | m_pads[3].buttons;
                break;
            }
            default:
                return;
        }
        RecordTapAndSocd(src, down);
    }

    constexpr void FeedGamepadButton(u8 padSlot, u16 button, bool down) noexcept {
        FeedButton(InputSource::Pad(button, padSlot), down);
    }

    constexpr void FeedGamepadAxis(u8 padSlot, u8 axisIdx, i16 value) noexcept {
        if (padSlot >= kMaxInputGamepads || axisIdx >= 6U) return;
        const i16 clamped = (value == -32768) ? i16{-32767} : value;
        m_pads[padSlot].axes[axisIdx] = clamped;

        if (clamped != 0) {
            m_activePadAxisMask |= static_cast<u8>(1U << padSlot);
        } else {
            bool anyNonZero = false;
            for (const i16 ax : m_pads[padSlot].axes) {
                if (ax != 0) {
                    anyNonZero = true;
                    break;
                }
            }
            if (!anyNonZero) {
                m_activePadAxisMask &= static_cast<u8>(~(1U << padSlot));
            }
        }

        // Schmitt-trigger hysteresis for virtual axis/trigger buttons [20..31]
        const u16 negBtn = static_cast<u16>(kPadVirtualButtonBase + static_cast<u16>(axisIdx) * 2U);
        const u16 posBtn = static_cast<u16>(negBtn + 1U);
        const bool wasNegDown = (m_pads[padSlot].buttons & (1U << negBtn)) != 0U;
        const bool wasPosDown = (m_pads[padSlot].buttons & (1U << posBtn)) != 0U;

        const bool nowNegDown = wasNegDown ? (clamped < -kAxisReleaseThreshold) : (clamped <= -kAxisPressThreshold);
        const bool nowPosDown = wasPosDown ? (clamped > kAxisReleaseThreshold) : (clamped >= kAxisPressThreshold);

        if (nowNegDown != wasNegDown) {
            FeedButton(InputSource::Pad(negBtn, padSlot), nowNegDown);
        }
        if (nowPosDown != wasPosDown) {
            FeedButton(InputSource::Pad(posBtn, padSlot), nowPosDown);
        }

        if (std::abs(static_cast<i32>(clamped)) > 8192) {
            m_frame.activeDevice = InputDevice::Gamepad;
        }
    }

    constexpr void FeedMouseMotion(Vec2 pos, Vec2 delta) noexcept {
        m_frame.mousePos = pos;
        m_frame.mouseDelta += delta;
        if ((delta.x * delta.x + delta.y * delta.y) > 4.0f) {
            m_frame.activeDevice = InputDevice::Mouse;
        }
    }

    constexpr void FeedMouseWheel(Vec2 wheel) noexcept {
        m_frame.mouseWheel += wheel;
        if (wheel.x != 0.0f || wheel.y != 0.0f) {
            m_frame.activeDevice = InputDevice::Mouse;
        }
    }

    constexpr void InjectSyntheticAction(InputId auto action, bool down) noexcept {
        const u16 bit = static_cast<u16>(1U << ToInputIndex(action));
        if (down) {
            m_syntheticHeld |= bit;
            const u16 masked = static_cast<u16>(bit & m_actionMask);
            m_tickTap |= masked;
            m_frameTap |= masked;
        } else {
            m_syntheticHeld &= static_cast<u16>(~bit);
        }
    }

    constexpr void InjectSyntheticAxis(InputId auto axis, f32 value) noexcept {
        const f32 clamped = std::clamp(value, -1.0f, 1.0f);
        m_syntheticAxesQ14[ToInputIndex(axis) & 3U] =
            static_cast<i16>(clamped * 16384.0f + (clamped >= 0.0f ? 0.5f : -0.5f));
    }

    [[nodiscard]] constexpr bool IsPhysicallyDown(InputSource src) const noexcept {
        switch (src.device) {
            case InputDevice::Keyboard:
                return (m_keyBits[src.code >> 6U] & (1ULL << (src.code & 63U))) != 0ULL;
            case InputDevice::Mouse:
                return (m_mouseBits & (1U << src.code)) != 0U;
            case InputDevice::Gamepad: {
                const u32 bit = (1U << src.code);
                if (src.gamepadIndex >= kMaxInputGamepads) return (m_anyPadButtons & bit) != 0U;
                return (m_pads[src.gamepadIndex].buttons & bit) != 0U;
            }
            default:
                return false;
        }
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 hdr = static_cast<u64>(m_prevTickHeld) |
                        (static_cast<u64>(m_actionMask) << 16U) |
                        (static_cast<u64>(m_tickTap) << 32U) |
                        (static_cast<u64>(m_frameTap) << 48U);
        u64 h = Mix64(hdr);
        for (const u16 ht : m_heldTicks) {
            h = HashCombine64(h, ht);
        }
        return h;
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
        u16 held = static_cast<u16>(m_syntheticHeld & m_actionMask);
        if ((m_keyWordMask | m_mouseBits | m_anyPadButtons) == 0U) return held;
        u16 enabled = m_actionMask;
        while (enabled != 0U) {
            const u8 a = static_cast<u8>(std::countr_zero(enabled));
            enabled &= static_cast<u16>(enabled - 1U);
            for (const InputSource& b : m_map.ActionBindings(a)) {
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
        if (m_syntheticAxesQ14[axisIdx] != 0) {
            return static_cast<f32>(m_syntheticAxesQ14[axisIdx]) * (1.0f / 16384.0f);
        }
        const u8 st = m_axisState[axisIdx];
        if ((m_keyWordMask | m_mouseBits | m_anyPadButtons | m_activePadAxisMask | st) == 0U) {
            return 0.0f;
        }

        // Consume sub-frame digital axis taps latched since the previous PollTick().
        m_axisState[axisIdx] = static_cast<u8>(st & ~(kAxisTapNeg | kAxisTapPos));

        const AxisBinding& ab = m_map.Axis(axisIdx);
        const bool neg = IsAxisDirDown(ab.negSource, ab.altNegSource) || ((st & kAxisTapNeg) != 0U);
        const bool pos = IsAxisDirDown(ab.posSource, ab.altPosSource) || ((st & kAxisTapPos) != 0U);
        f32 digital = 0.0f;
        if (neg && pos) {
            if (ab.socd == SocdPolicy::LastInputWins) {
                digital = (st & kAxisLastPos) != 0U ? 1.0f : ((st & kAxisLastNeg) != 0U ? -1.0f : 0.0f);
            } else if (ab.socd == SocdPolicy::FirstInputWins) {
                digital = (st & kAxisLastPos) != 0U ? -1.0f : ((st & kAxisLastNeg) != 0U ? 1.0f : 0.0f);
            }
        } else if (neg) {
            digital = -1.0f;
        } else if (pos) {
            digital = 1.0f;
        }

        f32 analog = 0.0f;
        if (m_activePadAxisMask != 0U && ab.gamepadAxis < 6U) {
            i16 rawSample = 0;
            if (ab.padSlot < kMaxInputGamepads) {
                rawSample = m_pads[ab.padSlot].axes[ab.gamepadAxis];
            } else {
                u8 padMask = m_activePadAxisMask;
                while (padMask != 0U) {
                    const u8 p = static_cast<u8>(std::countr_zero(padMask));
                    padMask &= static_cast<u8>(padMask - 1U);
                    const i16 candidate = m_pads[p].axes[ab.gamepadAxis];
                    if (std::abs(static_cast<i32>(candidate)) > std::abs(static_cast<i32>(rawSample))) {
                        rawSample = candidate;
                    }
                }
            }
            const f32 raw = static_cast<f32>(rawSample) * kInvAxisMax;
            const f32 absRaw = std::abs(raw);
            if (absRaw > ab.deadzone && ab.outerDeadzone > ab.deadzone) {
                const f32 sign = raw >= 0.0f ? 1.0f : -1.0f;
                analog = sign * std::clamp((absRaw - ab.deadzone) / (ab.outerDeadzone - ab.deadzone), 0.0f, 1.0f);
            }
        }

        f32 val = (std::abs(digital) >= std::abs(analog)) ? digital : analog;
        if (ab.invert) val = -val;
        return std::clamp(val * ab.sensitivity, -1.0f, 1.0f);
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
            m_frame.activeDevice = src.device;
            m_frame.lastPressed = src;

            u16 enabled = m_actionMask;
            while (enabled != 0U) {
                const u8 a = static_cast<u8>(std::countr_zero(enabled));
                enabled &= static_cast<u16>(enabled - 1U);
                const u16 bit = static_cast<u16>(1U << a);
                bool matchedSrc = false;
                bool alreadyDown = (m_syntheticHeld & bit) != 0U;
                for (const InputSource& b : m_map.ActionBindings(a)) {
                    if (b.Matches(src)) {
                        matchedSrc = true;
                    } else if (IsPhysicallyDown(b)) {
                        alreadyDown = true;
                    }
                }
                if (matchedSrc && !alreadyDown) {
                    m_tickTap |= bit;
                    m_frameTap |= bit;
                }
            }
        }

        for (u8 ax = 0U; ax < kMaxAxes; ++ax) {
            const AxisBinding& ab = m_map.Axis(ax);
            if (ab.negSource.Matches(src) || ab.altNegSource.Matches(src)) {
                if (down) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~kAxisLastPos) | kAxisLastNeg | kAxisTapNeg);
                } else if (!IsAxisDirDown(ab.negSource, ab.altNegSource)) {
                    m_axisState[ax] = static_cast<u8>(
                        (m_axisState[ax] & ~(kAxisLastNeg | kAxisLastPos)) |
                        (IsAxisDirDown(ab.posSource, ab.altPosSource) ? kAxisLastPos : 0U));
                }
            } else if (ab.posSource.Matches(src) || ab.altPosSource.Matches(src)) {
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

    // Cache Line 0 (offset 0..63, 64B exact, 0B padding)
    std::array<u16, kMaxActions> m_heldTicks{};
    u32 m_anyPadButtons{0};
    u16 m_actionMask{0xFFFFU};
    u16 m_tickTap{0};
    u16 m_frameTap{0};
    u16 m_prevTickHeld{0};
    u16 m_prevFrameHeld{0};
    u16 m_syntheticHeld{0};
    std::array<i16, kMaxAxes> m_syntheticAxesQ14{};
    std::array<u8, kMaxAxes> m_axisState{};
    u8 m_keyWordMask{0};
    u8 m_mouseBits{0};
    u8 m_rebindState{0};
    u8 m_activePadAxisMask{0};

    // Cache Line 1 (offset 64..127, 64B exact, 0B padding)
    FrameInput m_frame{};

    // Cache Line 2 (offset 128..191, 64B exact, 0B padding)
    std::array<u64, 8> m_keyBits{};

    // Cache Line 3 (offset 192..255, 64B exact, 0B padding)
    std::array<GamepadState, kMaxInputGamepads> m_pads{};

    // Cache Lines 4..9 (offset 256..639, 384B exact, 0B padding)
    InputMap m_map{};
};
static_assert(sizeof(InputPipeline) == 640);

} // namespace en

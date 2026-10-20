#pragma once

#include "input/ActionBuffer.h"
#include "platform/Platform.h"

namespace en {

// Run-length compressed deterministic tick input stream for automated tests and replays.
class InputReplayStream {
public:
    static constexpr usize kMaxSegments = 256;
    struct Segment {
        u16 held{0};
        i16 axisQ14[2]{0, 0};
        u16 durationTicks{0};
    };

    constexpr void Clear() noexcept {
        m_count = 0;
        m_playSegment = 0;
        m_playTickInSeg = 0;
    }

    constexpr void RecordTick(const TickInput& in) noexcept {
        const i16 ax0 = static_cast<i16>(std::clamp(in.axis[0], -1.0f, 1.0f) * 16384.0f);
        const i16 ax1 = static_cast<i16>(std::clamp(in.axis[1], -1.0f, 1.0f) * 16384.0f);
        if (m_count > 0) {
            Segment& last = m_segments[m_count - 1];
            if (last.held == in.held && last.axisQ14[0] == ax0 &&
                last.axisQ14[1] == ax1 && last.durationTicks < 0xFFFFU) {
                ++last.durationTicks;
                return;
            }
        }
        if (m_count < kMaxSegments) {
            m_segments[m_count++] = Segment{in.held, {ax0, ax1}, 1};
        }
    }

    constexpr void RewindPlayback() noexcept {
        m_playSegment = 0;
        m_playTickInSeg = 0;
    }

    [[nodiscard]] constexpr bool SamplePlaybackTick(u64 tickIndex, TickInput& outTick, u16& ioPrevHeld) noexcept {
        if (m_playSegment >= m_count) return false;
        const Segment& seg = m_segments[m_playSegment];
        outTick = TickInput{};
        outTick.tick = tickIndex;
        outTick.held = seg.held;
        outTick.pressed = static_cast<u16>(seg.held & ~ioPrevHeld);
        outTick.released = static_cast<u16>(~seg.held & ioPrevHeld);
        outTick.axis[0] = static_cast<f32>(seg.axisQ14[0]) / 16384.0f;
        outTick.axis[1] = static_cast<f32>(seg.axisQ14[1]) / 16384.0f;
        ioPrevHeld = seg.held;
        if (++m_playTickInSeg >= seg.durationTicks) {
            m_playTickInSeg = 0;
            ++m_playSegment;
        }
        return true;
    }

    [[nodiscard]] constexpr usize SegmentCount() const noexcept { return m_count; }

private:
    std::array<Segment, kMaxSegments> m_segments{};
    usize m_count{0};
    usize m_playSegment{0};
    u16 m_playTickInSeg{0};
};

// Pull-evaluated input pipeline with dual sub-frame tap latches, SOCD axes, chords, sequences, and rebinding.
class alignas(64) InputPipeline {
public:
    struct alignas(16) GamepadState {
        u32 buttons{0};
        std::array<i16, 6> axes{};
    };
    static_assert(sizeof(GamepadState) == 16);

    constexpr void Reset() noexcept {
        m_heldTicks.fill(0);
        m_anyPadButtons = 0;
        m_actionMask = 0xFFFFU;
        m_tickTap = 0;
        m_frameTap = 0;
        m_prevTickHeld = 0;
        m_prevFrameHeld = 0;
        m_syntheticHeld = 0;
        m_syntheticTap = 0;
        m_keyWordMask = 0;
        m_mouseBits = 0;
        m_rebindCapturing = false;
        m_rebindTargetAction = 0;
        m_rebindTargetSlot = 0;
        m_lastAxisDir.fill(0);
        m_syntheticAxes.fill(0.0f);
        m_frame = FrameInput{};
        m_keyBits.fill(0);
        m_pads.fill(GamepadState{});
    }

    const FrameInput& IngestPlatformFrame(const PlatformFrameEvents& batch) noexcept;
    void BeginFrame() noexcept;
    [[nodiscard]] const FrameInput& EndFrame() noexcept;
    [[nodiscard]] constexpr const FrameInput& GetFrameView() const noexcept { return m_frame; }
    [[nodiscard]] TickInput PollTick(u64 tickIndex) noexcept;

    [[nodiscard]] constexpr InputMap& Map() noexcept { return m_map; }
    [[nodiscard]] constexpr const InputMap& Map() const noexcept { return m_map; }

    constexpr void BeginRebindCapture(InputId auto targetAction, u8 targetSlot = 0) noexcept {
        m_rebindCapturing = true;
        m_rebindTargetAction = ToInputIndex(targetAction);
        m_rebindTargetSlot = targetSlot & 3U;
    }
    [[nodiscard]] constexpr bool IsWaitingForRebind() const noexcept { return m_rebindCapturing; }

    constexpr void SetActionMask(u16 mask) noexcept {
        m_actionMask = mask;
        m_tickTap &= mask;
        m_frameTap &= mask;
    }
    [[nodiscard]] constexpr u16 GetActionMask() const noexcept { return m_actionMask; }

    void FeedButton(InputSource src, bool down) noexcept;
    void InjectSyntheticAction(InputId auto action, bool down) noexcept {
        const u16 bit = static_cast<u16>(1U << ToInputIndex(action));
        if (down) {
            m_syntheticHeld |= bit;
            m_syntheticTap |= static_cast<u16>(bit & m_actionMask);
        } else {
            m_syntheticHeld &= static_cast<u16>(~bit);
        }
    }
    void InjectSyntheticAxis(InputId auto axis, f32 value) noexcept {
        m_syntheticAxes[ToInputIndex(axis) & 3U] = std::clamp(value, -1.0f, 1.0f);
    }

    void ClearPhysicalState() noexcept;
    [[nodiscard]] bool IsPhysicallyDown(InputSource src) const noexcept;
    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    [[nodiscard]] u16 EvaluateLiveHeld() const noexcept;
    [[nodiscard]] f32 EvaluateAxis(u8 axisIdx) const noexcept;
    void RecordTapAndSocd(InputSource src, bool down) noexcept;

    std::array<u16, kMaxActions> m_heldTicks{};
    u32 m_anyPadButtons{0};
    u16 m_actionMask{0xFFFFU};
    u16 m_tickTap{0};
    u16 m_frameTap{0};
    u16 m_prevTickHeld{0};
    u16 m_prevFrameHeld{0};
    u16 m_syntheticHeld{0};
    u16 m_syntheticTap{0};
    u8 m_keyWordMask{0};
    u8 m_mouseBits{0};
    bool m_rebindCapturing{false};
    u8 m_rebindTargetAction{0};
    u8 m_rebindTargetSlot{0};
    std::array<i8, kMaxAxes> m_lastAxisDir{};
    std::array<f32, kMaxAxes> m_syntheticAxes{};

    FrameInput m_frame{};
    std::array<u64, 8> m_keyBits{};
    std::array<GamepadState, kMaxGamepads> m_pads{};
    InputMap m_map{};
};

} // namespace en

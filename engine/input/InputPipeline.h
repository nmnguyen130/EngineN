#pragma once

#include "input/InputTypes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace en::input {

// Stateless-routing, pull-evaluated input pipeline for 120Hz simulation and variable render frames.
// Zero heap allocation, 100% constexpr, L1 cache-line partitioned, and immune to stuck-key bugs.
// Invariant: Sub-frame taps are preserved independently for render frames and simulation ticks.
class alignas(64) InputPipeline {
public:
    static constexpr i16 kGamepadWakeThreshold = 8000;
    static constexpr u16 kAllActionsMask       = 0xFFFFu;

    constexpr InputPipeline() noexcept = default;

    constexpr void BeginFrame() noexcept {
        m_frame.pressed     = 0;
        m_frame.released    = 0;
        m_frame.lastPressed = {};
        m_frame.mouseDelta  = {};
        m_frame.mouseWheel  = {};
    }

    [[nodiscard]] constexpr const FrameInput& EndFrame() noexcept {
        const u16 liveHeld = EvaluateLiveHeld();
        const u16 tapMask  = std::exchange(m_frameTap, 0) & m_actionMask;

        ++m_frame.frameIndex;
        m_frame.pressed  = static_cast<u16>(((liveHeld | tapMask) & ~m_prevFrameHeld) | tapMask);
        m_frame.released = static_cast<u16>(~liveHeld & (m_prevFrameHeld | tapMask));
        m_frame.held     = liveHeld;
        m_prevFrameHeld  = liveHeld;
        return m_frame;
    }

    // Single-pass ingestion of coalesced PlatformFrameEvents from en::platform::Platform::Pump().
    template <typename FrameBatchT>
    constexpr const FrameInput& IngestPlatformFrame(const FrameBatchT& batch) noexcept {
        BeginFrame();

        if (batch.focusCleared) {
            ClearPhysicalState();
        }

        for (u32 m = batch.disconnectedPadMask; m != 0; m &= m - 1) {
            DisconnectGamepad(static_cast<u8>(std::countr_zero(m)));
        }

        if (batch.hasMouseMotion) {
            FeedMouseMotion(Vec2{batch.mouseX, batch.mouseY}, Vec2{batch.mouseDx, batch.mouseDy});
        }

        if (batch.wheelX != 0.0f || batch.wheelY != 0.0f) {
            FeedMouseWheel(Vec2{batch.wheelX, batch.wheelY});
        }

        for (usize i = 0; i < batch.eventCount; ++i) {
            const auto& ev   = batch.events[i];
            const u16   hdr  = ev.packedHeader;
            const u16   code = static_cast<u16>(hdr & 0x0FFFu);
            const u8    slot = static_cast<u8>((hdr >> 12) & 0x03u);

            switch (hdr >> 14) {
                case 0: FeedButton(Source::Key(code), ev.value != 0); break;
                case 1: FeedButton(Source::Mouse(code), ev.value != 0); break;
                case 2: FeedGamepadButton(slot, code, ev.value != 0); break;
                case 3: FeedGamepadAxis(slot, static_cast<u8>(code), ev.value); break;
                default: break;
            }
        }

        return EndFrame();
    }

    [[nodiscard]] constexpr const FrameInput& GetFrameView() const noexcept {
        return m_frame;
    }

    [[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept {
        const u16 liveHeld = EvaluateLiveHeld();
        const u16 tapMask  = std::exchange(m_tickTap, 0) & m_actionMask;
        const u16 tickHeld = liveHeld | tapMask;
        const u16 pressed  = static_cast<u16>((tickHeld & ~m_prevTickHeld) | tapMask);
        const u16 released = static_cast<u16>(~tickHeld & m_prevTickHeld);
        m_prevTickHeld     = tickHeld;

        for (u32 m = released; m != 0; m &= m - 1) {
            m_heldTicks[std::countr_zero(m)] = 0;
        }
        for (u32 m = tickHeld; m != 0; m &= m - 1) {
            const int a = std::countr_zero(m);
            if (((pressed >> a) & 1u) != 0) {
                m_heldTicks[a] = 1;
            } else if (m_heldTicks[a] < 0xFFFFu) {
                ++m_heldTicks[a];
            }
        }

        return TickInput{
            .tick      = tickIndex,
            .held      = tickHeld,
            .pressed   = pressed,
            .released  = released,
            .axis      = EvaluateAxes(),
            .heldTicks = m_heldTicks
        };
    }

    [[nodiscard]] constexpr InputMap& Map() noexcept { return m_map; }
    [[nodiscard]] constexpr const InputMap& Map() const noexcept { return m_map; }

    constexpr void SetActionMask(u16 enabledActionsMask) noexcept {
        m_actionMask = enabledActionsMask;
        m_tickTap   &= enabledActionsMask;
        m_frameTap  &= enabledActionsMask;
    }

    [[nodiscard]] constexpr u16 GetActionMask() const noexcept { return m_actionMask; }

    constexpr void FeedButton(Source src, bool down) noexcept {
        if (!src.IsValid() || IsPhysicallyDown(src) == down) {
            return;
        }

        SetPhysicalBit(src, down);

        if (down) {
            m_frame.activeDevice = src.device;
            m_frame.lastPressed  = src;
            RecordTapAndSocd(src);
        }
    }

    constexpr void FeedMouseMotion(Vec2 pos, Vec2 delta) noexcept {
        m_frame.mousePos    = pos;
        m_frame.mouseDelta += delta;
    }

    constexpr void FeedMouseWheel(Vec2 wheel) noexcept {
        m_frame.mouseWheel += wheel;
    }

    constexpr void FeedGamepadAxis(u8 padSlot, u8 axisIdx, i16 value) noexcept {
        if (padSlot < kMaxGamepads && axisIdx < GamepadState::kMaxAxes) {
            m_pads[padSlot].axes[axisIdx] = value;
            if (value > kGamepadWakeThreshold || value < -kGamepadWakeThreshold) {
                m_frame.activeDevice = Device::Gamepad;
            }
        }
    }

    constexpr void FeedGamepadButton(u8 padSlot, u16 button, bool down) noexcept {
        if (padSlot < kMaxGamepads) {
            FeedButton(Source::Pad(button, padSlot), down);
        }
    }

    constexpr void DisconnectGamepad(u8 padSlot) noexcept {
        if (padSlot < kMaxGamepads) {
            m_pads[padSlot] = {};
            m_anyPadButtons = m_pads[0].buttons | m_pads[1].buttons |
                              m_pads[2].buttons | m_pads[3].buttons;
        }
    }

    constexpr void ClearPhysicalState() noexcept {
        m_keyBits       = {};
        m_pads          = {};
        m_anyPadButtons = 0;
        m_keyWordMask   = 0;
        m_mouseBits     = 0;
        m_tickTap       = 0;
        m_frameTap      = 0;
        m_lastAxisDir   = {};
    }

    [[nodiscard]] constexpr bool IsPhysicallyDown(Source src) const noexcept {
        switch (src.device) {
            case Device::Keyboard:
                return src.code < Source::kMaxScancodes &&
                       (m_keyBits[src.code >> 6] & (1ull << (src.code & 63))) != 0;
            case Device::Mouse:
                return src.code < Source::kMaxMouseButtons &&
                       (m_mouseBits & (1u << src.code)) != 0;
            case Device::Gamepad: {
                if (src.code >= Source::kMaxGamepadButtons) {
                    return false;
                }
                const u32 bit = 1u << src.code;
                if (src.gamepadIndex == Source::kAnyGamepad) {
                    return (m_anyPadButtons & bit) != 0;
                }
                return src.gamepadIndex < kMaxGamepads &&
                       (m_pads[src.gamepadIndex].buttons & bit) != 0;
            }
            default:
                return false;
        }
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = 14695981039346656037ull;
        auto mix = [&](u64 v) constexpr noexcept {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(static_cast<u64>(m_actionMask) |
            (static_cast<u64>(m_tickTap) << 16) |
            (static_cast<u64>(m_prevTickHeld) << 32));
        for (u32 m = m_prevTickHeld; m != 0; m &= m - 1) {
            mix(m_heldTicks[std::countr_zero(m)]);
        }
        for (const i8 dir : m_lastAxisDir) mix(static_cast<u8>(dir));
        return h;
    }

private:
    struct alignas(16) GamepadState {
        static constexpr usize kMaxAxes = 6;
        u32 buttons = 0;
        std::array<i16, kMaxAxes> axes{};
    };

    static_assert(sizeof(GamepadState) == 16);

    constexpr void SetPhysicalBit(Source src, bool down) noexcept {
        switch (src.device) {
            case Device::Keyboard: {
                const usize w    = src.code >> 6;
                const u64   mask = 1ull << (src.code & 63);
                if (down) {
                    m_keyBits[w]  |= mask;
                    m_keyWordMask |= static_cast<u8>(1u << w);
                } else if ((m_keyBits[w] &= ~mask) == 0) {
                    m_keyWordMask &= static_cast<u8>(~(1u << w));
                }
                break;
            }
            case Device::Mouse: {
                const u8 mask = static_cast<u8>(1u << src.code);
                if (down) m_mouseBits |= mask;
                else      m_mouseBits &= static_cast<u8>(~mask);
                break;
            }
            case Device::Gamepad: {
                if (src.gamepadIndex >= kMaxGamepads) return;
                const u32 mask = 1u << src.code;
                if (down) {
                    m_pads[src.gamepadIndex].buttons |= mask;
                    m_anyPadButtons                  |= mask;
                } else {
                    m_pads[src.gamepadIndex].buttons &= ~mask;
                    m_anyPadButtons = m_pads[0].buttons | m_pads[1].buttons |
                                      m_pads[2].buttons | m_pads[3].buttons;
                }
                break;
            }
            default:
                break;
        }
    }

    constexpr void RecordTapAndSocd(Source pressedSrc) noexcept {
        u16 tapped = 0;
        for (u32 m = m_actionMask; m != 0; m &= m - 1) {
            const int a = std::countr_zero(m);
            for (const Source s : m_map.actions[a]) {
                if (s.Matches(pressedSrc)) {
                    tapped |= static_cast<u16>(1u << a);
                    break;
                }
            }
        }
        m_tickTap  |= tapped;
        m_frameTap |= tapped;

        for (usize ax = 0; ax < kMaxAxes; ++ax) {
            if (m_map.axes[ax].negative.Matches(pressedSrc)) m_lastAxisDir[ax] = -1;
            if (m_map.axes[ax].positive.Matches(pressedSrc)) m_lastAxisDir[ax] = 1;
        }
    }

    [[nodiscard]] constexpr u16 EvaluateLiveHeld() const noexcept {
        if ((m_keyWordMask | m_mouseBits | m_anyPadButtons) == 0) {
            return 0;
        }

        u16 mask = 0;
        for (u32 m = m_actionMask; m != 0; m &= m - 1) {
            const int a = std::countr_zero(m);
            for (const Source s : m_map.actions[a]) {
                if (IsPhysicallyDown(s)) {
                    mask |= static_cast<u16>(1u << a);
                    break;
                }
            }
        }
        return mask;
    }

    [[nodiscard]] constexpr std::array<i16, kMaxAxes> EvaluateAxes() const noexcept {
        std::array<i16, kMaxAxes> out{};
        if (m_actionMask == 0) {
            return out;
        }

        const bool anyButtons = (m_keyWordMask | m_mouseBits | m_anyPadButtons) != 0;

        for (usize i = 0; i < kMaxAxes; ++i) {
            const AxisBinding& cfg = m_map.axes[i];

            f32 digital = 0.0f;
            if (anyButtons) {
                const bool neg = IsPhysicallyDown(cfg.negative);
                const bool pos = IsPhysicallyDown(cfg.positive);
                if (neg && pos) {
                    digital = (cfg.socd == SocdPolicy::LastInputWins) ? static_cast<f32>(m_lastAxisDir[i]) : 0.0f;
                } else if (pos) {
                    digital = 1.0f;
                } else if (neg) {
                    digital = -1.0f;
                }
            }

            f32 analog = 0.0f;
            if (cfg.gamepadAxis != GamepadAxis::None) {
                const usize axIdx = static_cast<usize>(cfg.gamepadAxis) - 1;
                for (usize p = 0; p < kMaxGamepads; ++p) {
                    if (cfg.gamepadIndex != Source::kAnyGamepad && cfg.gamepadIndex != p) {
                        continue;
                    }
                    const i16 raw = m_pads[p].axes[axIdx];
                    if (raw == 0) {
                        continue;
                    }
                    const f32 norm = (raw < 0)
                        ? (static_cast<f32>(raw) * (1.0f / 32768.0f))
                        : (static_cast<f32>(raw) * kInvAxisMax);
                    f32 v = ApplyAxialDeadzone(norm, cfg.deadzone);
                    if (cfg.inverted) v = -v;
                    v = std::clamp(v * cfg.scale, -1.0f, 1.0f);
                    if (std::abs(v) > std::abs(analog)) {
                        analog = v;
                    }
                }
            }

            const f32 val = (std::abs(analog) > std::abs(digital)) ? analog : digital;
            if (val != 0.0f) {
                out[i] = static_cast<i16>(val >= 0.0f ? (val * 32767.0f + 0.5f) : (val * 32767.0f - 0.5f));
            }
        }
        return out;
    }

    // Cache Line 0 (0..63B): Hot tick/frame bitmasks and hold counters.
    std::array<u16, kMaxActions> m_heldTicks{};
    u32                          m_anyPadButtons = 0;
    u16                          m_actionMask    = kAllActionsMask;
    u16                          m_tickTap       = 0;
    u16                          m_frameTap      = 0;
    u16                          m_prevTickHeld  = 0;
    u16                          m_prevFrameHeld = 0;
    u8                           m_keyWordMask   = 0;
    u8                           m_mouseBits     = 0;
    std::array<i8, kMaxAxes>     m_lastAxisDir{};

    // Cache Line 1 (64..127B): Render-frame snapshot.
    FrameInput m_frame{};

    // Cache Line 2 (128..191B): 512-bit physical keyboard state bitboard.
    alignas(64) std::array<u64, Source::kMaxScancodes / 64> m_keyBits{};

    // Cache Line 3 (192..255B): 4 x 16B physical gamepad slots.
    alignas(64) std::array<GamepadState, kMaxGamepads> m_pads{};

    // Cache Lines 4..9 (256..591B): Direct-indexed action and axis binding map.
    alignas(64) InputMap m_map{};
};

static_assert(alignof(InputPipeline) == 64);
static_assert(sizeof(InputPipeline) == 640);

} // namespace en::input

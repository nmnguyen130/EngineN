#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <algorithm>
#include <array>
#include <bit>

namespace EngineN {

// Stateless-routing, pull-evaluated input pipeline for 120Hz simulation and variable render frames.
// Zero heap allocation, 100% constexpr, and immune to stuck-key bugs on binding or mask changes.
// Invariant: Sub-frame taps (press+release within one frame) are preserved across consecutive ticks.
class InputPipeline {
public:
    static constexpr usize kMaxGamepads           = 4;
    static constexpr i16   kGamepadWakeThreshold  = 8000;
    static constexpr u16   kAllActionsMask        = 0xFFFFu;

    constexpr InputPipeline() noexcept = default;

    // 1. Call before polling OS events each render frame
    constexpr void BeginFrame() noexcept {
        m_frame.pressed     = 0;
        m_frame.released    = 0;
        m_frame.lastPressed = {};
        m_frame.mouseDelta  = {};
        m_frame.mouseWheel  = {};
    }

    // 2. Call once after OS event polling completes to publish UI/Render FrameInput
    [[nodiscard]] constexpr const FrameInput& EndFrame() noexcept {
        const u16 liveHeld = EvaluateLiveHeld() & m_actionMask;
        // Include m_latchedTap so a sub-frame tap also registers as pressed for UI
        const u16 effectiveHeld = liveHeld | (m_latchedTap & m_actionMask);

        ++m_frame.frameIndex;
        m_frame.pressed  = static_cast<u16>((effectiveHeld & ~m_prevFrameHeld) | (m_latchedTap & m_actionMask));
        m_frame.released = static_cast<u16>(~liveHeld & (m_prevFrameHeld | m_frame.pressed));
        m_frame.held     = liveHeld;
        m_prevFrameHeld  = liveHeld;
        return m_frame;
    }

    [[nodiscard]] constexpr const FrameInput& GetFrameView() const noexcept {
        return m_frame;
    }

    // 3. Call inside `while (const auto step = m_time.PollTick())`
    [[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept {
        const u16 liveHeld = EvaluateLiveHeld() & m_actionMask;
        const u16 tapMask  = m_latchedTap & m_actionMask;
        m_latchedTap       = 0;

        // If a button was tapped and already released before this tick, force it held for 1 tick
        // so simulation sees a clean Press on Tick N and the Release on Tick N+1.
        const u16 tickHeld = liveHeld | tapMask;
        const u16 pressed  = static_cast<u16>((tickHeld & ~m_prevTickHeld) | tapMask);
        const u16 released = static_cast<u16>(~tickHeld & m_prevTickHeld);
        m_prevTickHeld     = tickHeld;

        for (usize a = 0; a < kMaxActions; ++a) {
            const u16 bit = static_cast<u16>(1u << a);
            if ((tickHeld & bit) == 0) {
                m_heldTicks[a] = 0;
            } else if ((pressed & bit) != 0) {
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

    // Configuration & Context Masking
    [[nodiscard]] constexpr InputMap& Map() noexcept { return m_map; }
    [[nodiscard]] constexpr const InputMap& Map() const noexcept { return m_map; }

    constexpr void SetActionMask(u16 enabledActionsMask) noexcept {
        m_actionMask = enabledActionsMask;
    }

    [[nodiscard]] constexpr u16 GetActionMask() const noexcept { return m_actionMask; }

    // Physical Hardware Feeders (Called by SdlInput or Unit Tests)
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
        m_frame.mousePos   = pos;
        m_frame.mouseDelta += delta;
    }

    constexpr void FeedMouseWheel(Vec2 wheel) noexcept {
        m_frame.mouseWheel += wheel;
    }

    constexpr void FeedGamepadAxis(u32 instanceId, u8 axisIdx, i16 value) noexcept {
        if (axisIdx >= GamepadState::kMaxAxes) {
            return;
        }
        const u8 slot = AcquireGamepadSlot(instanceId);
        if (slot < kMaxGamepads) {
            m_pads[slot].axes[axisIdx] = value;
            if (value > kGamepadWakeThreshold || value < -kGamepadWakeThreshold) {
                m_frame.activeDevice = Device::Gamepad;
            }
        }
    }

    constexpr void DisconnectGamepad(u32 instanceId) noexcept {
        for (auto& pad : m_pads) {
            if (pad.connected && pad.instanceId == instanceId) {
                pad = {};
                break;
            }
        }
    }

    constexpr void ClearPhysicalState() noexcept {
        m_keyBits.fill(0);
        m_mouseBits  = 0;
        m_latchedTap = 0;
        m_lastAxisDir.fill(0);
        for (auto& pad : m_pads) {
            pad.buttons = 0;
            pad.axes.fill(0);
        }
    }

    [[nodiscard]] constexpr bool IsPhysicallyDown(Source src) const noexcept {
        switch (src.device) {
            case Device::Keyboard:
                return (m_keyBits[src.code >> 6] & (1ull << (src.code & 63))) != 0;
            case Device::Mouse:
                return (m_mouseBits & (1u << src.code)) != 0;
            case Device::Gamepad: {
                const u32 bit = 1u << src.code;
                if (src.gamepadIndex == Source::kAnyGamepad) {
                    for (const auto& pad : m_pads) {
                        if (pad.connected && (pad.buttons & bit) != 0) return true;
                    }
                    return false;
                }
                return src.gamepadIndex < kMaxGamepads &&
                       m_pads[src.gamepadIndex].connected &&
                       (m_pads[src.gamepadIndex].buttons & bit) != 0;
            }
            default:
                return false;
        }
    }

private:
    struct GamepadState {
        static constexpr usize kMaxAxes = 6;
        u32  instanceId = 0;
        u32  buttons    = 0;
        bool connected  = false;
        std::array<i16, kMaxAxes> axes{};
    };

    [[nodiscard]] constexpr u8 AcquireGamepadSlot(u32 instanceId) noexcept {
        u8 freeSlot = static_cast<u8>(kMaxGamepads);
        for (u8 i = 0; i < kMaxGamepads; ++i) {
            if (m_pads[i].connected) {
                if (m_pads[i].instanceId == instanceId) return i;
            } else if (freeSlot == kMaxGamepads) {
                freeSlot = i;
            }
        }
        if (freeSlot < kMaxGamepads) {
            m_pads[freeSlot] = {.instanceId = instanceId, .connected = true};
        }
        return freeSlot;
    }

    constexpr void SetPhysicalBit(Source& src, bool down) noexcept {
        switch (src.device) {
            case Device::Keyboard: {
                const u64 mask = 1ull << (src.code & 63);
                if (down) m_keyBits[src.code >> 6] |= mask;
                else      m_keyBits[src.code >> 6] &= ~mask;
                break;
            }
            case Device::Mouse: {
                const u8 mask = static_cast<u8>(1u << src.code);
                if (down) m_mouseBits |= mask;
                else      m_mouseBits &= ~mask;
                break;
            }
            case Device::Gamepad: {
                if (src.gamepadIndex >= kMaxGamepads) return;
                const u32 mask = 1u << src.code;
                if (down) m_pads[src.gamepadIndex].buttons |= mask;
                else      m_pads[src.gamepadIndex].buttons &= ~mask;
                break;
            }
            default:
                break;
        }
    }

    constexpr void RecordTapAndSocd(Source pressedSrc) noexcept {
        const auto matches = [&](Source binding) constexpr noexcept {
            return binding.device == pressedSrc.device &&
                   binding.code == pressedSrc.code &&
                   (binding.device != Device::Gamepad ||
                    binding.gamepadIndex == Source::kAnyGamepad ||
                    binding.gamepadIndex == pressedSrc.gamepadIndex);
        };

        for (usize a = 0; a < kMaxActions; ++a) {
            for (const Source s : m_map.actions[a]) {
                if (s.IsValid() && matches(s)) {
                    m_latchedTap |= static_cast<u16>(1u << a);
                }
            }
        }

        for (usize ax = 0; ax < kMaxAxes; ++ax) {
            if (matches(m_map.axes[ax].negative)) m_lastAxisDir[ax] = -1;
            if (matches(m_map.axes[ax].positive)) m_lastAxisDir[ax] = 1;
        }
    }

    [[nodiscard]] constexpr u16 EvaluateLiveHeld() const noexcept {
        u16 mask = 0;
        for (usize a = 0; a < kMaxActions; ++a) {
            for (const Source s : m_map.actions[a]) {
                if (s.IsValid() && IsPhysicallyDown(s)) {
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

        for (usize i = 0; i < kMaxAxes; ++i) {
            const auto& cfg = m_map.axes[i];
            const bool  neg = IsPhysicallyDown(cfg.negative);
            const bool  pos = IsPhysicallyDown(cfg.positive);

            f32 digital = 0.0f;
            if (neg && pos) {
                digital = (cfg.socd == SocdPolicy::LastInputWins) ? static_cast<f32>(m_lastAxisDir[i]) : 0.0f;
            } else if (pos) {
                digital = 1.0f;
            } else if (neg) {
                digital = -1.0f;
            }

            f32 analog = 0.0f;
            if (cfg.gamepadAxis != GamepadAxis::None) {
                const usize axIdx = static_cast<usize>(cfg.gamepadAxis) - 1;
                for (usize p = 0; p < kMaxGamepads; ++p) {
                    if (!m_pads[p].connected ||
                        (cfg.gamepadIndex != Source::kAnyGamepad && cfg.gamepadIndex != p)) {
                        continue;
                    }
                    const i16 raw  = m_pads[p].axes[axIdx];
                    const f32 norm = (raw < 0) ? (static_cast<f32>(raw) / 32768.0f) : (static_cast<f32>(raw) / 32767.0f);
                    f32 v = ApplyAxialDeadzone(norm, cfg.deadzone);
                    if (cfg.inverted) v = -v;
                    v = std::clamp(v * cfg.scale, -1.0f, 1.0f);
                    if ((v < 0.0f ? -v : v) > (analog < 0.0f ? -analog : analog)) {
                        analog = v;
                    }
                }
            }

            const f32 val = ((analog < 0.0f ? -analog : analog) > (digital < 0.0f ? -digital : digital))
                          ? analog
                          : digital;
            out[i] = static_cast<i16>(val >= 0.0f ? (val * 32767.0f + 0.5f) : (val * 32767.0f - 0.5f));
        }
        return out;
    }

    public:
    constexpr void FeedGamepadButton(u32 instanceId, u16 button, bool down) noexcept {
        const u8 slot = AcquireGamepadSlot(instanceId);
        if (slot < kMaxGamepads) {
            FeedButton(Source::Pad(button, slot), down);
        }
    }

private:
    alignas(64) std::array<u64, Source::kMaxScancodes / 64> m_keyBits{};
    u8 m_mouseBits = 0;
    std::array<i8, kMaxAxes> m_lastAxisDir{};
    std::array<GamepadState, kMaxGamepads> m_pads{};

    InputMap m_map{};
    u16      m_actionMask    = kAllActionsMask;
    u16      m_latchedTap    = 0;
    u16      m_prevTickHeld  = 0;
    u16      m_prevFrameHeld = 0;
    std::array<u16, kMaxActions> m_heldTicks{};

    FrameInput m_frame{};
};

} // namespace EngineN
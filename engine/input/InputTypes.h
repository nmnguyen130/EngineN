#pragma once

#include "core/Math.h"

#include <cstring>
#include <initializer_list>

namespace en {

inline constexpr usize kMaxActions = 16;
inline constexpr usize kMaxAxes = 4;
inline constexpr usize kMaxBindingsPerSlot = 4;
inline constexpr f32 kInvAxisMax = 1.0f / 32767.0f;

// Virtual gamepad button indices [20..31] driven by Schmitt-trigger hysteresis on axes [0..5].
inline constexpr u16 kPadVirtualButtonBase = 20U;
inline constexpr i16 kAxisPressThreshold = 18000;
inline constexpr i16 kAxisReleaseThreshold = 12000;

namespace detail {

[[nodiscard]] constexpr f32 ConstexprSqrt(f32 x) noexcept {
    if (x <= 0.0f) return 0.0f;
    if !consteval {
        return std::sqrt(x);
    }
    u32 bits = std::bit_cast<u32>(x);
    bits = (bits >> 1U) + 0x1FC00000U;
    f32 g = std::bit_cast<f32>(bits);
    for (int i = 0; i < 4; ++i) g = 0.5f * (g + x / g);
    return g;
}

} // namespace detail

enum class InputDevice : u8 {
    None = 0,
    Keyboard = 1,
    Mouse = 2,
    Gamepad = 3,
};

// Physical input descriptor for single-controller architecture.
// Supports keyboard scancodes [1..511], mouse buttons [1..7], gamepad buttons [0..19], and virtual axis buttons [20..31].
// Invariant: IsValid() guarantees code is within the bitset bounds of its target device.
struct alignas(2) InputSource {
    u16 packed{0};

    [[nodiscard]] static constexpr InputSource Make(InputDevice dev, u16 code) noexcept {
        return {static_cast<u16>((code & 0x3FFFU) | (static_cast<u16>(dev) << 14U))};
    }

    [[nodiscard]] static constexpr InputSource Key(u16 scancode) noexcept {
        return Make(InputDevice::Keyboard, scancode);
    }

    [[nodiscard]] static constexpr InputSource Mouse(u16 button) noexcept {
        return Make(InputDevice::Mouse, button);
    }

    [[nodiscard]] static constexpr InputSource Pad(u16 button) noexcept {
        return Make(InputDevice::Gamepad, button);
    }

    [[nodiscard]] static constexpr InputSource PadAxisButton(u8 axisIdx, bool positive = true) noexcept {
        if (axisIdx >= 6U) return {};
        const u16 virtualCode = static_cast<u16>(
            kPadVirtualButtonBase + static_cast<u16>(axisIdx) * 2U + (positive ? 1U : 0U));
        return Make(InputDevice::Gamepad, virtualCode);
    }

    [[nodiscard]] static constexpr InputSource PadTrigger(bool rightTrigger) noexcept {
        return PadAxisButton(rightTrigger ? 5U : 4U, true);
    }

    [[nodiscard]] constexpr InputDevice Device() const noexcept {
        return static_cast<InputDevice>((packed >> 14U) & 0x3U);
    }

    [[nodiscard]] constexpr u16 Code() const noexcept { return packed & 0x3FFFU; }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        const u16 c = Code();
        switch (Device()) {
            case InputDevice::Keyboard: return c > 0U && c < 512U;
            case InputDevice::Mouse:    return c > 0U && c < 8U;
            case InputDevice::Gamepad:  return c < 32U;
            default:                    return false;
        }
    }

    [[nodiscard]] constexpr bool operator==(const InputSource&) const noexcept = default;
};
static_assert(sizeof(InputSource) == 2);

// Composite 1D axis binding combining primary/alternate digital pairs (Last-Input-Wins) with an analog axis.
// Invariant: after Sanitize(), 0 <= deadzone <= 0.90f and invalid sources are cleared.
struct alignas(4) AxisBinding {
    InputSource negSource{};
    InputSource posSource{};
    InputSource altNegSource{};
    InputSource altPosSource{};
    f32 deadzone{0.15f};
    u8 gamepadAxis{0xFFU};
    bool invert{false};
    bool neutralSocd{false};
    u8 reserved{0};

    constexpr void Sanitize() noexcept {
        if (!negSource.IsValid()) negSource = {};
        if (!posSource.IsValid()) posSource = {};
        if (!altNegSource.IsValid()) altNegSource = {};
        if (!altPosSource.IsValid()) altPosSource = {};
        deadzone = std::clamp(deadzone, 0.0f, 0.90f);
    }
};
static_assert(sizeof(AxisBinding) == 16);

// Action and axis binding table.
// Value type; sanitizes all bindings on insertion and deserialization.
// Invariant: every non-empty slot satisfies IsValid().
class alignas(16) InputMap {
public:
    static constexpr usize kSerializedSize = 192;

    constexpr void Clear() noexcept {
        for (auto& slots : m_actions) slots.fill(InputSource{});
        m_axes.fill(AxisBinding{});
    }

    constexpr InputMap& BindSlot(u8 action, u8 slot, InputSource src) noexcept {
        if (slot < kMaxBindingsPerSlot) {
            m_actions[action & 0x0FU][slot] = src.IsValid() ? src : InputSource{};
        }
        return *this;
    }

    constexpr InputMap& Bind(u8 action, std::initializer_list<InputSource> sources) noexcept {
        auto& slots = m_actions[action & 0x0FU];
        slots.fill(InputSource{});
        u8 slot = 0U;
        for (const InputSource s : sources) {
            if (slot >= kMaxBindingsPerSlot) break;
            if (s.IsValid()) slots[slot++] = s;
        }
        return *this;
    }

    constexpr InputMap& UnbindAction(u8 action) noexcept {
        m_actions[action & 0x0FU].fill(InputSource{});
        return *this;
    }

    constexpr InputMap& BindAxis(u8 axis, const AxisBinding& binding) noexcept {
        AxisBinding sanitized = binding;
        sanitized.Sanitize();
        m_axes[axis & 3U] = sanitized;
        return *this;
    }

    [[nodiscard]] constexpr u16 FindConflictingActions(
        InputSource src, u16 checkMask = 0xFFFFU) const noexcept {
        if (!src.IsValid()) return 0U;
        u16 conflicts = 0U;
        for (u8 a = 0U; a < kMaxActions; ++a) {
            if ((checkMask & (1U << a)) == 0U) continue;
            for (const InputSource b : m_actions[a]) {
                if (b == src) {
                    conflicts |= static_cast<u16>(1U << a);
                    break;
                }
            }
        }
        return conflicts;
    }

    [[nodiscard]] constexpr std::span<const InputSource, kMaxBindingsPerSlot> ActionBindings(
        u8 action) const noexcept {
        return m_actions[action & 0x0FU];
    }

    [[nodiscard]] constexpr const AxisBinding& Axis(u8 axis) const noexcept {
        return m_axes[axis & 3U];
    }

    [[nodiscard]] usize SerializeBindings(std::span<u8> outBytes) const noexcept {
        if (outBytes.size() < kSerializedSize) return 0U;
        std::memcpy(outBytes.data(), m_actions.data(), sizeof(m_actions));
        std::memcpy(outBytes.data() + sizeof(m_actions), m_axes.data(), sizeof(m_axes));
        return kSerializedSize;
    }

    [[nodiscard]] bool DeserializeBindings(std::span<const u8> inBytes) noexcept {
        if (inBytes.size() < kSerializedSize) return false;
        std::memcpy(m_actions.data(), inBytes.data(), sizeof(m_actions));
        std::memcpy(m_axes.data(), inBytes.data() + sizeof(m_actions), sizeof(m_axes));

        for (auto& slots : m_actions) {
            for (InputSource& s : slots) {
                if (!s.IsValid()) s = InputSource{};
            }
        }
        for (AxisBinding& ab : m_axes) ab.Sanitize();
        return true;
    }

private:
    std::array<std::array<InputSource, kMaxBindingsPerSlot>, kMaxActions> m_actions{};
    std::array<AxisBinding, kMaxAxes> m_axes{};
};
static_assert(sizeof(InputMap) == 192);

// 120 Hz simulation tick input snapshot.
// heldTicks[a] stores active hold duration while Held(a) is true, and retains the final hold duration on release ticks.
struct alignas(16) TickInput {
    u64 tick{0};
    u16 held{0};
    u16 pressed{0};
    u16 released{0};
    u16 retriggered{0};
    std::array<f32, kMaxAxes> axis{};
    std::array<u16, kMaxActions> heldTicks{};

    [[nodiscard]] constexpr bool Held(u8 a) const noexcept {
        return (held & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr bool Pressed(u8 a) const noexcept {
        return (pressed & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr bool Released(u8 a) const noexcept {
        return (released & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr bool Retriggered(u8 a) const noexcept {
        return (retriggered & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr u16 HeldTicks(u8 a) const noexcept {
        return heldTicks[a & 0x0FU];
    }

    [[nodiscard]] constexpr f32 Axis(u8 ax) const noexcept {
        return axis[ax & 3U];
    }

    [[nodiscard]] constexpr bool HeldOneShot(u8 a, u16 holdThresholdTicks = 24) const noexcept {
        const u8 idx = a & 0x0FU;
        const u16 target = holdThresholdTicks > 0U ? holdThresholdTicks : 1U;
        return ((held & (1U << idx)) != 0U) && (heldTicks[idx] == target);
    }

    [[nodiscard]] constexpr bool TapReleased(u8 a, u16 maxTapTicks = 12) const noexcept {
        const u8 idx = a & 0x0FU;
        const u16 bit = static_cast<u16>(1U << idx);
        return ((released & bit) != 0U) &&
               ((held & bit) == 0U) &&
               (heldTicks[idx] > 0U) &&
               (heldTicks[idx] <= maxTapTicks);
    }

    [[nodiscard]] constexpr f32 ChargeRatio(u8 a, u16 fullChargeTicks = 30) const noexcept {
        if (fullChargeTicks == 0U) return 1.0f;
        const u8 idx = a & 0x0FU;
        if ((held & (1U << idx)) == 0U) return 0.0f;
        return std::clamp(
            static_cast<f32>(heldTicks[idx]) / static_cast<f32>(fullChargeTicks),
            0.0f,
            1.0f);
    }

    [[nodiscard]] constexpr bool WasChargedRelease(u8 a, u16 minChargeTicks = 20) const noexcept {
        const u8 idx = a & 0x0FU;
        const u16 bit = static_cast<u16>(1U << idx);
        return ((released & bit) != 0U) && ((held & bit) == 0U) && (heldTicks[idx] >= minChargeTicks);
    }

    [[nodiscard]] static constexpr IVec2 SnapOctagonal(Vec2 raw, f32 deadzone = 0.25f) noexcept {
        if (raw.LengthSq() <= deadzone * deadzone) return {0, 0};
        constexpr f32 kTan22_5 = 0.41421356f;
        const f32 ax = std::abs(raw.x);
        const f32 ay = std::abs(raw.y);
        const i32 ix = (ax > ay * kTan22_5) ? (raw.x >= 0.0f ? 1 : -1) : 0;
        const i32 iy = (ay > ax * kTan22_5) ? (raw.y >= 0.0f ? 1 : -1) : 0;
        return {ix, iy};
    }

    [[nodiscard]] constexpr Vec2 Stick(u8 axX = 0, u8 axY = 1) const noexcept {
        const Vec2 raw{Axis(axX), Axis(axY)};
        const f32 lenSq = raw.LengthSq();
        return (lenSq <= 1.0f) ? raw : (raw * (1.0f / detail::ConstexprSqrt(lenSq)));
    }

    [[nodiscard]] constexpr IVec2 StickOctagonal(
        u8 axX = 0, u8 axY = 1, f32 deadzone = 0.25f) const noexcept {
        return SnapOctagonal({Axis(axX), Axis(axY)}, deadzone);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 masks = static_cast<u64>(held) |
                          (static_cast<u64>(pressed) << 16U) |
                          (static_cast<u64>(released) << 32U) |
                          (static_cast<u64>(retriggered) << 48U);
        u64 h = HashCombine64(Mix64(tick), masks);
        for (const f32 ax : axis) h = HashCombine64(h, std::bit_cast<u32>(ax));
        return h;
    }
};
static_assert(sizeof(TickInput) == 64);

// Render-rate UI and camera input snapshot.
struct alignas(16) FrameInput {
    static constexpr usize kMaxTextBytes = 20;

    u64 frameIndex{0};
    Vec2 mousePos{};
    Vec2 mouseDelta{};
    Vec2 mouseWheel{};
    u16 held{0};
    u16 pressed{0};
    u16 released{0};
    InputSource lastPressed{};
    InputDevice activeDevice{InputDevice::Keyboard};
    bool hasFocus{true};
    u8 textLen{0};
    std::array<char, kMaxTextBytes + 1U> textInput{};

    [[nodiscard]] constexpr bool Held(u8 a) const noexcept {
        return (held & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr bool Pressed(u8 a) const noexcept {
        return (pressed & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr bool Released(u8 a) const noexcept {
        return (released & (1U << (a & 0x0FU))) != 0U;
    }

    [[nodiscard]] constexpr std::string_view TextView() const noexcept {
        return {textInput.data(), textLen};
    }
};
static_assert(sizeof(FrameInput) == 64);

} // namespace en

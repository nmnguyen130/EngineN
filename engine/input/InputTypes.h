#pragma once

#include "core/Math.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>

namespace en {

inline constexpr usize kMaxInputGamepads = 4;
inline constexpr u8 kAnyGamepad = 0xFFU;
inline constexpr usize kMaxActions = 16;
inline constexpr usize kMaxAxes = 4;
inline constexpr usize kMaxBindingsPerSlot = 4;
inline constexpr f32 kInvAxisMax = 1.0f / 32767.0f;

// Virtual gamepad button indices [20..31] driven by Schmitt-trigger hysteresis on axes [0..5].
inline constexpr u16 kPadVirtualButtonBase = 20U;
inline constexpr i16 kAxisPressThreshold = 18000;   // ~0.55f actuation point
inline constexpr i16 kAxisReleaseThreshold = 12000; // ~0.37f release point (hysteresis band)

template <typename T>
concept InputId = std::is_unsigned_v<T> || (std::is_enum_v<T> && sizeof(T) <= 4);

[[nodiscard]] constexpr u8 ToInputIndex(InputId auto id) noexcept {
    return static_cast<u8>(id) & 0x0FU;
}

template <InputId... Ids>
[[nodiscard]] constexpr u16 MakeActionMask(Ids... ids) noexcept {
    return static_cast<u16>(((1U << ToInputIndex(ids)) | ... | 0U));
}

namespace detail {

[[nodiscard]] constexpr f32 ConstexprSqrt(f32 x) noexcept {
    if (x <= 0.0f) return 0.0f;
    if !consteval {
        return std::sqrt(x);
    }
    u32 bits = std::bit_cast<u32>(x);
    bits = (bits >> 1U) + 0x1FC00000U;
    f32 g = std::bit_cast<f32>(bits);
    for (int i = 0; i < 4; ++i) {
        g = 0.5f * (g + x / g);
    }
    return g;
}

} // namespace detail

enum class InputDevice : u8 {
    None = 0,
    Keyboard = 1,
    Mouse = 2,
    Gamepad = 3,
};

enum class SocdPolicy : u8 {
    Neutral = 0,
    LastInputWins = 1,
    FirstInputWins = 2,
};

// Packed 4-byte physical input descriptor with strict per-device code range validation.
// Value type; supports keyboard scancodes [1..511], mouse buttons [1..7], physical gamepad buttons [0..19],
// and Schmitt-trigger virtual axis/trigger buttons [20..31].
// Invariant: IsValid() guarantees code is within the bitset bounds of its target device.
struct alignas(4) InputSource {
    InputDevice device{InputDevice::None};
    u8 gamepadIndex{kAnyGamepad};
    u16 code{0};

    [[nodiscard]] static constexpr InputSource Key(u16 scancode) noexcept {
        return {InputDevice::Keyboard, kAnyGamepad, scancode};
    }

    [[nodiscard]] static constexpr InputSource Mouse(u16 button) noexcept {
        return {InputDevice::Mouse, kAnyGamepad, button};
    }

    [[nodiscard]] static constexpr InputSource Pad(u16 button, u8 padSlot = kAnyGamepad) noexcept {
        return {InputDevice::Gamepad, padSlot, button};
    }

    // Binds an analog gamepad axis direction (e.g. L2 = axis 4, R2 = axis 5, or stick flicks) as a digital button.
    [[nodiscard]] static constexpr InputSource PadAxisButton(
        u8 axisIdx, bool positive = true, u8 padSlot = kAnyGamepad) noexcept {
        if (axisIdx >= 6U) return {};
        const u16 virtualCode = static_cast<u16>(
            kPadVirtualButtonBase + static_cast<u16>(axisIdx) * 2U + (positive ? 1U : 0U));
        return {InputDevice::Gamepad, padSlot, virtualCode};
    }

    [[nodiscard]] static constexpr InputSource PadTrigger(
        bool rightTrigger, u8 padSlot = kAnyGamepad) noexcept {
        return PadAxisButton(rightTrigger ? 5U : 4U, true, padSlot);
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        switch (device) {
            case InputDevice::Keyboard: return code > 0U && code < 512U;
            case InputDevice::Mouse:    return code > 0U && code < 8U;
            case InputDevice::Gamepad:
                return code < 32U && (gamepadIndex < kMaxInputGamepads || gamepadIndex == kAnyGamepad);
            default: return false;
        }
    }

    [[nodiscard]] constexpr bool Matches(InputSource physical) const noexcept {
        return device != InputDevice::None &&
               device == physical.device &&
               code == physical.code &&
               (gamepadIndex == kAnyGamepad || gamepadIndex == physical.gamepadIndex);
    }

    [[nodiscard]] constexpr bool operator==(const InputSource&) const noexcept = default;
};
static_assert(sizeof(InputSource) == 4);

// Cache-aligned 32-byte (2 per 64B cache line, zero straddling, zero padding) composite 1D axis binding.
// Combines primary and alternate digital SOCD pairs (e.g. WASD + Arrows/D-Pad) with an inner/outer deadzoned analog axis.
// Invariant: 0 <= deadzone < outerDeadzone <= 1.0f.
struct alignas(32) AxisBinding {
    InputSource negSource{};
    InputSource posSource{};
    InputSource altNegSource{};
    InputSource altPosSource{};
    f32 deadzone{0.15f};
    f32 outerDeadzone{0.95f};
    f32 sensitivity{1.0f};
    u8 gamepadAxis{0xFFU};
    u8 padSlot{kAnyGamepad};
    SocdPolicy socd{SocdPolicy::LastInputWins};
    bool invert{false};
};
static_assert(sizeof(AxisBinding) == 32);

// Direct-indexed 384-byte (6 cache lines exact, zero padding) action and axis binding table.
// Value type; sanitizes all bindings on insertion and deserialization so stored sources are always valid or None.
// Invariant: sizeof(InputMap) == 384 (256B actions + 128B axes) and every non-empty slot satisfies IsValid().
class alignas(64) InputMap {
public:
    static constexpr usize kSerializedSize = 384;

    constexpr void Clear() noexcept {
        for (auto& slots : m_actions) slots.fill(InputSource{});
        m_axes.fill(AxisBinding{});
    }

    constexpr InputMap& BindSlot(InputId auto action, u8 slot, InputSource src) noexcept {
        const u8 idx = ToInputIndex(action);
        if (slot < kMaxBindingsPerSlot) {
            m_actions[idx][slot] = src.IsValid() ? src : InputSource{};
        }
        return *this;
    }

    constexpr InputMap& Bind(InputId auto action, std::initializer_list<InputSource> sources) noexcept {
        const u8 idx = ToInputIndex(action);
        m_actions[idx].fill(InputSource{});
        u8 slot = 0U;
        for (const InputSource& s : sources) {
            if (slot >= kMaxBindingsPerSlot) break;
            if (s.IsValid()) m_actions[idx][slot++] = s;
        }
        return *this;
    }

    constexpr InputMap& UnbindAction(InputId auto action) noexcept {
        m_actions[ToInputIndex(action)].fill(InputSource{});
        return *this;
    }

    constexpr InputMap& BindAxis(InputId auto axis, const AxisBinding& binding) noexcept {
        AxisBinding sanitized = binding;
        SanitizeAxisBinding(sanitized);
        m_axes[ToInputIndex(axis) & 3U] = sanitized;
        return *this;
    }

    // Returns a 16-bit mask of all actions in checkMask currently bound to physical source src.
    [[nodiscard]] constexpr u16 FindConflictingActions(
        InputSource src, u16 checkMask = 0xFFFFU) const noexcept {
        if (!src.IsValid()) return 0U;
        u16 conflicts = 0U;
        for (u8 a = 0U; a < kMaxActions; ++a) {
            if ((checkMask & (1U << a)) == 0U) continue;
            for (const InputSource& b : m_actions[a]) {
                if (b.Matches(src)) {
                    conflicts |= static_cast<u16>(1U << a);
                    break;
                }
            }
        }
        return conflicts;
    }

    [[nodiscard]] constexpr std::span<const InputSource, kMaxBindingsPerSlot> ActionBindings(
        u8 actionIdx) const noexcept {
        return m_actions[actionIdx & 0x0FU];
    }

    [[nodiscard]] constexpr const AxisBinding& Axis(u8 axisIdx) const noexcept {
        return m_axes[axisIdx & 3U];
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
        for (AxisBinding& ab : m_axes) {
            SanitizeAxisBinding(ab);
        }
        return true;
    }

private:
    static constexpr void SanitizeAxisBinding(AxisBinding& ab) noexcept {
        if (!ab.negSource.IsValid()) ab.negSource = InputSource{};
        if (!ab.posSource.IsValid()) ab.posSource = InputSource{};
        if (!ab.altNegSource.IsValid()) ab.altNegSource = InputSource{};
        if (!ab.altPosSource.IsValid()) ab.altPosSource = InputSource{};
        ab.deadzone = std::clamp(ab.deadzone, 0.0f, 0.90f);
        ab.outerDeadzone = std::clamp(ab.outerDeadzone, ab.deadzone + 0.05f, 1.0f);
    }

    std::array<std::array<InputSource, kMaxBindingsPerSlot>, kMaxActions> m_actions{};
    std::array<AxisBinding, kMaxAxes> m_axes{};
};
static_assert(sizeof(InputMap) == 384);

// Authoritative 64-byte (1 L1 cache line exact, zero padding) 120 Hz simulation tick input snapshot.
// Value type; heldTicks[a] stores active hold duration while Held(a) is true, and retains the final
// hold duration on the release tick when Released(a) && !Held(a) holds (enabling Tap, HoldOneShot, Pulse, and Charge).
struct alignas(64) TickInput {
    u64 tick{0};
    u16 held{0};
    u16 pressed{0};
    u16 released{0};
    u16 retriggered{0};
    std::array<f32, kMaxAxes> axis{};
    std::array<u16, kMaxActions> heldTicks{};

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return (held & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return (pressed & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return (released & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool Retriggered(InputId auto a) const noexcept {
        return (retriggered & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr u16 HeldTicks(InputId auto a) const noexcept {
        return heldTicks[ToInputIndex(a)];
    }

    [[nodiscard]] constexpr f32 Axis(InputId auto ax) const noexcept {
        return axis[ToInputIndex(ax) & 3U];
    }

    // Fires once on the exact tick a held button crosses holdThresholdTicks (Unreal Enhanced Input Hold trigger).
    [[nodiscard]] constexpr bool HeldOneShot(
        InputId auto a, u16 holdThresholdTicks = 24) const noexcept {
        const u8 idx = ToInputIndex(a);
        const u16 target = holdThresholdTicks > 0U ? holdThresholdTicks : 1U;
        return ((held & (1U << idx)) != 0U) && (heldTicks[idx] == target);
    }

    // Fires on release iff the button was held for at most maxTapTicks (shared Dodge-on-Tap / Sprint-on-Hold trigger).
    [[nodiscard]] constexpr bool TapReleased(
        InputId auto a, u16 maxTapTicks = 12) const noexcept {
        const u8 idx = ToInputIndex(a);
        const u16 bit = static_cast<u16>(1U << idx);
        return ((released & bit) != 0U) &&
               ((held & bit) == 0U) &&
               (heldTicks[idx] > 0U) &&
               (heldTicks[idx] <= maxTapTicks);
    }

    // Fires on initial press, waits initialDelayTicks, then pulses every repeatIntervalTicks while held.
    [[nodiscard]] constexpr bool Pulsed(
        InputId auto a, u16 initialDelayTicks = 24, u16 repeatIntervalTicks = 6) const noexcept {
        const u8 idx = ToInputIndex(a);
        if ((held & (1U << idx)) == 0U) return false;
        const u16 ht = heldTicks[idx];
        if (ht == 1U) return true;
        const u16 delay = initialDelayTicks > 1U ? initialDelayTicks : 1U;
        if (ht <= delay) return false;
        const u16 step = repeatIntervalTicks > 0U ? repeatIntervalTicks : 1U;
        return ((ht - delay) % step) == 0U;
    }

    [[nodiscard]] constexpr f32 ChargeRatio(
        InputId auto a, u16 fullChargeTicks = 30) const noexcept {
        if (fullChargeTicks == 0U) return 1.0f;
        const u8 idx = ToInputIndex(a);
        if ((held & (1U << idx)) == 0U) return 0.0f;
        return std::clamp(
            static_cast<f32>(heldTicks[idx]) / static_cast<f32>(fullChargeTicks),
            0.0f,
            1.0f);
    }

    [[nodiscard]] constexpr bool WasChargedRelease(
        InputId auto a, u16 minChargeTicks = 20) const noexcept {
        const u8 idx = ToInputIndex(a);
        const u16 bit = static_cast<u16>(1U << idx);
        return ((released & bit) != 0U) && ((held & bit) == 0U) && (heldTicks[idx] >= minChargeTicks);
    }

    // Scaled radial deadzone with rational response curve (responseCurve == 1.0f is linear, > 1.0f adds precision).
    [[nodiscard]] static constexpr Vec2 ApplyRadialDeadzone(
        Vec2 raw,
        f32 innerDz = 0.15f,
        f32 outerDz = 0.95f,
        f32 responseCurve = 1.0f) noexcept {
        const f32 lenSq = raw.LengthSq();
        if (outerDz <= innerDz || lenSq <= innerDz * innerDz) return {0.0f, 0.0f};
        const f32 len = detail::ConstexprSqrt(lenSq);
        const f32 t = std::clamp((len - innerDz) / (outerDz - innerDz), 0.0f, 1.0f);
        const f32 k = std::max(0.1f, responseCurve);
        const f32 curved = (k == 1.0f) ? t : (t / (k - (k - 1.0f) * t));
        return raw * (curved / len);
    }

    [[nodiscard]] static constexpr Vec2 ApplyAxialDeadzone(
        Vec2 raw, f32 dzX = 0.15f, f32 dzY = 0.15f) noexcept {
        const auto remapAxis = [](f32 v, f32 dz) constexpr noexcept -> f32 {
            const f32 av = std::abs(v);
            if (av <= dz || dz >= 1.0f) return 0.0f;
            const f32 n = std::clamp((av - dz) / (1.0f - dz), 0.0f, 1.0f);
            return (v < 0.0f) ? -n : n;
        };
        return {remapAxis(raw.x, dzX), remapAxis(raw.y, dzY)};
    }

    // Quantizes an analog 2D vector into exact 8-way octagonal directions {-1..1, -1..1} without atan2.
    [[nodiscard]] static constexpr IVec2 SnapOctagonal(Vec2 raw, f32 deadzone = 0.25f) noexcept {
        if (raw.LengthSq() <= deadzone * deadzone) return {0, 0};
        constexpr f32 kTan22_5 = 0.41421356f;
        const f32 ax = std::abs(raw.x);
        const f32 ay = std::abs(raw.y);
        const i32 ix = (ax > ay * kTan22_5) ? (raw.x >= 0.0f ? 1 : -1) : 0;
        const i32 iy = (ay > ax * kTan22_5) ? (raw.y >= 0.0f ? 1 : -1) : 0;
        return {ix, iy};
    }

    // Converts an octagonal direction {-1..1, -1..1} into standard fighting-game Numpad notation [1..9] (5 = Neutral).
    [[nodiscard]] static constexpr u8 OctagonalToNumpad(IVec2 oct) noexcept {
        const i32 cx = std::clamp(oct.x, -1, 1);
        const i32 cy = std::clamp(oct.y, -1, 1);
        return static_cast<u8>((cy + 1) * 3 + (cx + 2));
    }

    // Returns a unit-circle clamped 2D movement vector from two axes without double-deadzoning by default.
    [[nodiscard]] constexpr Vec2 Stick(
        InputId auto axX,
        InputId auto axY,
        f32 extraRadialDeadzone = 0.0f,
        f32 outerDeadzone = 1.0f,
        f32 responseCurve = 1.0f) const noexcept {
        const Vec2 raw{Axis(axX), Axis(axY)};
        if (extraRadialDeadzone > 0.0f || outerDeadzone < 1.0f || responseCurve != 1.0f) {
            return ApplyRadialDeadzone(raw, extraRadialDeadzone, outerDeadzone, responseCurve);
        }
        const f32 lenSq = raw.LengthSq();
        if (lenSq <= 1.0f) return raw;
        return raw * (1.0f / detail::ConstexprSqrt(lenSq));
    }

    [[nodiscard]] constexpr IVec2 StickOctagonal(
        InputId auto axX, InputId auto axY, f32 deadzone = 0.25f) const noexcept {
        return SnapOctagonal({Axis(axX), Axis(axY)}, deadzone);
    }

    [[nodiscard]] constexpr u8 StickNumpad(
        InputId auto axX, InputId auto axY, f32 deadzone = 0.25f) const noexcept {
        return OctagonalToNumpad(StickOctagonal(axX, axY, deadzone));
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 masks = static_cast<u64>(held) |
                          (static_cast<u64>(pressed) << 16U) |
                          (static_cast<u64>(released) << 32U) |
                          (static_cast<u64>(retriggered) << 48U);
        u64 h = HashCombine64(Mix64(tick), masks);
        for (const f32 ax : axis) {
            h = HashCombine64(h, std::bit_cast<u32>(ax));
        }
        return h;
    }
};
static_assert(sizeof(TickInput) == 64);

// Render-rate 64-byte (1 L1 cache line exact, zero padding) UI and camera input snapshot.
// Packs mouse coordinates, frame-rate tap latches, active device indicator, and 18-byte UTF-8 text stream.
struct alignas(64) FrameInput {
    static constexpr usize kMaxTextBytes = 18;

    u64 frameIndex{0};
    Vec2 mousePos{};
    Vec2 mouseDelta{};
    Vec2 mouseWheel{};
    InputSource lastPressed{};
    u16 held{0};
    u16 pressed{0};
    u16 released{0};
    InputDevice activeDevice{InputDevice::Keyboard};
    bool hasFocus{true};
    u8 textLen{0};
    std::array<char, kMaxTextBytes + 1U> textInput{};

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return (held & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return (pressed & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return (released & (1U << ToInputIndex(a))) != 0U;
    }

    [[nodiscard]] constexpr std::string_view TextView() const noexcept {
        return std::string_view{textInput.data(), textLen};
    }
};
static_assert(sizeof(FrameInput) == 64);

} // namespace en

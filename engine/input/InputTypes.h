#pragma once

#include "core/Math.h"

#include <array>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>

namespace en {

inline constexpr usize kMaxActions = 16;
inline constexpr usize kMaxAxes = 4;
inline constexpr usize kMaxBindingsPerSlot = 4;
inline constexpr f32 kInvAxisMax = 1.0f / 32767.0f;

// Virtual pad buttons [20..31] via Schmitt on axes [0..5].
inline constexpr u16 kPadVirtualButtonBase = 20U;
inline constexpr i16 kAxisPressThreshold = 18000;
inline constexpr i16 kAxisReleaseThreshold = 12000;

enum class InputDevice : u8 { None, Keyboard, Mouse, Gamepad };

// Physical source. IsValid() gates all bit access.
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
        return Make(InputDevice::Gamepad,
            static_cast<u16>(kPadVirtualButtonBase + axisIdx * 2U + (positive ? 1U : 0U)));
    }
    [[nodiscard]] static constexpr InputSource PadTrigger(bool right) noexcept {
        return PadAxisButton(right ? 5U : 4U, true);
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

// Digital pair + alt (LIW / Neutral SOCD) + optional analog stick/trigger.
struct alignas(4) AxisBinding {
    static constexpr u8 kFlagInvert = 1U << 0;
    static constexpr u8 kFlagNeutralSocd = 1U << 1;

    InputSource neg{};
    InputSource pos{};
    InputSource altNeg{};
    InputSource altPos{};
    f32 deadzone{0.15f};
    u8 gamepadAxis{0xFFU}; // 0..5, or 0xFF = none
    u8 flags{0};           // kFlagInvert | kFlagNeutralSocd
    std::array<u8, 2> reserved{};

    [[nodiscard]] constexpr bool Invert() const noexcept { return (flags & kFlagInvert) != 0U; }
    [[nodiscard]] constexpr bool NeutralSocd() const noexcept {
        return (flags & kFlagNeutralSocd) != 0U;
    }

    constexpr void SetInvert(bool v) noexcept {
        flags = v ? static_cast<u8>(flags | kFlagInvert) : static_cast<u8>(flags & ~kFlagInvert);
    }
    constexpr void SetNeutralSocd(bool v) noexcept {
        flags = v ? static_cast<u8>(flags | kFlagNeutralSocd)
                  : static_cast<u8>(flags & ~kFlagNeutralSocd);
    }

    constexpr void Sanitize() noexcept {
        if (!neg.IsValid()) neg = {};
        if (!pos.IsValid()) pos = {};
        if (!altNeg.IsValid()) altNeg = {};
        if (!altPos.IsValid()) altPos = {};
        if (!(deadzone >= 0.0f && deadzone <= 0.90f)) deadzone = 0.15f;
        flags &= static_cast<u8>(kFlagInvert | kFlagNeutralSocd);
        reserved.fill(0);
        if (gamepadAxis >= 6U) gamepadAxis = 0xFFU;
    }
};
static_assert(sizeof(AxisBinding) == 16);
static_assert(alignof(AxisBinding) == 4);

// Binding table. Sanitize on write and deserialize. POD memcpy-safe.
class alignas(16) InputMap {
public:
    static constexpr usize kSerializedSize = 192;

    constexpr void Clear() noexcept {
        for (auto& s : m_actions) s.fill(InputSource{});
        m_axes.fill(AxisBinding{});
    }

    constexpr InputMap& BindSlot(u8 action, u8 slot, InputSource src) noexcept {
        if (slot < kMaxBindingsPerSlot)
            m_actions[action & 0x0FU][slot] = src.IsValid() ? src : InputSource{};
        return *this;
    }

    constexpr InputMap& Bind(u8 action, std::initializer_list<InputSource> sources) noexcept {
        auto& slots = m_actions[action & 0x0FU];
        slots.fill(InputSource{});
        u8 i = 0;
        for (const InputSource s : sources) {
            if (i >= kMaxBindingsPerSlot) break;
            if (s.IsValid()) slots[i++] = s;
        }
        return *this;
    }

    constexpr InputMap& Unbind(u8 action) noexcept {
        m_actions[action & 0x0FU].fill(InputSource{});
        return *this;
    }

    constexpr InputMap& BindAxis(u8 axis, AxisBinding b) noexcept {
        b.Sanitize();
        m_axes[axis & 3U] = b;
        return *this;
    }

    [[nodiscard]] constexpr std::span<const InputSource, kMaxBindingsPerSlot>
    Bindings(u8 action) const noexcept {
        return m_actions[action & 0x0FU];
    }

    [[nodiscard]] constexpr const AxisBinding& Axis(u8 axis) const noexcept {
        return m_axes[axis & 3U];
    }

    [[nodiscard]] usize Serialize(std::span<u8> out) const noexcept {
        if (out.size() < kSerializedSize) return 0U;
        std::memcpy(out.data(), m_actions.data(), sizeof(m_actions));
        std::memcpy(out.data() + sizeof(m_actions), m_axes.data(), sizeof(m_axes));
        return kSerializedSize;
    }

    [[nodiscard]] bool Deserialize(std::span<const u8> in) noexcept {
        if (in.size() < kSerializedSize) return false;
        std::memcpy(m_actions.data(), in.data(), sizeof(m_actions));
        std::memcpy(m_axes.data(), in.data() + sizeof(m_actions), sizeof(m_axes));
        for (auto& slots : m_actions)
            for (InputSource& s : slots)
                if (!s.IsValid()) s = {};
        for (AxisBinding& a : m_axes) a.Sanitize();
        return true;
    }

private:
    std::array<std::array<InputSource, kMaxBindingsPerSlot>, kMaxActions> m_actions{};
    std::array<AxisBinding, kMaxAxes> m_axes{};
};
static_assert(sizeof(InputMap) == 192);

// 120 Hz sim snapshot. heldTicks retains final duration on the release tick.
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
    [[nodiscard]] constexpr u16 HeldTicks(u8 a) const noexcept { return heldTicks[a & 0x0FU]; }
    [[nodiscard]] constexpr f32 Axis(u8 ax) const noexcept { return axis[ax & 3U]; }

    // Short tap release (variable jump cut).
    [[nodiscard]] constexpr bool TapReleased(u8 a, u16 maxTicks = 12) const noexcept {
        const u8 i = a & 0x0FU;
        const u16 b = static_cast<u16>(1U << i);
        return (released & b) != 0U && (held & b) == 0U &&
               heldTicks[i] > 0U && heldTicks[i] <= maxTicks;
    }

    // Charge beam / hold ratio in [0, 1].
    [[nodiscard]] constexpr f32 ChargeRatio(u8 a, u16 fullTicks = 30) const noexcept {
        if (fullTicks == 0U) return 1.0f;
        const u8 i = a & 0x0FU;
        if ((held & (1U << i)) == 0U) return 0.0f;
        return std::clamp(static_cast<f32>(heldTicks[i]) / static_cast<f32>(fullTicks), 0.0f, 1.0f);
    }

    [[nodiscard]] Vec2 Stick(u8 x = 0, u8 y = 1) const noexcept {
        Vec2 v{Axis(x), Axis(y)};
        const f32 lenSq = v.LengthSq();
        if (lenSq > 1.0f) {
            const f32 inv = 1.0f / std::sqrt(lenSq);
            v.x *= inv;
            v.y *= inv;
        }
        return v;
    }

    // 8-way snap (grid / tank controls).
    [[nodiscard]] static constexpr IVec2 Snap8(Vec2 raw, f32 dz = 0.25f) noexcept {
        if (!(dz >= 0.0f)) dz = 0.25f;
        if (raw.LengthSq() <= dz * dz) return {0, 0};
        constexpr f32 t = 0.41421356f; // tan(22.5 deg)
        const f32 ax = std::abs(raw.x), ay = std::abs(raw.y);
        return {
            (ax > ay * t) ? (raw.x >= 0.0f ? 1 : -1) : 0,
            (ay > ax * t) ? (raw.y >= 0.0f ? 1 : -1) : 0,
        };
    }
};
static_assert(sizeof(TickInput) == 64);

// Render-rate UI snapshot.
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
    [[nodiscard]] constexpr std::string_view Text() const noexcept {
        return {textInput.data(), textLen};
    }
};
static_assert(sizeof(FrameInput) == 64);

} // namespace en

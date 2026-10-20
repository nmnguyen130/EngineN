#pragma once

#include "core/Math.h"
#include <array>

namespace en {

inline constexpr usize kMaxGamepads = 4;
inline constexpr usize kMaxActions = 16;
inline constexpr usize kMaxAxes = 4;
inline constexpr usize kMaxBindingsPerSlot = 4;
inline constexpr f32 kInvAxisMax = 1.0f / 32767.0f;

template <typename T>
concept InputId = std::is_unsigned_v<T> || (std::is_enum_v<T> && sizeof(T) <= 4);

[[nodiscard]] constexpr u8 ToInputIndex(InputId auto id) noexcept {
    return static_cast<u8>(id) & 0x0FU;
}

template <InputId... Ids>
[[nodiscard]] constexpr u16 MakeActionMask(Ids... ids) noexcept {
    return static_cast<u16>(((1U << ToInputIndex(ids)) | ... | 0U));
}

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

struct alignas(4) InputSource {
    InputDevice device{InputDevice::None};
    u8 gamepadIndex{0xFFU};
    u16 code{0};

    [[nodiscard]] static constexpr InputSource Key(u16 scancode) noexcept {
        return {InputDevice::Keyboard, 0xFFU, scancode};
    }
    [[nodiscard]] static constexpr InputSource Mouse(u16 button) noexcept {
        return {InputDevice::Mouse, 0xFFU, button};
    }
    [[nodiscard]] static constexpr InputSource Pad(u16 button, u8 padSlot = 0xFFU) noexcept {
        return {InputDevice::Gamepad, padSlot, button};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return device != InputDevice::None && code < 512U;
    }
    [[nodiscard]] constexpr bool operator==(const InputSource&) const noexcept = default;
};
static_assert(sizeof(InputSource) == 4);

struct AxisBinding {
    InputSource negSource{};
    InputSource posSource{};
    u8 gamepadAxis{0xFFU};
    u8 padSlot{0xFFU};
    SocdPolicy socd{SocdPolicy::LastInputWins};
    bool invert{false};
    f32 deadzone{0.15f};
    f32 sensitivity{1.0f};
};
static_assert(sizeof(AxisBinding) == 20);

struct InputChord {
    u16 requiredMask{0};
    u8 slopWindowTicks{4};
    u8 outputAction{0};
};

struct InputSequence {
    std::array<u8, 4> steps{};
    u8 stepCount{0};
    u8 maxTicksBetweenSteps{15};
    u8 outputAction{0};
    u8 currentStep{0};
    u8 ticksSinceLastStep{0};
};

class alignas(64) InputMap {
public:
    constexpr void Clear() noexcept {
        for (auto& slots : m_actions) slots.fill(InputSource{});
        m_axes.fill(AxisBinding{});
        m_chords.fill(InputChord{});
        m_sequences.fill(InputSequence{});
    }

    constexpr InputMap& BindSlot(InputId auto action, u8 slot, InputSource src) noexcept {
        const u8 idx = ToInputIndex(action);
        if (slot < kMaxBindingsPerSlot && src.IsValid()) {
            m_actions[idx][slot] = src;
        }
        return *this;
    }

    constexpr InputMap& Bind(InputId auto action, std::initializer_list<InputSource> sources) noexcept {
        const u8 idx = ToInputIndex(action);
        u8 slot = 0;
        for (const InputSource& s : sources) {
            if (slot >= kMaxBindingsPerSlot) break;
            if (s.IsValid()) m_actions[idx][slot++] = s;
        }
        return *this;
    }

    constexpr InputMap& BindAxis(InputId auto axis, const AxisBinding& binding) noexcept {
        m_axes[ToInputIndex(axis) & 3U] = binding;
        return *this;
    }

    constexpr InputMap& BindChord(u8 chordSlot, InputChord chord) noexcept {
        if (chordSlot < 4U) m_chords[chordSlot] = chord;
        return *this;
    }

    constexpr InputMap& BindSequence(u8 seqSlot, const InputSequence& seq) noexcept {
        if (seqSlot < 4U) m_sequences[seqSlot] = seq;
        return *this;
    }

    [[nodiscard]] constexpr std::span<const InputSource, kMaxBindingsPerSlot> ActionBindings(u8 actionIdx) const noexcept {
        return m_actions[actionIdx & 0x0FU];
    }
    [[nodiscard]] constexpr const AxisBinding& Axis(u8 axisIdx) const noexcept {
        return m_axes[axisIdx & 3U];
    }
    [[nodiscard]] constexpr const InputChord& Chord(u8 chordSlot) const noexcept {
        return m_chords[chordSlot & 3U];
    }
    [[nodiscard]] constexpr InputSequence& Sequence(u8 seqSlot) noexcept {
        return m_sequences[seqSlot & 3U];
    }
    [[nodiscard]] constexpr const InputSequence& Sequence(u8 seqSlot) const noexcept {
        return m_sequences[seqSlot & 3U];
    }

    // InputRemapTable binary serialization for persistent user keybind profiles.
    [[nodiscard]] usize SerializeBindings(std::span<u8> outBytes) const noexcept {
        if (outBytes.size() < sizeof(m_actions) + sizeof(m_axes) + sizeof(m_chords)) return 0;
        usize offset = 0;
        std::memcpy(outBytes.data() + offset, m_actions.data(), sizeof(m_actions));
        offset += sizeof(m_actions);
        std::memcpy(outBytes.data() + offset, m_axes.data(), sizeof(m_axes));
        offset += sizeof(m_axes);
        std::memcpy(outBytes.data() + offset, m_chords.data(), sizeof(m_chords));
        offset += sizeof(m_chords);
        return offset;
    }

    [[nodiscard]] bool DeserializeBindings(std::span<const u8> inBytes) noexcept {
        if (inBytes.size() < sizeof(m_actions) + sizeof(m_axes) + sizeof(m_chords)) return false;
        usize offset = 0;
        std::memcpy(m_actions.data(), inBytes.data() + offset, sizeof(m_actions));
        offset += sizeof(m_actions);
        std::memcpy(m_axes.data(), inBytes.data() + offset, sizeof(m_axes));
        offset += sizeof(m_axes);
        std::memcpy(m_chords.data(), inBytes.data() + offset, sizeof(m_chords));
        return true;
    }

private:
    std::array<std::array<InputSource, kMaxBindingsPerSlot>, kMaxActions> m_actions{};
    std::array<AxisBinding, kMaxAxes> m_axes{};
    std::array<InputChord, 4> m_chords{};
    std::array<InputSequence, 4> m_sequences{};
};

struct alignas(64) TickInput {
    u64 tick{0};
    u16 held{0};
    u16 pressed{0};
    u16 released{0};
    u16 chordMask{0};
    std::array<f32, kMaxAxes> axis{};
    std::array<u16, kMaxActions> heldTicks{};

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return (held & (1U << ToInputIndex(a))) != 0;
    }
    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return (pressed & (1U << ToInputIndex(a))) != 0;
    }
    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return (released & (1U << ToInputIndex(a))) != 0;
    }
    [[nodiscard]] constexpr u16 HeldTicks(InputId auto a) const noexcept {
        return heldTicks[ToInputIndex(a)];
    }
    [[nodiscard]] constexpr f32 Axis(InputId auto ax) const noexcept {
        return axis[ToInputIndex(ax) & 3U];
    }
    [[nodiscard]] Vec2 Stick(InputId auto axX, InputId auto axY, f32 radialDeadzone = 0.15f) const noexcept {
        const Vec2 raw{Axis(axX), Axis(axY)};
        const f32 len = raw.Length();
        if (len <= radialDeadzone) return {0.0f, 0.0f};
        const f32 norm = std::min((len - radialDeadzone) / (1.0f - radialDeadzone), 1.0f);
        return raw * (norm / len);
    }
    [[nodiscard]] static Vec2 ApplyRadialDeadzone(Vec2 raw, f32 innerDz = 0.15f, f32 outerDz = 0.95f) noexcept {
        const f32 len = raw.Length();
        if (len <= innerDz || outerDz <= innerDz) return {0.0f, 0.0f};
        const f32 t = std::clamp((len - innerDz) / (outerDz - innerDz), 0.0f, 1.0f);
        return (raw / len) * t;
    }
    [[nodiscard]] static constexpr Vec2 ApplyAxialDeadzone(Vec2 raw, f32 dzX = 0.15f, f32 dzY = 0.15f) noexcept {
        auto remapAxis = [](f32 v, f32 dz) constexpr noexcept -> f32 {
            const f32 av = (v < 0.0f) ? -v : v;
            if (av <= dz || dz >= 1.0f) return 0.0f;
            const f32 n = std::clamp((av - dz) / (1.0f - dz), 0.0f, 1.0f);
            return (v < 0.0f) ? -n : n;
        };
        return {remapAxis(raw.x, dzX), remapAxis(raw.y, dzY)};
    }
    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 masks = static_cast<u64>(held) |
                          (static_cast<u64>(pressed) << 16U) |
                          (static_cast<u64>(released) << 32U) |
                          (static_cast<u64>(chordMask) << 48U);
        return HashCombine64(Mix64(tick), masks);
    }
};
static_assert(sizeof(TickInput) == 64);

struct alignas(32) FrameInput {
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

    [[nodiscard]] constexpr bool Held(InputId auto a) const noexcept {
        return (held & (1U << ToInputIndex(a))) != 0;
    }
    [[nodiscard]] constexpr bool Pressed(InputId auto a) const noexcept {
        return (pressed & (1U << ToInputIndex(a))) != 0;
    }
    [[nodiscard]] constexpr bool Released(InputId auto a) const noexcept {
        return (released & (1U << ToInputIndex(a))) != 0;
    }
};
static_assert(sizeof(FrameInput) == 64);

} // namespace en

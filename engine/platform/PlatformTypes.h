#pragma once

#include "core/Types.h"
#include <array>

namespace en {

enum class PlatformErrc : u8 {
    None = 0,
    SdlInitFailed,
    WindowCreateFailed,
    FullscreenFailed,
    PathUnavailable,
    PathRejected,
    FileIoFailed,
    ClipboardFailed,
    InvalidState,
};

struct alignas(8) Extent2i {
    i32 w{0};
    i32 h{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
    }
    [[nodiscard]] constexpr bool operator==(const Extent2i&) const noexcept = default;
};
static_assert(sizeof(Extent2i) == 8);

enum class WindowMode : u8 {
    Windowed = 0,
    Borderless = 1,
    ExclusiveFullscreen = 2,
};

enum class WindowFlags : u8 {
    None = 0,
    Resized = 1U << 0,
    DisplayChanged = 1U << 1,
    FocusGained = 1U << 2,
    FocusLost = 1U << 3,
    CloseRequested = 1U << 4,
};
consteval bool enable_bitmask_operators(WindowFlags) { return true; }

struct WindowConfig {
    std::string_view title{"EngineN 2026"};
    Extent2i size{1280, 720};
    Extent2i minSize{640, 360};
    WindowMode mode{WindowMode::Windowed};
    bool resizable{true};
    bool highDpi{true};
};

struct alignas(32) WindowState {
    Extent2i logicalSize{1280, 720};
    Extent2i pixelSize{1280, 720};
    f32 pixelDensity{1.0f};
    f32 displayScale{1.0f};
    f32 refreshHz{60.0f};
    WindowMode mode{WindowMode::Windowed};
    bool focused{true};
    bool suspended{false};
    bool relativeMouse{false};

    [[nodiscard]] constexpr bool IsPresentable() const noexcept {
        return !suspended && pixelSize.IsValid();
    }
    [[nodiscard]] constexpr f32 AspectRatio() const noexcept { return pixelSize.AspectRatio(); }
    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = Mix64(static_cast<u64>(pixelSize.w) | (static_cast<u64>(pixelSize.h) << 32U));
        return HashCombine64(h, static_cast<u64>(mode) | (focused ? 256ULL : 0ULL));
    }
};
static_assert(sizeof(WindowState) == 32);

enum class EventCapture : u8 {
    None = 0,
    Keyboard = 1U << 0,
    Mouse = 1U << 1,
    Gamepad = 1U << 2,
};
consteval bool enable_bitmask_operators(EventCapture) { return true; }

enum class InputEventKind : u8 {
    Key = 0,
    MouseButton = 1,
    GamepadButton = 2,
    GamepadAxis = 3,
};

struct alignas(4) PlatformInputEvent {
    u16 packedHeader{0};
    i16 value{0};

    [[nodiscard]] static constexpr PlatformInputEvent Make(
        InputEventKind kind, u8 padSlot, u16 code, i16 val) noexcept {
        const u16 hdr = static_cast<u16>((code & 0x0FFFU) |
                        ((padSlot & 0x3U) << 12U) |
                        (static_cast<u16>(kind) << 14U));
        return {hdr, val};
    }

    [[nodiscard]] constexpr u16 Code() const noexcept { return packedHeader & 0x0FFFU; }
    [[nodiscard]] constexpr u8 PadSlot() const noexcept { return static_cast<u8>((packedHeader >> 12U) & 0x3U); }
    [[nodiscard]] constexpr InputEventKind Kind() const noexcept {
        return static_cast<InputEventKind>((packedHeader >> 14U) & 0x3U);
    }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
};
static_assert(sizeof(PlatformInputEvent) == 4);

struct alignas(64) PlatformFrameEvents {
    static constexpr usize kMaxEvents = 57;

    f32 mouseX{0.0f};
    f32 mouseY{0.0f};
    f32 mouseDx{0.0f};
    f32 mouseDy{0.0f};
    f32 wheelX{0.0f};
    f32 wheelY{0.0f};
    u8 eventCount{0};
    u8 disconnectedPadMask{0};
    u8 focusCleared{0};
    u8 hasMouseMotion{0};
    std::array<PlatformInputEvent, kMaxEvents> events{};

    constexpr void ClearFrameDeltas() noexcept {
        mouseDx = 0.0f;
        mouseDy = 0.0f;
        wheelX = 0.0f;
        wheelY = 0.0f;
        eventCount = 0;
        disconnectedPadMask = 0;
        focusCleared = 0;
        hasMouseMotion = 0;
    }

    constexpr void ClearInput(bool markFocusLost = false) noexcept {
        ClearFrameDeltas();
        if (markFocusLost) focusCleared = 1;
    }

    constexpr void PushEvent(InputEventKind kind, u8 padSlot, u16 code, i16 value) noexcept {
        const PlatformInputEvent ev = PlatformInputEvent::Make(kind, padSlot, code, value);
        if (kind == InputEventKind::GamepadAxis && eventCount > 0) {
            if (events[eventCount - 1].packedHeader == ev.packedHeader) {
                events[eventCount - 1].value = value;
                return;
            }
        }
        if (eventCount < kMaxEvents) {
            events[eventCount++] = ev;
        }
    }

    [[nodiscard]] constexpr std::span<const PlatformInputEvent> Events() const noexcept {
        return std::span<const PlatformInputEvent>(events.data(), eventCount);
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = Mix64(static_cast<u64>(eventCount) | (static_cast<u64>(focusCleared) << 8U));
        for (u8 i = 0; i < eventCount; ++i) {
            const u32 word = static_cast<u32>(events[i].packedHeader) |
                             (static_cast<u32>(static_cast<u16>(events[i].value)) << 16U);
            h = HashCombine64(h, word);
        }
        return h;
    }
};
static_assert(sizeof(PlatformFrameEvents) == 256);

} // namespace en

#pragma once

#include "core/Math.h"

namespace en {

using FilePath = FixedString<256>;
static_assert(sizeof(FilePath) == 256);

enum class PlatformErrc : u8 {
    None = 0,
    SdlInitFailed,
    WindowCreateFailed,
    FullscreenFailed,
    PathRejected,
    FileIoFailed,
    InvalidState,
};

struct alignas(8) Extent2i {
    i32 w{0};
    i32 h{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }

    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return IsValid() ? (static_cast<f32>(w) / static_cast<f32>(h)) : 1.0f;
    }

    [[nodiscard]] constexpr bool operator==(const Extent2i&) const noexcept = default;
};
static_assert(sizeof(Extent2i) == 8);

enum class WindowMode : u8 {
    Windowed = 0,
    Borderless = 1,
    ExclusiveFullscreen = 2,
};

enum class ScaleMode : u8 {
    Stretch = 0,
    Letterbox = 1,
    IntegerScale = 2,
    Expand = 3,
};

enum class WindowFlags : u8 {
    None = 0,
    Resized = 1U << 0,
    DisplayChanged = 1U << 1,
    FocusGained = 1U << 2,
    FocusLost = 1U << 3,
    Minimized = 1U << 4,
    Restored = 1U << 5,
    CloseRequested = 1U << 6,
};
consteval bool enable_bitmask_operators(WindowFlags) { return true; }

struct alignas(8) WindowConfig {
    std::string_view title{"EngineN"};
    Extent2i size{1280, 720};
    f32 targetRefreshHz{0.0f};
    WindowMode mode{WindowMode::Windowed};
    bool resizable{true};
    bool highDpi{true};
};
static_assert(sizeof(WindowConfig) == 32);

// Window and display metrics with high-DPI coordinate conversion and 2D virtual canvas viewport scaling.
// Value type stored inside Window.
// Invariant: pixelDensity > 0.0f, displayScale > 0.0f, and refreshHz > 0.0f.
struct alignas(8) WindowState {
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
    [[nodiscard]] constexpr Vec2 WindowToPixel(Vec2 windowPos) const noexcept { return windowPos * pixelDensity; }
    [[nodiscard]] constexpr Vec2 PixelToWindow(Vec2 pixelPos) const noexcept {
        return pixelDensity > 0.0f ? (pixelPos / pixelDensity) : pixelPos;
    }
    [[nodiscard]] constexpr Vec2 PixelToUiLogical(Vec2 pixelPos) const noexcept {
        return displayScale > 0.0f ? (pixelPos / displayScale) : pixelPos;
    }
    [[nodiscard]] constexpr Vec2 UiLogicalToPixel(Vec2 uiPos) const noexcept { return uiPos * displayScale; }

    [[nodiscard]] constexpr Rect ComputeViewport(
        Extent2i virtualSize, ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        if (!pixelSize.IsValid() || !virtualSize.IsValid()) return {};
        const f32 pw = static_cast<f32>(pixelSize.w);
        const f32 ph = static_cast<f32>(pixelSize.h);
        if (scaleMode == ScaleMode::Stretch) return {0.0f, 0.0f, pw, ph};

        if (scaleMode == ScaleMode::IntegerScale) {
            if (const i32 s = std::min(pixelSize.w / virtualSize.w, pixelSize.h / virtualSize.h); s >= 1) {
                const i32 dw = virtualSize.w * s;
                const i32 dh = virtualSize.h * s;
                return {
                    static_cast<f32>((pixelSize.w - dw) / 2),
                    static_cast<f32>((pixelSize.h - dh) / 2),
                    static_cast<f32>(dw),
                    static_cast<f32>(dh),
                };
            }
        }

        const f32 vw = static_cast<f32>(virtualSize.w);
        const f32 vh = static_cast<f32>(virtualSize.h);
        const f32 scale = (scaleMode == ScaleMode::Expand)
            ? std::max(pw / vw, ph / vh)
            : std::min(pw / vw, ph / vh);
        const f32 dstW = static_cast<f32>(static_cast<i32>(vw * scale + 0.5f));
        const f32 dstH = static_cast<f32>(static_cast<i32>(vh * scale + 0.5f));
        return {
            static_cast<f32>(static_cast<i32>((pw - dstW) * 0.5f)),
            static_cast<f32>(static_cast<i32>((ph - dstH) * 0.5f)),
            dstW,
            dstH,
        };
    }

    [[nodiscard]] constexpr Vec2 PixelToVirtual(
        Vec2 pixelPos,
        Extent2i virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox,
        bool clampToCanvas = true) const noexcept {
        const Rect vp = ComputeViewport(virtualSize, scaleMode);
        if (vp.IsEmpty()) return {};
        const f32 vw = static_cast<f32>(virtualSize.w);
        const f32 vh = static_cast<f32>(virtualSize.h);
        Vec2 virt{(pixelPos.x - vp.x) * (vw / vp.w), (pixelPos.y - vp.y) * (vh / vp.h)};
        if (clampToCanvas) {
            virt.x = std::clamp(virt.x, 0.0f, vw);
            virt.y = std::clamp(virt.y, 0.0f, vh);
        }
        return virt;
    }

    [[nodiscard]] constexpr Vec2 VirtualToPixel(
        Vec2 virtualPos,
        Extent2i virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        const Rect vp = ComputeViewport(virtualSize, scaleMode);
        if (vp.IsEmpty() || !virtualSize.IsValid()) return {};
        return {
            vp.x + virtualPos.x * (vp.w / static_cast<f32>(virtualSize.w)),
            vp.y + virtualPos.y * (vp.h / static_cast<f32>(virtualSize.h)),
        };
    }

    constexpr WindowFlags ApplyMetrics(
        Extent2i newLogical,
        Extent2i newPixel,
        f32 newDensity,
        f32 newDisplayScale,
        f32 newRefreshHz) noexcept {
        WindowFlags flags = WindowFlags::None;

        if (newLogical.IsValid() && newLogical != logicalSize) {
            logicalSize = newLogical;
            flags |= WindowFlags::Resized;
        }
        if (newPixel.IsValid() && newPixel != pixelSize) {
            pixelSize = newPixel;
            flags |= WindowFlags::Resized;
        }

        const f32 validDensity = newDensity > 0.0f ? newDensity : 1.0f;
        const f32 validScale = newDisplayScale > 0.0f ? newDisplayScale : validDensity;
        const f32 validHz = newRefreshHz > 0.0f ? newRefreshHz : 60.0f;

        if (std::abs(validDensity - pixelDensity) > 1e-3f ||
            std::abs(validScale - displayScale) > 1e-3f ||
            std::abs(validHz - refreshHz) > 1e-2f) {
            flags |= WindowFlags::DisplayChanged;
        }

        pixelDensity = validDensity;
        displayScale = validScale;
        refreshHz = validHz;
        return flags;
    }

    constexpr void ApplyLifecycleEvent(
        WindowFlags eventFlag, WindowFlags& ioNetFlags, bool& outNeedRefresh) noexcept {
        if (HasFlag(eventFlag, WindowFlags::Resized | WindowFlags::DisplayChanged)) {
            outNeedRefresh = true;
        }
        if (HasFlag(eventFlag, WindowFlags::Minimized)) {
            suspended = true;
            focused = false;
            ioNetFlags &= ~(WindowFlags::Restored | WindowFlags::FocusGained);
        }
        if (HasFlag(eventFlag, WindowFlags::Restored)) {
            suspended = false;
            outNeedRefresh = true;
            ioNetFlags &= ~WindowFlags::Minimized;
        }
        if (HasFlag(eventFlag, WindowFlags::FocusLost)) {
            focused = false;
            ioNetFlags &= ~WindowFlags::FocusGained;
        }
        if (HasFlag(eventFlag, WindowFlags::FocusGained)) {
            focused = true;
            ioNetFlags &= ~WindowFlags::FocusLost;
        }
        ioNetFlags |= eventFlag;
    }
};
static_assert(sizeof(WindowState) == 32);

enum class EventCapture : u8 {
    None = 0,
    Keyboard = 1U << 0,
    Mouse = 1U << 1,
    Gamepad = 1U << 2,
    All = Keyboard | Mouse | Gamepad,
};
consteval bool enable_bitmask_operators(EventCapture) { return true; }

enum class InputEventKind : u8 {
    Key = 0,
    MouseButton = 1,
    GamepadButton = 2,
    GamepadAxis = 3,
};

// Packed 4-byte discrete physical input event record for single-player controller architecture.
// Bits [0..13] = device code, bits [14..15] = InputEventKind.
// Invariant: GamepadAxis values are clamped to [-32767, 32767] for symmetric float normalization.
struct alignas(4) PlatformInputEvent {
    u16 packedHeader{0};
    i16 value{0};

    [[nodiscard]] static constexpr PlatformInputEvent Make(
        InputEventKind kind, u16 code, i16 val) noexcept {
        const u16 hdr = static_cast<u16>((code & 0x3FFFU) | (static_cast<u16>(kind) << 14U));
        return {hdr, val};
    }

    [[nodiscard]] constexpr u16 Code() const noexcept { return packedHeader & 0x3FFFU; }
    [[nodiscard]] constexpr InputEventKind Kind() const noexcept {
        return static_cast<InputEventKind>((packedHeader >> 14U) & 0x3U);
    }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const PlatformInputEvent&) const noexcept = default;
};
static_assert(sizeof(PlatformInputEvent) == 4);

// Coalesced per-frame input and UTF-8 text batch.
// Invariant: mouseX/Y/Dx/Dy are in physical pixel space, and each gamepad axis occupies at most 1 slot.
struct alignas(16) PlatformFrameEvents {
    static constexpr usize kMaxTextBytes = 31;
    static constexpr usize kMaxEvents = 49;

    f32 mouseX{0.0f};
    f32 mouseY{0.0f};
    f32 mouseDx{0.0f};
    f32 mouseDy{0.0f};
    f32 wheelX{0.0f};
    f32 wheelY{0.0f};
    u8 eventCount{0};
    u8 padDisconnected{0};
    u8 focused{1};
    u8 textLen{0};
    std::array<char, kMaxTextBytes + 1U> textInput{};
    std::array<PlatformInputEvent, kMaxEvents> events{};

    constexpr void ClearFrameDeltas() noexcept {
        mouseDx = 0.0f;
        mouseDy = 0.0f;
        wheelX = 0.0f;
        wheelY = 0.0f;
        eventCount = 0;
        padDisconnected = 0;
        focused = 1U;
        textLen = 0;
        textInput[0] = '\0';
    }

    constexpr void PushEvent(InputEventKind kind, u16 code, i16 value) noexcept {
        // Clamp INT16_MIN (-32768) to -32767 so downstream division by 32767.0f stays within [-1.0f, 1.0f].
        const i16 clampedVal = (kind == InputEventKind::GamepadAxis && value == -32768) ? i16{-32767} : value;
        const PlatformInputEvent ev = PlatformInputEvent::Make(kind, code, clampedVal);

        if (kind == InputEventKind::GamepadAxis) {
            for (u8 i = 0U; i < eventCount; ++i) {
                if (events[i].packedHeader == ev.packedHeader) {
                    events[i].value = clampedVal;
                    return;
                }
            }
        } else {
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].packedHeader == ev.packedHeader) {
                    if (events[i - 1U].value == clampedVal) {
                        return;
                    }
                    break;
                }
            }
        }

        if (eventCount < kMaxEvents) {
            events[eventCount++] = ev;
        }
    }

    constexpr void IngestKey(
        u16 scancode, bool down, bool isRepeat, bool windowFocused, EventCapture capture) noexcept {
        if (down && isRepeat) return;
        if (!down || (windowFocused && !HasFlag(capture, EventCapture::Keyboard))) {
            PushEvent(InputEventKind::Key, scancode, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestMouseButton(
        u16 button,
        bool down,
        f32 logicalX,
        f32 logicalY,
        f32 pixelDensity,
        bool windowFocused,
        EventCapture capture) noexcept {
        const f32 d = pixelDensity > 0.0f ? pixelDensity : 1.0f;
        const bool passMouse = windowFocused && !HasFlag(capture, EventCapture::Mouse);
        if (passMouse) {
            mouseX = logicalX * d;
            mouseY = logicalY * d;
        }
        if (!down || passMouse) {
            PushEvent(InputEventKind::MouseButton, button, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestMouseMotion(
        f32 logicalX,
        f32 logicalY,
        f32 logicalDx,
        f32 logicalDy,
        f32 pixelDensity,
        bool windowFocused,
        EventCapture capture) noexcept {
        if (windowFocused && !HasFlag(capture, EventCapture::Mouse)) {
            const f32 d = pixelDensity > 0.0f ? pixelDensity : 1.0f;
            mouseX = logicalX * d;
            mouseY = logicalY * d;
            mouseDx += logicalDx * d;
            mouseDy += logicalDy * d;
        }
    }

    constexpr void IngestMouseWheel(
        f32 wx, f32 wy, bool flipped, bool windowFocused, EventCapture capture) noexcept {
        if (windowFocused && !HasFlag(capture, EventCapture::Mouse)) {
            const f32 sign = flipped ? -1.0f : 1.0f;
            wheelX += wx * sign;
            wheelY += wy * sign;
        }
    }

    constexpr void IngestTextInput(std::string_view utf8, bool windowFocused) noexcept {
        if (!windowFocused || utf8.empty()) return;
        usize validLen = 0U;
        while (validLen < utf8.size() && utf8[validLen] != '\0') ++validLen;
        if (validLen == 0U) return;

        const usize avail = (textLen < kMaxTextBytes) ? (kMaxTextBytes - textLen) : 0U;
        usize toCopy = std::min<usize>(validLen, avail);
        if (toCopy < validLen) {
            while (toCopy > 0U && (static_cast<u8>(utf8[toCopy]) & 0xC0U) == 0x80U) {
                --toCopy;
            }
        }

        for (usize i = 0U; i < toCopy; ++i) {
            textInput[textLen++] = utf8[i];
        }
        textInput[textLen] = '\0';
    }

    constexpr void IngestGamepadButton(
        u16 button, bool down, bool windowFocused, EventCapture capture) noexcept {
        if (!down || (windowFocused && !HasFlag(capture, EventCapture::Gamepad))) {
            PushEvent(InputEventKind::GamepadButton, button, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestGamepadAxis(
        u16 axis, i16 value, bool windowFocused, EventCapture capture) noexcept {
        if (windowFocused && !HasFlag(capture, EventCapture::Gamepad)) {
            PushEvent(InputEventKind::GamepadAxis, axis, value);
        }
    }

    constexpr void IngestGamepadDisconnect() noexcept {
        padDisconnected = 1U;
    }

    [[nodiscard]] constexpr std::span<const PlatformInputEvent> Events() const noexcept {
        return {events.data(), eventCount};
    }

    [[nodiscard]] constexpr std::string_view TextView() const noexcept {
        return {textInput.data(), textLen};
    }

    [[nodiscard]] constexpr bool HasMouseWheel() const noexcept {
        return wheelX != 0.0f || wheelY != 0.0f;
    }
};
static_assert(sizeof(PlatformFrameEvents) == 256);

} // namespace en

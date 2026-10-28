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

enum class WindowMode : u8 { Windowed, Borderless, ExclusiveFullscreen };

enum class ScaleMode : u8 { Stretch, Letterbox, IntegerScale, Expand };

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

// Metrics + DPI + virtual-canvas viewport. Invariant: density/scale/hz > 0.
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

    [[nodiscard]] constexpr Vec2 WindowToPixel(Vec2 pos) const noexcept {
        return pos * pixelDensity;
    }
    [[nodiscard]] constexpr Vec2 PixelToWindow(Vec2 pos) const noexcept {
        return pixelDensity > 0.0f ? (pos / pixelDensity) : pos;
    }
    [[nodiscard]] constexpr Vec2 PixelToUiLogical(Vec2 pos) const noexcept {
        return displayScale > 0.0f ? (pos / displayScale) : pos;
    }
    [[nodiscard]] constexpr Vec2 UiLogicalToPixel(Vec2 pos) const noexcept {
        return pos * displayScale;
    }

    [[nodiscard]] constexpr Rect ComputeViewport(
        Extent2i virtualSize, ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        if (!pixelSize.IsValid() || !virtualSize.IsValid()) return {};
        const f32 pixelW = static_cast<f32>(pixelSize.w);
        const f32 pixelH = static_cast<f32>(pixelSize.h);
        if (scaleMode == ScaleMode::Stretch) return {0.0f, 0.0f, pixelW, pixelH};

        if (scaleMode == ScaleMode::IntegerScale) {
            if (const i32 intScale = std::min(pixelSize.w / virtualSize.w, pixelSize.h / virtualSize.h); intScale >= 1) {
                const i32 drawW = virtualSize.w * intScale;
                const i32 drawH = virtualSize.h * intScale;
                return {
                    static_cast<f32>((pixelSize.w - drawW) / 2),
                    static_cast<f32>((pixelSize.h - drawH) / 2),
                    static_cast<f32>(drawW),
                    static_cast<f32>(drawH),
                };
            }
        }

        const f32 virtW = static_cast<f32>(virtualSize.w);
        const f32 virtH = static_cast<f32>(virtualSize.h);
        const f32 scale = (scaleMode == ScaleMode::Expand)
            ? std::max(pixelW / virtW, pixelH / virtH)
            : std::min(pixelW / virtW, pixelH / virtH);
        const f32 destW = static_cast<f32>(static_cast<i32>(virtW * scale + 0.5f));
        const f32 destH = static_cast<f32>(static_cast<i32>(virtH * scale + 0.5f));
        return {
            static_cast<f32>(static_cast<i32>((pixelW - destW) * 0.5f)),
            static_cast<f32>(static_cast<i32>((pixelH - destH) * 0.5f)),
            destW,
            destH,
        };
    }

    [[nodiscard]] constexpr Vec2 PixelToVirtual(
        Vec2 pixelPos,
        Extent2i virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox,
        bool clampToCanvas = true) const noexcept {
        const Rect viewport = ComputeViewport(virtualSize, scaleMode);
        if (viewport.IsEmpty()) return {};
        const f32 virtW = static_cast<f32>(virtualSize.w);
        const f32 virtH = static_cast<f32>(virtualSize.h);
        Vec2 result{
            (pixelPos.x - viewport.x) * (virtW / viewport.w),
            (pixelPos.y - viewport.y) * (virtH / viewport.h),
        };
        if (clampToCanvas) {
            result.x = std::clamp(result.x, 0.0f, virtW);
            result.y = std::clamp(result.y, 0.0f, virtH);
        }
        return result;
    }

    [[nodiscard]] constexpr Vec2 VirtualToPixel(
        Vec2 virtualPos,
        Extent2i virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        const Rect viewport = ComputeViewport(virtualSize, scaleMode);
        if (viewport.IsEmpty() || !virtualSize.IsValid()) return {};
        return {
            viewport.x + virtualPos.x * (viewport.w / static_cast<f32>(virtualSize.w)),
            viewport.y + virtualPos.y * (viewport.h / static_cast<f32>(virtualSize.h)),
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
        const f32 density = newDensity > 0.0f ? newDensity : 1.0f;
        const f32 scale = newDisplayScale > 0.0f ? newDisplayScale : density;
        const f32 hz = newRefreshHz > 0.0f ? newRefreshHz : 60.0f;
        if (std::abs(density - pixelDensity) > 1e-3f || std::abs(scale - displayScale) > 1e-3f ||
            std::abs(hz - refreshHz) > 1e-2f) {
            flags |= WindowFlags::DisplayChanged;
        }
        pixelDensity = density;
        displayScale = scale;
        refreshHz = hz;
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

enum class InputEventKind : u8 { Key = 0, MouseButton = 1, GamepadButton = 2, GamepadAxis = 3 };

// 4B packed event. code[0..13] | kind[14..15]. Axis clamped to [-32767, 32767].
struct alignas(4) PlatformInputEvent {
    u16 packedHeader{0};
    i16 value{0};

    [[nodiscard]] static constexpr PlatformInputEvent Make(
        InputEventKind kind, u16 code, i16 val) noexcept {
        return {
            static_cast<u16>((code & 0x3FFFU) | (static_cast<u16>(kind) << 14U)),
            val,
        };
    }
    [[nodiscard]] constexpr u16 Code() const noexcept { return packedHeader & 0x3FFFU; }
    [[nodiscard]] constexpr InputEventKind Kind() const noexcept {
        return static_cast<InputEventKind>((packedHeader >> 14U) & 0x3U);
    }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const PlatformInputEvent&) const noexcept = default;
};
static_assert(sizeof(PlatformInputEvent) == 4);

// One frame batch. Pixel-space mouse. Axis coalesce; key/button releases never dropped when full.
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
        const i16 clamped =
            (kind == InputEventKind::GamepadAxis && value == -32768) ? i16{-32767} : value;
        const PlatformInputEvent ev = PlatformInputEvent::Make(kind, code, clamped);

        if (kind == InputEventKind::GamepadAxis) {
            for (u8 i = 0U; i < eventCount; ++i) {
                if (events[i].packedHeader == ev.packedHeader) {
                    events[i].value = clamped;
                    return;
                }
            }
        } else {
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].packedHeader == ev.packedHeader) {
                    if (events[i - 1U].value == clamped) return;
                    break;
                }
            }
        }

        if (eventCount < kMaxEvents) {
            events[eventCount++] = ev;
            return;
        }

        // Full: never drop a discrete release (would stick inputs).
        if (kind != InputEventKind::GamepadAxis && clamped == 0) {
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].packedHeader == ev.packedHeader) {
                    events[i - 1U].value = 0;
                    return;
                }
            }
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].Kind() == InputEventKind::GamepadAxis ||
                    events[i - 1U].value != 0) {
                    events[i - 1U] = ev;
                    return;
                }
            }
            events[kMaxEvents - 1U] = ev;
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
        const f32 density = pixelDensity > 0.0f ? pixelDensity : 1.0f;
        const bool acceptMouse = windowFocused && !HasFlag(capture, EventCapture::Mouse);
        if (acceptMouse) {
            mouseX = logicalX * density;
            mouseY = logicalY * density;
        }
        if (!down || acceptMouse) {
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
            const f32 density = pixelDensity > 0.0f ? pixelDensity : 1.0f;
            mouseX = logicalX * density;
            mouseY = logicalY * density;
            mouseDx += logicalDx * density;
            mouseDy += logicalDy * density;
        }
    }

    constexpr void IngestMouseWheel(
        f32 wheelDeltaX,
        f32 wheelDeltaY,
        bool flipped,
        bool windowFocused,
        EventCapture capture) noexcept {
        if (windowFocused && !HasFlag(capture, EventCapture::Mouse)) {
            const f32 sign = flipped ? -1.0f : 1.0f;
            wheelX += wheelDeltaX * sign;
            wheelY += wheelDeltaY * sign;
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
            while (toCopy > 0U && (static_cast<u8>(utf8[toCopy]) & 0xC0U) == 0x80U) --toCopy;
        }
        for (usize i = 0U; i < toCopy; ++i) textInput[textLen++] = utf8[i];
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

    constexpr void IngestGamepadDisconnect() noexcept { padDisconnected = 1U; }

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

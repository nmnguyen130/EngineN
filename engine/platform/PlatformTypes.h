#pragma once

#include "core/Math.h"

namespace en {

using FilePath = FixedString<256>;
static_assert(sizeof(FilePath) == 256);

enum class PlatformErrc : u8 {
    None               = 0,
    InvalidState       = 1,
    SdlInitFailed      = 2,
    WindowCreateFailed = 3,
    FullscreenFailed   = 4,
    PathRejected       = 5,
    FileIoFailed       = 6,
};

enum class WindowMode : u8 {
    Windowed            = 0,
    Borderless          = 1,
    ExclusiveFullscreen = 2,
};

enum class ScaleMode : u8 {
    Stretch      = 0,
    Letterbox    = 1,
    IntegerScale = 2,
    Expand       = 3,
};

enum class WindowFlags : u8 {
    None           = 0,
    Resized        = 1U << 0,
    DisplayChanged = 1U << 1,
    FocusGained    = 1U << 2,
    FocusLost      = 1U << 3,
    Minimized      = 1U << 4,
    Restored       = 1U << 5,
    CloseRequested = 1U << 6,
};
consteval bool enable_bitmask_operators(WindowFlags) { return true; }

enum class EventCapture : u8 {
    None     = 0,
    Keyboard = 1U << 0,
    Mouse    = 1U << 1,
    Gamepad  = 1U << 2,
    All      = Keyboard | Mouse | Gamepad,
};
consteval bool enable_bitmask_operators(EventCapture) { return true; }

struct WindowSize {
    i32 w{1280};
    i32 h{720};

    [[nodiscard]] constexpr bool IsValid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return IsValid() ? (static_cast<f32>(w) / static_cast<f32>(h)) : 1.0f;
    }
    [[nodiscard]] constexpr bool operator==(const WindowSize&) const noexcept = default;
};
static_assert(sizeof(WindowSize) == 8);

struct WindowConfig {
    std::string_view title{"EngineN"};
    WindowSize       size{1280, 720};
    f32              targetRefreshHz{0.0f};
    WindowMode       mode{WindowMode::Windowed};
    bool             resizable{true};
    bool             highDpi{true};
};
static_assert(sizeof(WindowConfig) == 32);

// Normalize directory slashes and ensure trailing slash; rejects embedded nulls or overflow.
[[nodiscard]] constexpr bool NormalizeDir(std::string_view raw, FilePath& out) noexcept {
    out.Clear();
    if (raw.empty() || raw.contains('\0')) return false;
    char prev = '\0';
    for (const char ch : raw) {
        const char norm = (ch == '\\') ? '/' : ch;
        if (norm == '/' && prev == '/' && out.Size() > 1U) continue;
        if (!out.Append({&norm, 1U})) return false;
        prev = norm;
    }
    if (!out.Empty() && out.View().back() != '/') {
        if (!out.Append("/")) return false;
    }
    return !out.Empty();
}

// Resolve a sandboxed relative path against a normalized base directory; rejects "..", absolute paths, or drive colons.
[[nodiscard]] constexpr bool ResolvePath(
    const FilePath& baseDir, std::string_view rel, FilePath& out) noexcept {
    out.Clear();
    if (baseDir.Empty() || rel.empty() || rel.contains('\0')) return false;
    if (rel.front() == '/' || rel.front() == '\\' || rel.contains(':')) return false;

    FilePath work = baseDir;
    usize i = 0U;
    while (i < rel.size()) {
        while (i < rel.size() && (rel[i] == '/' || rel[i] == '\\')) ++i;
        if (i >= rel.size()) break;
        const usize start = i;
        while (i < rel.size() && rel[i] != '/' && rel[i] != '\\') ++i;
        const std::string_view seg = rel.substr(start, i - start);
        if (seg == ".") continue;
        if (seg == "..") return false;
        if (!work.Empty() && work.View().back() != '/' && !work.Append("/")) return false;
        if (!work.Append(seg)) return false;
    }
    if (work.Size() <= baseDir.Size()) return false;
    out = work;
    return true;
}

// Cached 32-byte window geometry, DPI metrics, and virtual-canvas coordinate mappers.
struct WindowState {
    WindowSize logicalSize{1280, 720};
    WindowSize pixelSize{1280, 720};
    f32        pixelDensity{1.0f};
    f32        displayScale{1.0f};
    f32        refreshHz{60.0f};
    WindowMode mode{WindowMode::Windowed};
    bool       focused{true};
    bool       minimized{false};
    bool       relativeMouse{false};

    constexpr void ApplyLifecycleEvent(
        WindowFlags evFlag, WindowFlags& ioNetFlags, bool& outNeedRefresh) noexcept {
        if (HasFlag(evFlag, WindowFlags::Resized | WindowFlags::DisplayChanged)) {
            outNeedRefresh = true;
        }
        if (HasFlag(evFlag, WindowFlags::Minimized)) {
            minimized = true;
            focused   = false;
            ioNetFlags &= ~(WindowFlags::Restored | WindowFlags::FocusGained);
        }
        if (HasFlag(evFlag, WindowFlags::Restored)) {
            minimized      = false;
            outNeedRefresh = true;
            ioNetFlags &= ~WindowFlags::Minimized;
        }
        if (HasFlag(evFlag, WindowFlags::FocusLost)) {
            focused = false;
            ioNetFlags &= ~WindowFlags::FocusGained;
        }
        if (HasFlag(evFlag, WindowFlags::FocusGained)) {
            focused = true;
            ioNetFlags &= ~WindowFlags::FocusLost;
        }
        ioNetFlags |= evFlag;
    }

    constexpr WindowFlags ApplyMetrics(
        WindowSize newLogical, WindowSize newPixel, f32 newDensity, f32 newScale, f32 newHz) noexcept {
        WindowFlags changed = WindowFlags::None;
        if (newLogical.IsValid() && newLogical != logicalSize) {
            logicalSize = newLogical;
            changed |= WindowFlags::Resized;
        }
        if (newPixel.IsValid() && newPixel != pixelSize) {
            pixelSize = newPixel;
            changed |= WindowFlags::Resized;
        }
        const f32 density = newDensity > 0.0f ? newDensity : 1.0f;
        const f32 scale   = newScale > 0.0f ? newScale : density;
        const f32 hz      = newHz > 0.0f ? newHz : 60.0f;
        if (std::abs(density - pixelDensity) > 1e-3f || std::abs(scale - displayScale) > 1e-3f ||
            std::abs(hz - refreshHz) > 1e-2f) {
            changed |= WindowFlags::DisplayChanged;
        }
        pixelDensity = density;
        displayScale = scale;
        refreshHz    = hz;
        return changed;
    }

    [[nodiscard]] constexpr Vec2 PixelToWindow(Vec2 px) const noexcept {
        return pixelDensity > 0.0f ? (px * (1.0f / pixelDensity)) : px;
    }

    [[nodiscard]] constexpr Rect ComputeViewport(
        WindowSize virtualSize, ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        if (!pixelSize.IsValid() || !virtualSize.IsValid()) return {};
        const f32 pw = static_cast<f32>(pixelSize.w);
        const f32 ph = static_cast<f32>(pixelSize.h);
        if (scaleMode == ScaleMode::Stretch) return {0.0f, 0.0f, pw, ph};

        if (scaleMode == ScaleMode::IntegerScale) {
            if (const i32 intScale = std::min(pixelSize.w / virtualSize.w, pixelSize.h / virtualSize.h);
                intScale >= 1) {
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

        const f32 vw = static_cast<f32>(virtualSize.w);
        const f32 vh = static_cast<f32>(virtualSize.h);
        const f32 scale = (scaleMode == ScaleMode::Expand)
            ? std::max(pw / vw, ph / vh)
            : std::min(pw / vw, ph / vh);
        const f32 dw = static_cast<f32>(static_cast<i32>(vw * scale + 0.5f));
        const f32 dh = static_cast<f32>(static_cast<i32>(vh * scale + 0.5f));
        return {
            static_cast<f32>(static_cast<i32>((pw - dw) * 0.5f)),
            static_cast<f32>(static_cast<i32>((ph - dh) * 0.5f)),
            dw,
            dh,
        };
    }

    [[nodiscard]] constexpr Vec2 PixelToVirtual(
        Vec2 px,
        WindowSize virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox,
        bool clampToCanvas = true) const noexcept {
        const Rect vp = ComputeViewport(virtualSize, scaleMode);
        if (vp.IsEmpty()) return {};
        const f32 vw = static_cast<f32>(virtualSize.w);
        const f32 vh = static_cast<f32>(virtualSize.h);
        Vec2 out{(px.x - vp.x) * (vw / vp.w), (px.y - vp.y) * (vh / vp.h)};
        if (clampToCanvas) {
            out.x = std::clamp(out.x, 0.0f, vw);
            out.y = std::clamp(out.y, 0.0f, vh);
        }
        return out;
    }
};
static_assert(sizeof(WindowState) == 32);

enum class InputEventKind : u8 {
    Key           = 0,
    MouseButton   = 1,
    GamepadButton = 2,
    GamepadAxis   = 3,
};

// 4-byte packed platform input event: code[0..13] | (kind << 14), plus signed 16-bit value.
struct InputEvent {
    u16 packedHeader{0};
    i16 value{0};

    [[nodiscard]] static constexpr InputEvent Make(InputEventKind kind, u16 code, i16 val) noexcept {
        return {static_cast<u16>((code & 0x3FFFU) | (static_cast<u16>(kind) << 14U)), val};
    }
    [[nodiscard]] constexpr u16 Code() const noexcept { return packedHeader & 0x3FFFU; }
    [[nodiscard]] constexpr InputEventKind Kind() const noexcept {
        return static_cast<InputEventKind>((packedHeader >> 14U) & 0x3U);
    }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const InputEvent&) const noexcept = default;
};
static_assert(sizeof(InputEvent) == 4);

// 256-byte per-frame platform event batch with axis coalescing and release-drop protection.
struct FrameEvents {
    static constexpr usize kMaxTextBytes = 31U;
    static constexpr usize kMaxEvents    = 49U;

    f32                                  mouseX{0.0f};
    f32                                  mouseY{0.0f};
    f32                                  mouseDx{0.0f};
    f32                                  mouseDy{0.0f};
    f32                                  wheelX{0.0f};
    f32                                  wheelY{0.0f};
    u8                                   eventCount{0};
    u8                                   padDisconnected{0};
    u8                                   focused{1};
    u8                                   textLen{0};
    std::array<char, kMaxTextBytes + 1U> textInput{};
    std::array<InputEvent, kMaxEvents>   events{};

    constexpr void ClearFrameDeltas() noexcept {
        mouseDx         = 0.0f;
        mouseDy         = 0.0f;
        wheelX          = 0.0f;
        wheelY          = 0.0f;
        eventCount      = 0U;
        padDisconnected = 0U;
        focused         = 1U;
        textLen         = 0U;
        textInput[0]    = '\0';
    }

    constexpr void PushEvent(InputEventKind kind, u16 code, i16 value) noexcept {
        const i16 clamped = (kind == InputEventKind::GamepadAxis && value == -32768) ? i16{-32767} : value;
        const InputEvent ev = InputEvent::Make(kind, code, clamped);

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

        // Never drop a discrete button/key release when the queue is full:
        // 1) First scan the full queue to overwrite a prior press of the SAME key/button.
        // 2) Otherwise overwrite the latest GamepadAxis motion or non-release event.
        if (kind != InputEventKind::GamepadAxis && clamped == 0) {
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].packedHeader == ev.packedHeader) {
                    events[i - 1U].value = 0;
                    return;
                }
            }
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].Kind() == InputEventKind::GamepadAxis) {
                    events[i - 1U] = ev;
                    return;
                }
            }
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].value != 0) {
                    events[i - 1U] = ev;
                    return;
                }
            }
            events[kMaxEvents - 1U] = ev;
        }
    }

    constexpr void IngestKey(
        u16 scancode, bool down, bool repeat, bool winFocused, EventCapture cap) noexcept {
        if (down && repeat) return;
        if (!down || (winFocused && !HasFlag(cap, EventCapture::Keyboard))) {
            PushEvent(InputEventKind::Key, scancode, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestMouseButton(
        u16 btn, bool down, f32 x, f32 y, f32 density, bool winFocused, EventCapture cap) noexcept {
        const f32 d = density > 0.0f ? density : 1.0f;
        const bool accept = winFocused && !HasFlag(cap, EventCapture::Mouse);
        if (accept) {
            mouseX = x * d;
            mouseY = y * d;
        }
        if (!down || accept) {
            PushEvent(InputEventKind::MouseButton, btn, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestMouseMotion(
        f32 x, f32 y, f32 dx, f32 dy, f32 density, bool winFocused, EventCapture cap) noexcept {
        if (winFocused && !HasFlag(cap, EventCapture::Mouse)) {
            const f32 d = density > 0.0f ? density : 1.0f;
            mouseX  = x * d;
            mouseY  = y * d;
            mouseDx += dx * d;
            mouseDy += dy * d;
        }
    }

    constexpr void IngestMouseWheel(
        f32 wx, f32 wy, bool flipped, bool winFocused, EventCapture cap) noexcept {
        if (winFocused && !HasFlag(cap, EventCapture::Mouse)) {
            const f32 sign = flipped ? -1.0f : 1.0f;
            wheelX += wx * sign;
            wheelY += wy * sign;
        }
    }

    constexpr void IngestGamepadButton(
        u16 btn, bool down, bool winFocused, EventCapture cap) noexcept {
        if (!down || (winFocused && !HasFlag(cap, EventCapture::Gamepad))) {
            PushEvent(InputEventKind::GamepadButton, btn, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestGamepadAxis(
        u16 axis, i16 val, bool winFocused, EventCapture cap) noexcept {
        if (winFocused && !HasFlag(cap, EventCapture::Gamepad)) {
            PushEvent(InputEventKind::GamepadAxis, axis, val);
        }
    }

    constexpr void IngestGamepadDisconnect() noexcept { padDisconnected = 1U; }

    constexpr void IngestTextInput(std::string_view utf8, bool winFocused) noexcept {
        if (!winFocused || utf8.empty()) return;
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

    [[nodiscard]] constexpr std::span<const InputEvent> Events() const noexcept {
        return {events.data(), eventCount};
    }

    [[nodiscard]] constexpr std::string_view TextView() const noexcept {
        return {textInput.data(), textLen};
    }
};
static_assert(sizeof(FrameEvents) == 256);

} // namespace en

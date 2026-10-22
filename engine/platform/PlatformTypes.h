#pragma once

#include "core/Math.h"

#include <algorithm>
#include <array>

namespace en {

inline constexpr u8 kMaxGamepads = 4U;
inline constexpr u8 kInvalidGamepadSlot = 0xFFU;

using FilePath = FixedString<256>;
static_assert(sizeof(FilePath) == 256);

enum class PlatformErrc : u8 {
    None = 0,
    SdlInitFailed,
    WindowCreateFailed,
    FullscreenFailed,
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
        return (w > 0 && h > 0) ? (static_cast<f32>(w) / static_cast<f32>(h)) : 1.0f;
    }

    [[nodiscard]] constexpr bool operator==(const Extent2i&) const noexcept = default;
};
static_assert(sizeof(Extent2i) == 8);

enum class WindowMode : u8 {
    Windowed = 0,
    Borderless = 1,
    ExclusiveFullscreen = 2,
};

enum class GraphicsApi : u8 {
    Auto = 0,
    OpenGL = 1,
    Vulkan = 2,
    Metal = 3,
};

enum class ScaleMode : u8 {
    Stretch = 0,
    Letterbox = 1,
    IntegerScale = 2,
    Expand = 3,
};

enum class CursorShape : u8 {
    Arrow = 0,
    IBeam = 1,
    Crosshair = 2,
    Hand = 3,
    ResizeEW = 4,
    ResizeNS = 5,
    NotAllowed = 6,
};

enum class WindowFlashOp : u8 {
    Cancel = 0,
    Briefly = 1,
    UntilFocused = 2,
};

enum class PowerSupplyState : u8 {
    Unknown = 0,
    OnBattery = 1,
    NoBattery = 2,
    Charging = 3,
    Charged = 4,
};

// Compact 4-byte battery and thermal/power telemetry snapshot for handhelds and laptops.
// Value type; negative batteryPercent or remainingSeconds indicates unknown/unreported OS values.
// Invariant: sizeof(PowerInfo) == 4 with zero padding.
struct alignas(4) PowerInfo {
    i16 remainingSeconds{-1};
    i8 batteryPercent{-1};
    PowerSupplyState state{PowerSupplyState::Unknown};

    [[nodiscard]] constexpr bool IsLowBattery(i8 thresholdPercent = 20) const noexcept {
        return state == PowerSupplyState::OnBattery &&
               batteryPercent >= 0 &&
               batteryPercent <= thresholdPercent;
    }
};
static_assert(sizeof(PowerInfo) == 4);

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
    Extent2i minSize{640, 360};
    f32 targetRefreshHz{0.0f};
    WindowMode mode{WindowMode::Windowed};
    GraphicsApi api{GraphicsApi::Auto};
    bool resizable{true};
    bool highDpi{true};
};
static_assert(sizeof(WindowConfig) == 40);

// Authoritative window and display topology state placed at offset 0 of Window (32 bytes exact, zero padding).
// Value type; provides high-DPI coordinate transforms and 2D virtual canvas viewport scaling.
// Invariant: pixelDensity > 0.0f, displayScale > 0.0f, and refreshHz > 0.0f.
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

    [[nodiscard]] constexpr f32 AspectRatio() const noexcept {
        return pixelSize.AspectRatio();
    }

    [[nodiscard]] constexpr Vec2 WindowToPixel(Vec2 windowPos) const noexcept {
        return windowPos * pixelDensity;
    }

    [[nodiscard]] constexpr Vec2 PixelToWindow(Vec2 pixelPos) const noexcept {
        return pixelDensity > 0.0f ? (pixelPos / pixelDensity) : pixelPos;
    }

    [[nodiscard]] constexpr Vec2 PixelToUiLogical(Vec2 pixelPos) const noexcept {
        return displayScale > 0.0f ? (pixelPos / displayScale) : pixelPos;
    }

    [[nodiscard]] constexpr Vec2 UiLogicalToPixel(Vec2 uiPos) const noexcept {
        return uiPos * displayScale;
    }

    // Computes the destination presentation viewport in physical backbuffer pixels for a 2D virtual canvas.
    [[nodiscard]] constexpr Rect ComputeViewport(
        Extent2i virtualSize, ScaleMode scaleMode = ScaleMode::Letterbox) const noexcept {
        if (!pixelSize.IsValid() || !virtualSize.IsValid()) return {};
        const f32 pw = static_cast<f32>(pixelSize.w);
        const f32 ph = static_cast<f32>(pixelSize.h);
        if (scaleMode == ScaleMode::Stretch) {
            return {0.0f, 0.0f, pw, ph};
        }

        if (scaleMode == ScaleMode::IntegerScale) {
            const i32 intScale = std::min(pixelSize.w / virtualSize.w, pixelSize.h / virtualSize.h);
            if (intScale >= 1) {
                const f32 dstW = static_cast<f32>(virtualSize.w * intScale);
                const f32 dstH = static_cast<f32>(virtualSize.h * intScale);
                return {
                    static_cast<f32>((pixelSize.w - virtualSize.w * intScale) / 2),
                    static_cast<f32>((pixelSize.h - virtualSize.h * intScale) / 2),
                    dstW,
                    dstH,
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
        const f32 offX = static_cast<f32>(static_cast<i32>((pw - dstW) * 0.5f));
        const f32 offY = static_cast<f32>(static_cast<i32>((ph - dstH) * 0.5f));
        return {offX, offY, dstW, dstH};
    }

    // Unprojects physical backbuffer pixel coordinates (e.g. mouseX/Y) into 2D virtual canvas coordinates.
    [[nodiscard]] constexpr Vec2 PixelToVirtual(
        Vec2 pixelPos,
        Extent2i virtualSize,
        ScaleMode scaleMode = ScaleMode::Letterbox,
        bool clampToCanvas = true) const noexcept {
        const Rect vp = ComputeViewport(virtualSize, scaleMode);
        if (vp.IsEmpty()) return {};
        const f32 vw = static_cast<f32>(virtualSize.w);
        const f32 vh = static_cast<f32>(virtualSize.h);
        Vec2 virt{
            (pixelPos.x - vp.x) * (vw / vp.w),
            (pixelPos.y - vp.y) * (vh / vp.h),
        };
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
            ioNetFlags &= ~WindowFlags::Restored;
            ioNetFlags &= ~WindowFlags::FocusGained;
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

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 dims = (static_cast<u64>(static_cast<u32>(pixelSize.w)) << 32U) |
                         static_cast<u64>(static_cast<u32>(pixelSize.h));
        const u64 logDims = (static_cast<u64>(static_cast<u32>(logicalSize.w)) << 32U) |
                            static_cast<u64>(static_cast<u32>(logicalSize.h));
        const u64 scales = (static_cast<u64>(std::bit_cast<u32>(pixelDensity)) << 32U) |
                           static_cast<u64>(std::bit_cast<u32>(displayScale));
        const u64 stateWord = (static_cast<u64>(std::bit_cast<u32>(refreshHz)) << 32U) |
                              static_cast<u64>(mode) |
                              (focused ? 0x100ULL : 0ULL) |
                              (suspended ? 0x200ULL : 0ULL) |
                              (relativeMouse ? 0x400ULL : 0ULL);
        u64 h = Mix64(dims);
        h = HashCombine64(h, logDims);
        h = HashCombine64(h, scales);
        return HashCombine64(h, stateWord);
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

// Branchless 16-byte delegate allowing debug overlays or tools to dynamically report input capture masks.
// Non-owning function pointer and context; implicitly constructible from a static EventCapture bitmask.
// Invariant: fn is never nullptr (defaults to FixedMaskThunk<0U>), making Evaluate() a branchless call.
struct alignas(16) EventTap {
    using CallbackFn = EventCapture (*)(void* userCtx) noexcept;

    template <u8 MaskBits>
    [[nodiscard]] static constexpr EventCapture FixedMaskThunk(void*) noexcept {
        return static_cast<EventCapture>(MaskBits);
    }

    CallbackFn fn{&FixedMaskThunk<0U>};
    void* userCtx{nullptr};

    constexpr EventTap() noexcept = default;
    constexpr EventTap(CallbackFn callback, void* ctx = nullptr) noexcept
        : fn(callback ? callback : &FixedMaskThunk<0U>), userCtx(ctx) {}

    constexpr EventTap(EventCapture fixedMask) noexcept {
        switch (static_cast<u8>(fixedMask & EventCapture::All)) {
            case 0U: fn = &FixedMaskThunk<0U>; break;
            case 1U: fn = &FixedMaskThunk<1U>; break;
            case 2U: fn = &FixedMaskThunk<2U>; break;
            case 3U: fn = &FixedMaskThunk<3U>; break;
            case 4U: fn = &FixedMaskThunk<4U>; break;
            case 5U: fn = &FixedMaskThunk<5U>; break;
            case 6U: fn = &FixedMaskThunk<6U>; break;
            default: fn = &FixedMaskThunk<7U>; break;
        }
    }

    template <auto Method, typename T>
    [[nodiscard]] static constexpr EventTap Bind(T* instance) noexcept {
        return EventTap{
            [](void* ctx) noexcept -> EventCapture {
                return (static_cast<T*>(ctx)->*Method)();
            },
            instance};
    }

    [[nodiscard]] constexpr EventCapture Evaluate() const noexcept {
        return fn(userCtx);
    }
};
static_assert(sizeof(EventTap) == 16);

enum class InputEventKind : u8 {
    Key = 0,
    MouseButton = 1,
    GamepadButton = 2,
    GamepadAxis = 3,
};

// Packed 4-byte discrete physical input event record (16 events per 64-byte cache line, zero padding).
// Value type; bits [0..11] = device code, bits [12..13] = gamepad slot [0..3], bits [14..15] = InputEventKind.
// Invariant: GamepadAxis values are clamped to [-32767, 32767] for symmetric float normalization.
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
    [[nodiscard]] constexpr bool operator==(const PlatformInputEvent&) const noexcept = default;
};
static_assert(sizeof(PlatformInputEvent) == 4);

// Coalesced per-frame input and UTF-8 text batch occupying exactly 256 bytes (4 cache lines, zero padding).
// Cache Line 0 packs the 28-byte analog/status header, 32-byte UTF-8 text buffer, and first discrete event.
// Invariant: mouseX/Y/Dx/Dy are always in physical pixel space, and each (padSlot, axis) occupies at most 1 slot.
struct alignas(64) PlatformFrameEvents {
    static constexpr usize kMaxTextBytes = 31;
    static constexpr usize kMaxEvents = 49;

    f32 mouseX{0.0f};
    f32 mouseY{0.0f};
    f32 mouseDx{0.0f};
    f32 mouseDy{0.0f};
    f32 wheelX{0.0f};
    f32 wheelY{0.0f};
    u8 eventCount{0};
    u8 disconnectedPadMask{0};
    u8 focusCleared{0};
    u8 textLen{0};
    std::array<char, kMaxTextBytes + 1U> textInput{};
    std::array<PlatformInputEvent, kMaxEvents> events{};

    constexpr void ClearFrameDeltas() noexcept {
        mouseDx = 0.0f;
        mouseDy = 0.0f;
        wheelX = 0.0f;
        wheelY = 0.0f;
        eventCount = 0;
        disconnectedPadMask = 0;
        focusCleared = 0;
        textLen = 0;
        textInput[0] = '\0';
    }

    constexpr void ClearInput(bool markFocusLost = false) noexcept {
        ClearFrameDeltas();
        if (markFocusLost) focusCleared = 1U;
    }

    constexpr void PushEvent(InputEventKind kind, u8 padSlot, u16 code, i16 value) noexcept {
        // Clamp INT16_MIN (-32768) to -32767 so downstream division by 32767.0f stays strictly in [-1.0f, 1.0f].
        const i16 clampedVal = (kind == InputEventKind::GamepadAxis && value == -32768) ? i16{-32767} : value;
        const PlatformInputEvent ev = PlatformInputEvent::Make(kind, padSlot, code, clampedVal);

        if (kind == InputEventKind::GamepadAxis) {
            // Coalesce across interleaved multi-axis samples (e.g. X, Y, X, Y at 1000Hz) in-place.
            for (u8 i = 0U; i < eventCount; ++i) {
                if (events[i].packedHeader == ev.packedHeader) {
                    events[i].value = clampedVal;
                    return;
                }
            }
        } else {
            // Suppress redundant duplicate state edges (e.g. Down followed by Down without an intervening Up).
            for (u8 i = eventCount; i > 0U; --i) {
                if (events[i - 1U].packedHeader == ev.packedHeader) {
                    if (events[i - 1U].value == clampedVal) return;
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
            PushEvent(InputEventKind::Key, 0U, scancode, down ? i16{1} : i16{0});
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
        if (windowFocused && !HasFlag(capture, EventCapture::Mouse)) {
            mouseX = logicalX * d;
            mouseY = logicalY * d;
        }
        if (!down || (windowFocused && !HasFlag(capture, EventCapture::Mouse))) {
            PushEvent(InputEventKind::MouseButton, 0U, button, down ? i16{1} : i16{0});
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
        const usize avail = kMaxTextBytes - textLen;
        const usize toCopy = std::min<usize>(utf8.size(), avail);
        for (usize i = 0U; i < toCopy; ++i) {
            if (utf8[i] == '\0') break;
            textInput[textLen++] = utf8[i];
        }
        textInput[textLen] = '\0';
    }

    constexpr void IngestGamepadButton(
        u8 padSlot, u16 button, bool down, bool windowFocused, EventCapture capture) noexcept {
        if (padSlot >= kMaxGamepads) return;
        if (!down || (windowFocused && !HasFlag(capture, EventCapture::Gamepad))) {
            PushEvent(InputEventKind::GamepadButton, padSlot, button, down ? i16{1} : i16{0});
        }
    }

    constexpr void IngestGamepadAxis(
        u8 padSlot, u16 axis, i16 value, bool windowFocused, EventCapture capture) noexcept {
        if (padSlot >= kMaxGamepads) return;
        if (windowFocused && !HasFlag(capture, EventCapture::Gamepad)) {
            PushEvent(InputEventKind::GamepadAxis, padSlot, axis, value);
        }
    }

    // Marks the slot disconnected and strips any earlier same-frame events from that pad to prevent ghost latches.
    constexpr void IngestGamepadDisconnect(u8 padSlot) noexcept {
        if (padSlot >= kMaxGamepads) return;
        disconnectedPadMask |= static_cast<u8>(1U << padSlot);
        u8 writeIdx = 0U;
        for (u8 readIdx = 0U; readIdx < eventCount; ++readIdx) {
            const PlatformInputEvent ev = events[readIdx];
            const InputEventKind k = ev.Kind();
            const bool isFromPad =
                (k == InputEventKind::GamepadButton || k == InputEventKind::GamepadAxis) &&
                (ev.PadSlot() == padSlot);
            if (!isFromPad) {
                events[writeIdx++] = ev;
            }
        }
        eventCount = writeIdx;
    }

    [[nodiscard]] constexpr std::span<const PlatformInputEvent> Events() const noexcept {
        return std::span<const PlatformInputEvent>(events.data(), eventCount);
    }

    [[nodiscard]] constexpr std::string_view TextView() const noexcept {
        return std::string_view{textInput.data(), textLen};
    }

    [[nodiscard]] constexpr bool HasMouseWheel() const noexcept {
        return wheelX != 0.0f || wheelY != 0.0f;
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        const u64 hdrFlags = static_cast<u64>(eventCount) |
                             (static_cast<u64>(disconnectedPadMask) << 8U) |
                             (static_cast<u64>(focusCleared) << 16U) |
                             (static_cast<u64>(textLen) << 24U);
        u64 h = Mix64(hdrFlags);
        const u64 mousePosBits = (static_cast<u64>(std::bit_cast<u32>(mouseX)) << 32U) |
                                 static_cast<u64>(std::bit_cast<u32>(mouseY));
        const u64 mouseDeltaBits = (static_cast<u64>(std::bit_cast<u32>(mouseDx)) << 32U) |
                                   static_cast<u64>(std::bit_cast<u32>(mouseDy));
        const u64 wheelBits = (static_cast<u64>(std::bit_cast<u32>(wheelX)) << 32U) |
                              static_cast<u64>(std::bit_cast<u32>(wheelY));
        h = HashCombine64(h, mousePosBits);
        h = HashCombine64(h, mouseDeltaBits);
        h = HashCombine64(h, wheelBits);
        for (u8 i = 0U; i < textLen; ++i) {
            h = HashCombine64(h, static_cast<u8>(textInput[i]));
        }
        for (u8 i = 0U; i < eventCount; ++i) {
            const u32 word = static_cast<u32>(events[i].packedHeader) |
                             (static_cast<u32>(static_cast<u16>(events[i].value)) << 16U);
            h = HashCombine64(h, word);
        }
        return h;
    }
};
static_assert(sizeof(PlatformFrameEvents) == 256);

} // namespace en

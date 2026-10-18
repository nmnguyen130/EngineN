#pragma once

#include "core/Result.h"
#include "core/Types.h"

#include <array>
#include <bit>
#include <span>
#include <string_view>

namespace en::platform {

enum class PlatformErrc : u8 {
    None = 0,
    SdlInitFailed,
    WindowCreateFailed,
    FullscreenFailed,
    PathUnavailable,
    PathRejected,
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
    friend constexpr bool operator==(const Extent2i&, const Extent2i&) noexcept = default;
};
static_assert(sizeof(Extent2i) == 8);

enum class WindowMode : u8 {
    Windowed            = 0,
    Borderless          = 1,
    ExclusiveFullscreen = 2,
};

enum class GraphicsApi : u8 {
    None   = 0,
    OpenGL = 1,
    Vulkan = 2,
    Metal  = 3,
};

enum class WindowFlags : u8 {
    None           = 0,
    Resized        = 1u << 0,
    DisplayChanged = 1u << 1,
    FocusGained    = 1u << 2,
    FocusLost      = 1u << 3,
    CloseRequested = 1u << 4,
};

[[nodiscard]] constexpr WindowFlags operator|(WindowFlags a, WindowFlags b) noexcept {
    return static_cast<WindowFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr WindowFlags operator&(WindowFlags a, WindowFlags b) noexcept {
    return static_cast<WindowFlags>(static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr WindowFlags& operator|=(WindowFlags& a, WindowFlags b) noexcept { return a = a | b; }
[[nodiscard]] constexpr bool HasFlag(WindowFlags val, WindowFlags mask) noexcept {
    return (val & mask) != WindowFlags::None;
}

struct WindowConfig {
    std::string_view title{"EngineN"};
    Extent2i size{1280, 720};
    Extent2i minSize{640, 360};
    WindowMode mode{WindowMode::Windowed};
    GraphicsApi api{GraphicsApi::None};
    bool resizable{true};
    bool highDpi{true};
};

namespace detail {

[[nodiscard]] constexpr u64 MixAuditBits(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

[[nodiscard]] constexpr u32 CanonF32Bits(f32 v) noexcept {
    const u32 bits = std::bit_cast<u32>(v);
    return (bits == 0x80000000u) ? 0u : bits;
}

} // namespace detail

// Synchronous window geometry, high-DPI scaling, and lifecycle snapshot.
// Embedded at offset 0 of Window (first 32B of Cache Line 0) with zero padding.
// Invariant: pixelDensity > 0.0f, displayScale > 0.0f, and refreshHz > 0.0f.
struct alignas(32) WindowState {
    Extent2i logicalSize{0, 0};
    Extent2i pixelSize{0, 0};
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

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = detail::MixAuditBits(std::bit_cast<u64>(logicalSize));
        h ^= detail::MixAuditBits(std::bit_cast<u64>(pixelSize));
        h ^= detail::MixAuditBits(
            (static_cast<u64>(detail::CanonF32Bits(pixelDensity)) << 32) |
            detail::CanonF32Bits(displayScale));
        const u64 meta =
            (static_cast<u64>(detail::CanonF32Bits(refreshHz)) << 32) |
            (static_cast<u64>(mode) << 24) |
            (static_cast<u64>(focused) << 16) |
            (static_cast<u64>(suspended) << 8) |
            static_cast<u64>(relativeMouse);
        return detail::MixAuditBits(h ^ meta);
    }
};
static_assert(sizeof(WindowState) == 32);

// Bitmask returned by EventTap to block initiating/continuous events while allowing
// release edges through to guarantee zero stuck keys or buttons.
enum class EventCapture : u8 {
    None     = 0,
    Keyboard = 1u << 0,
    Mouse    = 1u << 1,
    Gamepad  = 1u << 2,
};

[[nodiscard]] constexpr EventCapture operator|(EventCapture a, EventCapture b) noexcept {
    return static_cast<EventCapture>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr EventCapture operator&(EventCapture a, EventCapture b) noexcept {
    return static_cast<EventCapture>(static_cast<u8>(a) & static_cast<u8>(b));
}
[[nodiscard]] constexpr bool HasCapture(EventCapture val, EventCapture mask) noexcept {
    return (val & mask) != EventCapture::None;
}

enum class InputEventKind : u8 {
    Key           = 0,
    MouseButton   = 1,
    GamepadButton = 2,
    GamepadAxis   = 3,
};

// 4-byte packed discrete input transition (16 events per 64B L1 cache line).
// Layout of packedHeader: bits [0..11] = code, bits [12..13] = padSlot (0..3), bits [14..15] = InputEventKind.
struct alignas(4) PlatformInputEvent {
    u16 packedHeader{0};
    i16 value{0};

    [[nodiscard]] static constexpr PlatformInputEvent Make(
        InputEventKind kind, u8 padSlot, u16 code, i16 val) noexcept
    {
        return PlatformInputEvent{
            .packedHeader = static_cast<u16>(
                (code & 0x0FFFu) |
                (static_cast<u16>(padSlot & 0x03u) << 12) |
                (static_cast<u16>(kind) << 14)),
            .value = val
        };
    }

    [[nodiscard]] constexpr u16 Code() const noexcept { return packedHeader & 0x0FFFu; }
    [[nodiscard]] constexpr u8 PadSlot() const noexcept { return static_cast<u8>((packedHeader >> 12) & 0x03u); }
    [[nodiscard]] constexpr InputEventKind Kind() const noexcept { return static_cast<InputEventKind>(packedHeader >> 14); }
    [[nodiscard]] constexpr bool Down() const noexcept { return value != 0; }
};
static_assert(sizeof(PlatformInputEvent) == 4);

inline constexpr usize kMaxGamepads = 4;
inline constexpr usize kMaxFrameInputEvents = 57;

// Coalesced 256-byte (4 cache lines, zero padding) per-frame OS input batch produced by Platform::Pump().
// Cache Line 0 (offset 0..63) packs the 28B header AND the first 9 discrete events for single-line frame reads.
// Invariant: eventCount <= kMaxFrameInputEvents.
struct alignas(64) PlatformFrameEvents {
    f32 mouseX{0.0f};
    f32 mouseY{0.0f};
    f32 mouseDx{0.0f};
    f32 mouseDy{0.0f};
    f32 wheelX{0.0f};
    f32 wheelY{0.0f};
    u8 eventCount{0};
    u8 disconnectedPadMask{0};
    bool focusCleared{false};
    bool hasMouseMotion{false};
    std::array<PlatformInputEvent, kMaxFrameInputEvents> events{};

    constexpr void ClearInput(bool markFocusLost = false) noexcept {
        mouseX = 0.0f;
        mouseY = 0.0f;
        mouseDx = 0.0f;
        mouseDy = 0.0f;
        wheelX = 0.0f;
        wheelY = 0.0f;
        eventCount = 0;
        if (!markFocusLost) {
            disconnectedPadMask = 0;
        }
        focusCleared = markFocusLost;
        hasMouseMotion = false;
    }

    constexpr void PushEvent(InputEventKind kind, u8 padSlot, u16 code, i16 value) noexcept {
        const auto ev = PlatformInputEvent::Make(kind, padSlot, code, value);
        if (kind == InputEventKind::GamepadAxis && eventCount > 0 &&
            events[eventCount - 1].packedHeader == ev.packedHeader) {
            events[eventCount - 1].value = value;
            return;
        }
        if (eventCount < kMaxFrameInputEvents) {
            events[eventCount++] = ev;
        }
    }

    [[nodiscard]] constexpr std::span<const PlatformInputEvent> Events() const noexcept {
        return {events.data(), eventCount};
    }

    [[nodiscard]] constexpr bool HasMouseWheel() const noexcept {
        return wheelX != 0.0f || wheelY != 0.0f;
    }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = detail::MixAuditBits(
            (static_cast<u64>(detail::CanonF32Bits(mouseX)) << 32) |
            detail::CanonF32Bits(mouseY));
        h ^= detail::MixAuditBits(
            (static_cast<u64>(detail::CanonF32Bits(mouseDx)) << 32) |
            detail::CanonF32Bits(mouseDy));
        h ^= detail::MixAuditBits(
            (static_cast<u64>(detail::CanonF32Bits(wheelX)) << 32) |
            detail::CanonF32Bits(wheelY));
        const u64 meta =
            (static_cast<u64>(eventCount) << 24) |
            (static_cast<u64>(disconnectedPadMask) << 16) |
            (static_cast<u64>(focusCleared) << 8) |
            static_cast<u64>(hasMouseMotion);
        h = detail::MixAuditBits(h ^ meta);
        for (u8 i = 0; i < eventCount; ++i) {
            h = detail::MixAuditBits(h ^ std::bit_cast<u32>(events[i]));
        }
        return h;
    }
};
static_assert(sizeof(PlatformFrameEvents) == 256);

// Fixed-capacity NUL-terminated string buffer with exact N-byte footprint (zero cache-line tail spill).
// Places m_len at offset 0 alongside m_buf[0..61] in Cache Line 0 for single-line short string reads.
// Invariant: m_buf[m_len] == '\0' and m_len <= kMaxLength.
template <usize N>
class alignas(N >= 64 ? 64 : alignof(u16)) FixedString {
    static_assert(N >= 4 && N <= 65536);

public:
    static constexpr usize kMaxLength = N - sizeof(u16) - 1;

    constexpr FixedString() noexcept = default;
    constexpr explicit FixedString(std::string_view s) noexcept { (void)Assign(s); }

    constexpr bool Assign(std::string_view s) noexcept {
        const usize n = s.size() < kMaxLength ? s.size() : kMaxLength;
        for (usize i = 0; i < n; ++i) {
            m_buf[i] = s[i];
        }
        m_buf[n] = '\0';
        m_len = static_cast<u16>(n);
        return n == s.size();
    }

    // Normalizes directory separators ('\\' -> '/') and guarantees a trailing '/' once at Init().
    constexpr bool AssignNormalizedDir(std::string_view dir) noexcept {
        usize len = 0;
        for (char c : dir) {
            if (c == '\0') break;
            if (len >= kMaxLength) {
                m_buf[0] = '\0';
                m_len = 0;
                return false;
            }
            const char norm = (c == '\\') ? '/' : c;
            if (norm == '/' && len > 0 && m_buf[len - 1] == '/') continue;
            m_buf[len++] = norm;
        }
        if (len > 0 && m_buf[len - 1] != '/') {
            if (len >= kMaxLength) {
                m_buf[0] = '\0';
                m_len = 0;
                return false;
            }
            m_buf[len++] = '/';
        }
        m_buf[len] = '\0';
        m_len = static_cast<u16>(len);
        return true;
    }

    // Validates and appends a relative path in a single pass onto an already-normalized directory root.
    [[nodiscard]] constexpr Result<FixedString, PlatformErrc> Resolve(std::string_view rel) const noexcept {
        if (rel.empty() || m_len + rel.size() > kMaxLength) {
            return Err(rel.empty() ? PlatformErrc::PathRejected : PlatformErrc::PathUnavailable);
        }

        FixedString out{};
        for (usize i = 0; i < m_len; ++i) {
            out.m_buf[i] = m_buf[i];
        }

        usize len = m_len;
        usize segLen = 0;
        usize dotCount = 0;
        for (char c : rel) {
            if (c == '/' || c == '\\') {
                if (segLen == 0 || (segLen == dotCount && dotCount <= 2)) {
                    return Err(PlatformErrc::PathRejected);
                }
                out.m_buf[len++] = '/';
                segLen = 0;
                dotCount = 0;
            } else {
                if (static_cast<u8>(c) < 0x20u || c == ':') {
                    return Err(PlatformErrc::PathRejected);
                }
                if (c == '.') ++dotCount;
                ++segLen;
                out.m_buf[len++] = c;
            }
        }

        if (segLen == 0 || (segLen == dotCount && dotCount <= 2)) {
            return Err(PlatformErrc::PathRejected);
        }

        out.m_buf[len] = '\0';
        out.m_len = static_cast<u16>(len);
        return out;
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept { return {m_buf.data(), m_len}; }
    [[nodiscard]] constexpr const char* CStr() const noexcept { return m_buf.data(); }
    [[nodiscard]] constexpr usize Size() const noexcept { return m_len; }
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_len == 0; }
    [[nodiscard]] static consteval usize Capacity() noexcept { return kMaxLength; }

    constexpr operator std::string_view() const noexcept { return View(); }
    [[nodiscard]] constexpr bool operator==(std::string_view other) const noexcept { return View() == other; }

private:
    u16 m_len{0};
    std::array<char, N - sizeof(u16)> m_buf{};
};

inline constexpr usize kMaxPathBytes = 1024;
using FilePath = FixedString<kMaxPathBytes>;
static_assert(sizeof(FilePath) == kMaxPathBytes);
static_assert(alignof(FilePath) == 64);

} // namespace en::platform

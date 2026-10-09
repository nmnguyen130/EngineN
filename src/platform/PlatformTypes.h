#pragma once

#include "core/Types.h"
#include <string>
#include <string_view>

union SDL_Event;

namespace EngineN {

// ============================================================================
// Clipboard RAII Safe Wrapper
// ============================================================================

/// RAII wrapper for SDL-allocated clipboard text ensuring zero memory leaks.
/// SDL_GetClipboardText() allocates memory with SDL_malloc, which MUST be freed with SDL_free().
/// This wrapper guarantees RAII cleanup upon destruction or reassignment.
class ClipboardText {
public:
    ClipboardText() noexcept = default;
    explicit ClipboardText(char* text) noexcept : m_text(text) {}
    ~ClipboardText();

    ClipboardText(const ClipboardText&) = delete;
    ClipboardText& operator=(const ClipboardText&) = delete;

    ClipboardText(ClipboardText&& other) noexcept : m_text(other.m_text) {
        other.m_text = nullptr;
    }

    ClipboardText& operator=(ClipboardText&& other) noexcept {
        if (this != &other) {
            Reset(other.m_text);
            other.m_text = nullptr;
        }
        return *this;
    }

    void Reset(char* text = nullptr) noexcept;

    [[nodiscard]] char* Release() noexcept {
        char* tmp = m_text;
        m_text = nullptr;
        return tmp;
    }

    [[nodiscard]] const char* c_str() const noexcept { return m_text ? m_text : ""; }
    [[nodiscard]] const char* data() const noexcept { return c_str(); }
    [[nodiscard]] std::string_view view() const noexcept { return std::string_view(c_str()); }
    [[nodiscard]] usize length() const noexcept { return view().length(); }
    [[nodiscard]] usize size() const noexcept { return view().size(); }
    [[nodiscard]] bool empty() const noexcept { return !m_text || m_text[0] == '\0'; }
    [[nodiscard]] explicit operator bool() const noexcept { return !empty(); }
    [[nodiscard]] operator std::string_view() const noexcept { return view(); }

    [[nodiscard]] bool operator==(std::string_view other) const noexcept { return view() == other; }
    [[nodiscard]] bool operator==(const char* other) const noexcept {
        return other ? (view() == other) : empty();
    }

private:
    char* m_text = nullptr;
};

// ============================================================================
// Event Routing & Listener Abstractions
// ============================================================================

/// Event routing category bitmask for decoupled event pump filtering.
enum class EventCategory : u32 {
    None      = 0,
    Window    = 1 << 0,
    Keyboard  = 1 << 1,
    Mouse     = 1 << 2,
    Gamepad   = 1 << 3,
    Input     = (1 << 1) | (1 << 2) | (1 << 3),
    Audio     = 1 << 4,
    System    = 1 << 5, // Quit, Terminating, LowMemory
    All       = 0xFFFFFFFF
};

constexpr EventCategory operator|(EventCategory a, EventCategory b) noexcept {
    return static_cast<EventCategory>(static_cast<u32>(a) | static_cast<u32>(b));
}

constexpr EventCategory operator&(EventCategory a, EventCategory b) noexcept {
    return static_cast<EventCategory>(static_cast<u32>(a) & static_cast<u32>(b));
}

constexpr EventCategory operator~(EventCategory a) noexcept {
    return static_cast<EventCategory>(~static_cast<u32>(a));
}

/// Modular event listener interface for decoupled platform event routing.
class IEventListener {
public:
    virtual ~IEventListener() = default;
    virtual void ProcessEvent(const SDL_Event& event) = 0;
};

// ============================================================================
// Display Information & Window Modes
// ============================================================================

/// Display monitor metadata with memory-safe owned string and native High-DPI scale.
struct DisplayInfo {
    u32 id = 0;              // SDL_DisplayID
    i32 width = 0;           // Native resolution width
    i32 height = 0;          // Native resolution height
    f32 refreshRate = 0.0f;  // Hz (e.g. 60.0, 120.0, 144.0, 240.0)
    f32 contentScale = 1.0f; // Native SDL3 content scale factor (1.0 = 100%, 2.0 = 200%)
    f32 hdpi = 96.0f;        // Horizontal DPI (contentScale * 96.0)
    f32 vdpi = 96.0f;        // Vertical DPI (contentScale * 96.0)
    std::string name;        // Display name (memory-safe owned string)
};

/// Window display modes compliant with SDL3 display switching architecture.
enum class WindowMode : u8 {
    Windowed = 0,
    BorderlessFullscreen,    // Desktop borderless fullscreen (mode = NULL)
    ExclusiveFullscreen      // Exclusive fullscreen (display mode switch via SDL_SetWindowFullscreenMode)
};

// ============================================================================
// Power and Battery States
// ============================================================================

enum class PowerState : u8 {
    Unknown = 0,
    OnBattery,
    NoBattery,
    Charging,
    Charged
};

struct PowerInfo {
    PowerState state = PowerState::Unknown;
    i32 batteryPercent = -1;  // -1 if unknown
    i32 secondsLeft = -1;     // -1 if unknown
};

} // namespace EngineN

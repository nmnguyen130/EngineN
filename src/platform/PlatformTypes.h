#pragma once

#include "core/Types.h"
#include <string_view>

namespace EngineN {

// Display information
struct DisplayInfo {
    u32 id = 0;              // SDL_DisplayID
    i32 width = 0;           // Native resolution width
    i32 height = 0;          // Native resolution height
    f32 refreshRate = 0.0f;  // Hz (e.g. 60.0, 120.0, 144.0, 240.0)
    f32 hdpi = 0.0f;         // Horizontal DPI
    f32 vdpi = 0.0f;         // Vertical DPI
    std::string_view name;   // Display name
};

// Window display modes
enum class WindowMode : u8 {
    Windowed = 0,
    BorderlessFullscreen,    // Desktop fullscreen (borderless)
    ExclusiveFullscreen      // Exclusive fullscreen (display mode switch)
};

// Power and battery states
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

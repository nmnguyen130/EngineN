#include "platform/PlatformTypes.h"
#include <SDL3/SDL_error.h>

namespace EngineN {

EngineError MakePlatformError(PlatformErrc code, std::string_view detail) noexcept {
    if (!detail.empty()) {
        return EngineError(detail, static_cast<i32>(code));
    }

    const char* sdlErr = SDL_GetError();
    if (sdlErr && sdlErr[0] != '\0') {
        return EngineError(sdlErr, static_cast<i32>(code));
    }

    const char* msg = "Unknown platform error";
    switch (code) {
        case PlatformErrc::SdlInitFailed:      msg = "SDL initialization failed"; break;
        case PlatformErrc::WindowCreateFailed: msg = "Window creation failed"; break;
        case PlatformErrc::DisplayQueryFailed: msg = "Display query failed"; break;
        case PlatformErrc::FullscreenFailed:   msg = "Fullscreen mode transition failed"; break;
        case PlatformErrc::PathUnavailable:    msg = "Platform path unavailable"; break;
        case PlatformErrc::PathRejected:       msg = "Path violates sandbox constraints"; break;
        case PlatformErrc::ClipboardFailed:    msg = "Clipboard operation failed"; break;
        case PlatformErrc::DeviceOpenFailed:   msg = "Device open failed"; break;
        case PlatformErrc::CapacityExceeded:   msg = "Platform buffer capacity exceeded"; break;
        case PlatformErrc::InvalidState:       msg = "Invalid platform state"; break;
    }

    return EngineError(msg, static_cast<i32>(code));
}

} // namespace EngineN

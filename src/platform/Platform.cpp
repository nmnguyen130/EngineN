#include "platform/Platform.h"
#include "core/Log.h"
#include <SDL3/SDL.h>
#include <algorithm>

namespace EngineN {

Platform::~Platform() {
    Shutdown();
}

Result<void> Platform::Init() {
    if (m_initialized) {
        return Ok;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ENGINE_LOG(Error, Platform, "SDL_Init failed: {}", SDL_GetError());
        return Err(EngineError(SDL_GetError()));
    }

    m_initialized = true;
    ENGINE_LOG(Info, Platform, "Platform initialized successfully (VIDEO | GAMEPAD)");
    return Ok;
}

void Platform::Shutdown() {
    if (!m_initialized) {
        return;
    }

    SDL_Quit();
    m_initialized = false;
    ENGINE_LOG(Info, Platform, "Platform shutdown complete");
}

i32 Platform::GetDisplayCount() const {
    if (!m_initialized) return 0;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (displays) {
        SDL_free(displays);
    }
    return count;
}

i32 Platform::QueryDisplays(std::span<DisplayInfo> outDisplays) const {
    if (!m_initialized || outDisplays.empty()) return 0;

    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || count <= 0) {
        if (displays) SDL_free(displays);
        return 0;
    }

    int toWrite = std::min(count, static_cast<int>(outDisplays.size()));
    for (int i = 0; i < toWrite; ++i) {
        SDL_DisplayID id = displays[i];
        DisplayInfo info{};
        info.id = id;

        const char* name = SDL_GetDisplayName(id);
        info.name = name ? name : "Unknown Display";

        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
        if (mode) {
            info.width = mode->w;
            info.height = mode->h;
            info.refreshRate = mode->refresh_rate;
        }

        // DPI queries
        info.hdpi = 96.0f;
        info.vdpi = 96.0f;

        outDisplays[i] = info;
    }

    SDL_free(displays);
    return toWrite;
}

DisplayInfo Platform::GetPrimaryDisplayInfo() const {
    DisplayInfo info{};
    if (!m_initialized) return info;

    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    info.id = id;

    const char* name = SDL_GetDisplayName(id);
    info.name = name ? name : "Primary Display";

    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
    if (mode) {
        info.width = mode->w;
        info.height = mode->h;
        info.refreshRate = mode->refresh_rate;
    }
    info.hdpi = 96.0f;
    info.vdpi = 96.0f;

    return info;
}

bool Platform::HasClipboardText() const {
    return SDL_HasClipboardText();
}

const char* Platform::GetClipboardText() const {
    return SDL_GetClipboardText();
}

bool Platform::SetClipboardText(const char* text) {
    return SDL_SetClipboardText(text);
}

u64 Platform::GetPerformanceCounter() const noexcept {
    return SDL_GetPerformanceCounter();
}

u64 Platform::GetPerformanceFrequency() const noexcept {
    return SDL_GetPerformanceFrequency();
}

PowerInfo Platform::GetPowerInfo() const {
    PowerInfo info{};
    int seconds = -1;
    int percent = -1;
    SDL_PowerState state = SDL_GetPowerInfo(&seconds, &percent);

    info.secondsLeft = seconds;
    info.batteryPercent = percent;

    switch (state) {
        case SDL_POWERSTATE_ON_BATTERY: info.state = PowerState::OnBattery; break;
        case SDL_POWERSTATE_NO_BATTERY: info.state = PowerState::NoBattery; break;
        case SDL_POWERSTATE_CHARGING:   info.state = PowerState::Charging; break;
        case SDL_POWERSTATE_CHARGED:    info.state = PowerState::Charged; break;
        default:                        info.state = PowerState::Unknown; break;
    }

    return info;
}

i32 Platform::GetSystemRAM() const noexcept {
    return SDL_GetSystemRAM();
}

i32 Platform::GetCPUCount() const noexcept {
    return SDL_GetNumLogicalCPUCores();
}

} // namespace EngineN

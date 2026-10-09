#include "platform/Platform.h"
#include "core/Log.h"
#include <SDL3/SDL.h>
#include <algorithm>

namespace EngineN {

// ============================================================================
// ClipboardText RAII Implementation
// ============================================================================

ClipboardText::~ClipboardText() {
    Reset();
}

void ClipboardText::Reset(char* text) noexcept {
    if (m_text) {
        SDL_free(m_text);
    }
    m_text = text;
}

// ============================================================================
// Display Helper Functions
// ============================================================================

static DisplayInfo ExtractDisplayInfo(SDL_DisplayID id) {
    DisplayInfo info{};
    info.id = id;

    const char* name = SDL_GetDisplayName(id);
    info.name = (name && name[0] != '\0') ? name : "Unknown Display";

    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
    if (mode) {
        info.width = mode->w;
        info.height = mode->h;
        info.refreshRate = mode->refresh_rate;
    }

    float scale = SDL_GetDisplayContentScale(id);
    info.contentScale = (scale > 0.0f) ? scale : 1.0f;
    info.hdpi = 96.0f * info.contentScale;
    info.vdpi = 96.0f * info.contentScale;

    return info;
}

// ============================================================================
// Platform Subsystem Implementation
// ============================================================================

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
        outDisplays[i] = ExtractDisplayInfo(displays[i]);
    }

    SDL_free(displays);
    return toWrite;
}

std::vector<DisplayInfo> Platform::GetAllDisplays() const {
    std::vector<DisplayInfo> result;
    if (!m_initialized) return result;

    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || count <= 0) {
        if (displays) SDL_free(displays);
        return result;
    }

    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        result.push_back(ExtractDisplayInfo(displays[i]));
    }

    SDL_free(displays);
    return result;
}

DisplayInfo Platform::GetPrimaryDisplayInfo() const {
    if (!m_initialized) return DisplayInfo{};
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    return ExtractDisplayInfo(id);
}

DisplayInfo Platform::GetDisplayInfo(u32 displayId) const {
    if (!m_initialized) return DisplayInfo{};
    return ExtractDisplayInfo(displayId);
}

f32 Platform::GetDisplayRefreshRate(u32 displayId) const {
    if (!m_initialized) return 60.0f;
    SDL_DisplayID id = (displayId == 0) ? SDL_GetPrimaryDisplay() : displayId;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    if (!mode) {
        mode = SDL_GetDesktopDisplayMode(id);
    }
    return (mode && mode->refresh_rate > 0.0f) ? mode->refresh_rate : 60.0f;
}

f32 Platform::GetDisplayContentScale(u32 displayId) const {
    if (!m_initialized) return 1.0f;
    SDL_DisplayID id = (displayId == 0) ? SDL_GetPrimaryDisplay() : displayId;
    float scale = SDL_GetDisplayContentScale(id);
    return (scale > 0.0f) ? scale : 1.0f;
}

bool Platform::HasClipboardText() const {
    return SDL_HasClipboardText();
}

ClipboardText Platform::GetClipboardText() const {
    // SDL_GetClipboardText() allocates memory via SDL_malloc that must be freed with SDL_free().
    // ClipboardText takes ownership and guarantees zero memory leaks via RAII.
    return ClipboardText(SDL_GetClipboardText());
}

std::string Platform::GetClipboardString() const {
    ClipboardText clip = GetClipboardText();
    return std::string(clip.view());
}

bool Platform::SetClipboardText(const char* text) {
    return SDL_SetClipboardText(text ? text : "");
}

bool Platform::SetClipboardText(std::string_view text) {
    std::string str(text);
    return SDL_SetClipboardText(str.c_str());
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

i32 Platform::GetCPUCacheLineSize() const noexcept {
    return SDL_GetCPUCacheLineSize();
}

} // namespace EngineN

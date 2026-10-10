#include "platform/Display.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cmath>

namespace EngineN {

namespace {

DisplayOrientation MapOrientation(SDL_DisplayOrientation orient) noexcept {
    switch (orient) {
        case SDL_ORIENTATION_LANDSCAPE:         return DisplayOrientation::Landscape;
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return DisplayOrientation::LandscapeFlipped;
        case SDL_ORIENTATION_PORTRAIT:          return DisplayOrientation::Portrait;
        case SDL_ORIENTATION_PORTRAIT_FLIPPED:  return DisplayOrientation::PortraitFlipped;
        default:                                return DisplayOrientation::Unknown;
    }
}

} // namespace

Result<void, EngineError> DisplayTopology::Refresh() noexcept {
    int count = 0;
    SDL_DisplayID* displayIds = SDL_GetDisplays(&count);
    if (!displayIds || count <= 0) {
        if (displayIds) SDL_free(displayIds);
        return Unexpected(MakePlatformError(PlatformErrc::DisplayQueryFailed, "Failed to query displays from SDL3"));
    }

    m_displays.Clear();
    const SDL_DisplayID primaryId = SDL_GetPrimaryDisplay();

    for (int i = 0; i < count && !m_displays.Full(); ++i) {
        const SDL_DisplayID id = displayIds[i];
        DisplayInfo info{};
        info.id = DisplayId{static_cast<u32>(id)};
        info.isPrimary = (id == primaryId);

        const char* name = SDL_GetDisplayName(id);
        if (name) {
            info.name.Assign(name);
        }

        SDL_Rect rect{};
        if (SDL_GetDisplayBounds(id, &rect)) {
            info.bounds = IRect{rect.x, rect.y, rect.w, rect.h};
        }
        if (SDL_GetDisplayUsableBounds(id, &rect)) {
            info.usableBounds = IRect{rect.x, rect.y, rect.w, rect.h};
        }

        info.contentScale = SDL_GetDisplayContentScale(id);
        if (info.contentScale <= 0.0f) {
            info.contentScale = 1.0f;
        }

        info.naturalOrientation = MapOrientation(SDL_GetNaturalDisplayOrientation(id));
        info.currentOrientation = MapOrientation(SDL_GetCurrentDisplayOrientation(id));

        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(id);
        if (desktop) {
            info.desktopMode.width = desktop->w;
            info.desktopMode.height = desktop->h;
            info.desktopMode.pixelDensity = desktop->pixel_density;
            info.desktopMode.refreshHz = desktop->refresh_rate;
            info.desktopMode.refreshNumerator = static_cast<u32>(desktop->refresh_rate_numerator);
            info.desktopMode.refreshDenominator = static_cast<u32>(desktop->refresh_rate_denominator);
            info.desktopMode.sdlPixelFormat = desktop->format;
        }

        int modeCount = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &modeCount);
        if (modes) {
            for (int m = 0; m < modeCount && !info.fullscreenModes.Full(); ++m) {
                const SDL_DisplayMode* sm = modes[m];
                if (!sm) continue;

                DisplayMode dm{
                    .width = sm->w,
                    .height = sm->h,
                    .pixelDensity = sm->pixel_density,
                    .refreshHz = sm->refresh_rate,
                    .refreshNumerator = static_cast<u32>(sm->refresh_rate_numerator),
                    .refreshDenominator = static_cast<u32>(sm->refresh_rate_denominator),
                    .sdlPixelFormat = sm->format
                };

                bool exists = false;
                for (const auto& existing : info.fullscreenModes.View()) {
                    if (existing == dm) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    (void)info.fullscreenModes.TryPush(dm);
                }
            }
            SDL_free(modes);
        }

        (void)m_displays.TryPush(info);
    }

    SDL_free(displayIds);
    ++m_revision;
    return Ok;
}

const DisplayInfo* DisplayTopology::Find(DisplayId id) const noexcept {
    for (const auto& d : m_displays.View()) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

const DisplayInfo* DisplayTopology::Primary() const noexcept {
    for (const auto& d : m_displays.View()) {
        if (d.isPrimary) return &d;
    }
    return m_displays.Empty() ? nullptr : &m_displays[0];
}

f32 DisplayTopology::MaxRefreshHz() const noexcept {
    f32 maxHz = 60.0f;
    for (const auto& d : m_displays.View()) {
        if (d.desktopMode.refreshHz > maxHz) {
            maxHz = d.desktopMode.refreshHz;
        }
    }
    return maxHz;
}

std::optional<DisplayMode> DisplayTopology::FindClosestMode(
    DisplayId display,
    const ModeQuery& query) const noexcept
{
    const DisplayInfo* info = Find(display);
    if (!info) {
        info = Primary();
    }
    if (!info || info->fullscreenModes.Empty()) {
        return std::nullopt;
    }

    const i32 targetW = (query.width > 0) ? query.width : info->desktopMode.width;
    const i32 targetH = (query.height > 0) ? query.height : info->desktopMode.height;
    const f32 targetHz = (query.refreshHz > 0.0f) ? query.refreshHz : 0.0f;

    const DisplayMode* best = nullptr;
    f32 bestScore = 1e9f;

    for (const auto& mode : info->fullscreenModes.View()) {
        if (!query.includeHighDensityModes && mode.pixelDensity > 1.0f) {
            continue;
        }

        const f32 resDiff = static_cast<f32>(std::abs(mode.width - targetW) + std::abs(mode.height - targetH));
        const f32 hzDiff = (targetHz > 0.0f) ? std::abs(mode.refreshHz - targetHz) : -mode.refreshHz;
        const f32 score = resDiff * 100.0f + hzDiff;

        if (score < bestScore) {
            bestScore = score;
            best = &mode;
        }
    }

    if (best) {
        return *best;
    }
    return info->desktopMode;
}

} // namespace EngineN

#pragma once

#include "platform/PlatformTypes.h"
#include <optional>

namespace EngineN {

enum class DisplayOrientation : u8 {
    Unknown,
    Landscape,
    LandscapeFlipped,
    Portrait,
    PortraitFlipped
};

struct DisplayMode {
    i32 width{0};
    i32 height{0};
    f32 pixelDensity{1.0f};
    f32 refreshHz{0.0f};
    u32 refreshNumerator{0};
    u32 refreshDenominator{0};
    u32 sdlPixelFormat{0};

    friend constexpr bool operator==(const DisplayMode&, const DisplayMode&) noexcept = default;
};

struct ModeQuery {
    i32 width{0};                    // 0 = native desktop resolution
    i32 height{0};
    f32 refreshHz{0.0f};             // 0 = highest available
    bool includeHighDensityModes{false};
};

inline constexpr usize kMaxDisplays = 8;
inline constexpr usize kMaxModesPerDisplay = 48;

struct DisplayInfo {
    DisplayId id{};
    FixedString<64> name{};
    IRect bounds{};
    IRect usableBounds{};
    DisplayMode desktopMode{};
    f32 contentScale{1.0f};
    DisplayOrientation naturalOrientation{DisplayOrientation::Unknown};
    DisplayOrientation currentOrientation{DisplayOrientation::Unknown};
    bool isPrimary{false};
    FixedVector<DisplayMode, kMaxModesPerDisplay> fullscreenModes{};

    [[nodiscard]] f32 RefreshHz() const noexcept { return desktopMode.refreshHz; }
};

/**
 * DisplayTopology: cached multi-monitor topology snapshot.
 * Refreshed on cold startup and when SDL_EVENT_DISPLAY_* events fire.
 */
class DisplayTopology {
public:
    [[nodiscard]] Result<void, EngineError> Refresh() noexcept;

    [[nodiscard]] std::span<const DisplayInfo> Displays() const noexcept { return m_displays.View(); }
    [[nodiscard]] const DisplayInfo* Find(DisplayId id) const noexcept;
    [[nodiscard]] const DisplayInfo* Primary() const noexcept;
    [[nodiscard]] f32 MaxRefreshHz() const noexcept;

    [[nodiscard]] u32 Revision() const noexcept { return m_revision; }

    [[nodiscard]] std::optional<DisplayMode> FindClosestMode(DisplayId display, const ModeQuery& query) const noexcept;

private:
    FixedVector<DisplayInfo, kMaxDisplays> m_displays{};
    u32 m_revision{0};
};

} // namespace EngineN

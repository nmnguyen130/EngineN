#pragma once

#include "core/Types.h"

namespace en {

enum class MapCellFlags : u8 {
    Unexplored = 0,
    Discovered = 1U << 0,
    Visited    = 1U << 1,
    SaveRoom   = 1U << 2,
    BossRoom   = 1U << 3,
    LockedDoor = 1U << 4,
    SecretRoom = 1U << 5,
    ItemFound  = 1U << 6,
};
consteval bool enable_bitmask_operators(MapCellFlags) noexcept { return true; }

// 8-byte custom player or designer pin placed on the Metroidvania WorldMap2D grid.
struct MapMarker2D {
    i16 cellX{0};
    i16 cellY{0};
    u8  iconId{0};
    u8  colorId{0};
    u16 userTag{0};
};
static_assert(sizeof(MapMarker2D) == 8);

// Zero-allocation 2D Metroidvania world map & fog-of-war grid (32x32 cells + 16 custom markers).
// Value type allocated inside GameplaySystem's Arena and persisted automatically in save blobs.
// Invariant: sizeof(WorldMap2D) == 1160.
struct WorldMap2D {
    static constexpr i32 kWidth      = 32;
    static constexpr i32 kHeight     = 32;
    static constexpr u32 kCellCount  = static_cast<u32>(kWidth * kHeight);
    static constexpr u32 kMaxMarkers = 16U;

    std::array<MapCellFlags, kCellCount> cells{};
    std::array<MapMarker2D, kMaxMarkers> markers{};
    i16                                  currentCellX{0};
    i16                                  currentCellY{0};
    u16                                  discoveredCount{0};
    u8                                   markerCount{0};
    u8                                   pad{0};

    constexpr void Clear() noexcept { *this = WorldMap2D{}; }

    [[nodiscard]] static constexpr bool InBounds(i32 x, i32 y) noexcept {
        return x >= 0 && x < kWidth && y >= 0 && y < kHeight;
    }

    [[nodiscard]] static constexpr u32 CellIndex(i32 x, i32 y) noexcept {
        return static_cast<u32>(y * kWidth + x);
    }

    [[nodiscard]] constexpr MapCellFlags GetCell(i32 x, i32 y) const noexcept {
        return InBounds(x, y) ? cells[CellIndex(x, y)] : MapCellFlags::Unexplored;
    }

    [[nodiscard]] constexpr bool IsDiscovered(i32 x, i32 y) const noexcept {
        return HasFlag(GetCell(x, y), MapCellFlags::Discovered);
    }

    [[nodiscard]] constexpr bool IsVisited(i32 x, i32 y) const noexcept {
        return HasFlag(GetCell(x, y), MapCellFlags::Visited);
    }

    constexpr bool RevealCell(i32 x, i32 y, MapCellFlags extraFlags = MapCellFlags::Visited) noexcept {
        if (!InBounds(x, y)) return false;
        MapCellFlags& c = cells[CellIndex(x, y)];
        const bool newlyDiscovered = !HasFlag(c, MapCellFlags::Discovered);
        c |= (MapCellFlags::Discovered | extraFlags);
        if (newlyDiscovered) ++discoveredCount;
        if (HasFlag(extraFlags, MapCellFlags::Visited)) {
            currentCellX = static_cast<i16>(x);
            currentCellY = static_cast<i16>(y);
        }
        return newlyDiscovered;
    }

    constexpr void RevealRect(
        i32 x0, i32 y0, i32 w, i32 h, MapCellFlags flags = MapCellFlags::Discovered) noexcept {
        for (i32 dy = 0; dy < h; ++dy) {
            for (i32 dx = 0; dx < w; ++dx) {
                (void)RevealCell(x0 + dx, y0 + dy, flags);
            }
        }
    }

    constexpr void RevealRadius(i32 cx, i32 cy, i32 radius, bool visitCenter = true) noexcept {
        for (i32 dy = -radius; dy <= radius; ++dy) {
            for (i32 dx = -radius; dx <= radius; ++dx) {
                if (std::abs(dx) + std::abs(dy) <= radius) {
                    const MapCellFlags f = (dx == 0 && dy == 0 && visitCenter)
                        ? MapCellFlags::Visited
                        : MapCellFlags::Discovered;
                    (void)RevealCell(cx + dx, cy + dy, f);
                }
            }
        }
    }

    constexpr void SetCellFlags(i32 x, i32 y, MapCellFlags flags) noexcept {
        if (!InBounds(x, y)) return;
        if (HasFlag(flags, MapCellFlags::Discovered)) {
            (void)RevealCell(x, y, flags);
        } else {
            cells[CellIndex(x, y)] |= flags;
        }
    }

    constexpr void ClearCellFlags(i32 x, i32 y, MapCellFlags flags) noexcept {
        if (!InBounds(x, y)) return;
        MapCellFlags& c = cells[CellIndex(x, y)];
        if (HasFlag(c, MapCellFlags::Discovered) && HasFlag(flags, MapCellFlags::Discovered) &&
            discoveredCount > 0U) {
            --discoveredCount;
        }
        c &= ~flags;
    }

    [[nodiscard]] constexpr f32 CompletionPercent(u32 totalMapCells = kCellCount) const noexcept {
        if (totalMapCells == 0U) return 0.0f;
        return std::min(100.0f, (static_cast<f32>(discoveredCount) * 100.0f) / static_cast<f32>(totalMapCells));
    }

    constexpr bool PlaceMarker(
        i16 cellX, i16 cellY, u8 iconId, u8 colorId = 0U, u16 userTag = 0U) noexcept {
        if (!InBounds(cellX, cellY)) return false;
        for (u8 i = 0U; i < markerCount; ++i) {
            if (markers[i].cellX == cellX && markers[i].cellY == cellY) {
                markers[i] = {cellX, cellY, iconId, colorId, userTag};
                return true;
            }
        }
        if (markerCount >= kMaxMarkers) return false;
        markers[markerCount++] = {cellX, cellY, iconId, colorId, userTag};
        return true;
    }

    constexpr bool RemoveMarker(i16 cellX, i16 cellY) noexcept {
        for (u8 i = 0U; i < markerCount; ++i) {
            if (markers[i].cellX == cellX && markers[i].cellY == cellY) {
                markers[i] = markers[--markerCount];
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr std::span<const MapMarker2D> Markers() const noexcept {
        return {markers.data(), markerCount};
    }
};
static_assert(sizeof(WorldMap2D) == 1160);

} // namespace en

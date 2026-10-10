#pragma once

#include "core/Types.h"

namespace EngineN {

/**
 * 64-bit packed sort key.
 * High bits encode strict visual order (Pass, Layer, Depth).
 * Low bits group state to minimize pipeline, texture, and blend changes.
 *
 * Bit allocation:
 *  63-60 (4b):  Pass
 *  59-52 (8b):  Layer
 *  51-36 (16b): Depth (Y-sort or sequence order)
 *  35-33 (3b):  Blend Mode
 *  32-21 (12b): Pipeline ID
 *  20-5  (16b): Texture Slot
 *  4-0   (5b):  Spare / Kind
 */
struct SortKey {
    u64 value{0};

    constexpr SortKey() = default;
    constexpr explicit SortKey(u64 inVal) noexcept : value(inVal) {}

    static constexpr u64 Pack(
        u8  passId,
        u8  layerId,
        u16 depth,
        u8  blendMode,
        u16 pipelineId,
        u16 textureSlot,
        u8  spare = 0
    ) noexcept {
        return (static_cast<u64>(passId & 0x0F)      << 60) |
               (static_cast<u64>(layerId)             << 52) |
               (static_cast<u64>(depth)               << 36) |
               (static_cast<u64>(blendMode & 0x07)    << 33) |
               (static_cast<u64>(pipelineId & 0x0FFF) << 21) |
               (static_cast<u64>(textureSlot)         <<  5) |
               (static_cast<u64>(spare & 0x1F));
    }

    [[nodiscard]] constexpr u8  GetPass()     const noexcept { return static_cast<u8>((value >> 60) & 0x0F); }
    [[nodiscard]] constexpr u8  GetLayer()    const noexcept { return static_cast<u8>((value >> 52) & 0xFF); }
    [[nodiscard]] constexpr u16 GetDepth()    const noexcept { return static_cast<u16>((value >> 36) & 0xFFFF); }
    [[nodiscard]] constexpr u8  GetBlend()    const noexcept { return static_cast<u8>((value >> 33) & 0x07); }
    [[nodiscard]] constexpr u16 GetPipeline() const noexcept { return static_cast<u16>((value >> 21) & 0x0FFF); }
    [[nodiscard]] constexpr u16 GetTexture()  const noexcept { return static_cast<u16>((value >> 5) & 0xFFFF); }
    [[nodiscard]] constexpr u8  GetSpare()    const noexcept { return static_cast<u8>(value & 0x1F); }

    constexpr auto operator<=>(const SortKey&) const = default;
};

/**
 * 16-byte SortEntry referencing quad instances and view slot.
 */
struct alignas(16) SortEntry {
    u64 key{0};             // 8 B: Packed 64-bit sort key
    u32 instanceIndex{0};   // 4 B: Offset in FrameArena QuadInstance storage
    u16 instanceCount{1};   // 2 B: Number of consecutive instances
    u16 viewSlot{0};        // 2 B: Index into ViewSlotDesc array

    constexpr auto operator<=>(const SortEntry&) const = default;
};

static_assert(sizeof(SortEntry) == 16, "SortEntry must be exactly 16 bytes");
static_assert(alignof(SortEntry) == 16, "SortEntry must be 16-byte aligned");

} // namespace EngineN

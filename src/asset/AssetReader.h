#pragma once

#include "asset/AssetTypes.h"
#include "asset/AssetTable.h"
#include "asset/TextureAsset.h"
#include "asset/AtlasAsset.h"
#include <algorithm>

namespace EngineN {

/**
 * AssetReader: Read-only facade consumed by simulation and visual extraction.
 * Guarantees zero heap allocations, no locks, and branchless fallback resolution.
 */
class AssetReader {
public:
    AssetReader(const AssetTable<Texture>& textures, const AssetTable<Atlas>& atlases) noexcept
        : m_textures(&textures), m_atlases(&atlases) {}

    explicit AssetReader(const AssetTable<Texture>& textures) noexcept
        : m_textures(&textures), m_atlases(nullptr) {}

    /**
     * Resolves an asset handle to its hot rendering descriptor.
     * Stale, null, or out-of-range handles resolve branchlessly to slot 0 (fallback).
     */
    template <typename T>
    [[nodiscard]] const typename T::Hot& Resolve(AssetHandle<T> handle) const noexcept {
        const auto& table = GetTable<T>();
        const u32 idx = handle.Index();
        const u32 inBounds = (idx < table.Capacity());
        const u32 safeIdx = inBounds ? idx : 0;
        const u32 ok = inBounds & (table.GenData()[safeIdx] == handle.Generation());
        return table.HotData()[ok ? safeIdx : 0];
    }

    /**
     * Resolves a sprite reference into a SpriteView ready for QuadInstance generation.
     * Guaranteed O(1), branchless fallback, zero heap allocations.
     */
    [[nodiscard]] SpriteView Sprite(SpriteRef ref) const noexcept {
        if (!m_atlases) {
            const auto& fbTexHot = Resolve(AssetHandle<Texture>::Null());
            return SpriteView{
                .texture = fbTexHot.gpuHandle,
                .uvRect = {0, 0, 65535, 65535},
                .size = {fbTexHot.width, fbTexHot.height},
                .pivot = {0, 0}
            };
        }

        const auto& atlasHot = Resolve(ref.atlas);
        if (atlasHot.frameCount == 0 || !atlasHot.frames) {
            const auto& fbTexHot = Resolve(AssetHandle<Texture>::Null());
            return SpriteView{
                .texture = fbTexHot.gpuHandle,
                .uvRect = {0, 0, 65535, 65535},
                .size = {fbTexHot.width, fbTexHot.height},
                .pivot = {0, 0}
            };
        }

        const u16 frameIdx = std::min(ref.frameIndex, static_cast<u16>(atlasHot.frameCount - 1));
        const auto& frame = atlasHot.frames[frameIdx];
        const auto& texHot = Resolve(atlasHot.texture);

        return SpriteView{
            .texture = texHot.gpuHandle,
            .uvRect = {frame.uvRect[0], frame.uvRect[1], frame.uvRect[2], frame.uvRect[3]},
            .size = {frame.width, frame.height},
            .pivot = {frame.pivotX, frame.pivotY}
        };
    }

    /**
     * Convenience helper to extract the GPU texture descriptor directly from an asset handle.
     */
    [[nodiscard]] TextureHandle GpuTexture(AssetHandle<Texture> handle) const noexcept {
        return Resolve(handle).gpuHandle;
    }

    [[nodiscard]] const AssetTable<Texture>& Textures() const noexcept {
        return *m_textures;
    }

    [[nodiscard]] const AssetTable<Atlas>* Atlases() const noexcept {
        return m_atlases;
    }

private:
    template <typename T>
    [[nodiscard]] const AssetTable<T>& GetTable() const noexcept {
        if constexpr (std::is_same_v<T, Texture>) {
            return *m_textures;
        } else if constexpr (std::is_same_v<T, Atlas>) {
            return *m_atlases;
        }
    }

    const AssetTable<Texture>* m_textures{nullptr};
    const AssetTable<Atlas>*   m_atlases{nullptr};
};

} // namespace EngineN

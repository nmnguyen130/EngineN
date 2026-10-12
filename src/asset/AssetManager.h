#pragma once

#include "core/Types.h"
#include "core/Result.h"
#include "core/Error.h"
#include "asset/AssetTypes.h"
#include "asset/TextureAsset.h"
#include "asset/AtlasAsset.h"
#include "asset/AssetTable.h"
#include "asset/AssetReader.h"
#include "asset/ITextureDevice.h"

namespace EngineN {

struct AssetManagerConfig {
    u32 textureCapacity{128};
    u32 atlasCapacity{64};
    u32 maxFrames{4096};
};

struct AssetStats {
    u32 activeTextures{0};
    u32 activeAtlases{0};
    u32 totalFrames{0};
    u32 droppedRequests{0};
};

/**
 * AssetManager: Central control plane coordinator for asset registration, decoding, and GPU uploading.
 * Dispatches work to ITextureDevice and provides immutable AssetReader facades to simulation and render pipelines.
 */
class AssetManager {
public:
    [[nodiscard]] static Result<AssetManager, EngineError> Create(const AssetManagerConfig& config,
                                                                 ITextureDevice& textureDevice) noexcept;

    AssetManager() noexcept = default;
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager(AssetManager&& other) noexcept;
    AssetManager& operator=(AssetManager&& other) noexcept;

    [[nodiscard]] Result<void, EngineError> Init(ITextureDevice& textureDevice,
                                                 const AssetManagerConfig& config = {}) noexcept;
    void Shutdown() noexcept;

    // The single point in the frame where asset tables mutate
    void Commit() noexcept { Pump(); }

    // Per-frame control plane pump: processes completions and staged uploads
    void Pump() noexcept;

    // Synchronous texture loading for development and boot-time assets
    [[nodiscard]] AssetHandle<Texture> LoadTexture(AssetId id, const char* path) noexcept;
    [[nodiscard]] AssetHandle<Texture> LoadTexture(const char* path) noexcept;
    [[nodiscard]] Result<AssetHandle<Texture>, EngineError> LoadTexture(const class AssetPath& path) noexcept;
    [[nodiscard]] AssetHandle<Texture> Find(AssetId id) const noexcept;
    void Unload(AssetHandle<Texture> handle) noexcept;

    // Sprite atlas creation and resolution
    [[nodiscard]] AssetHandle<Atlas> CreateAtlas(AssetId id, const AtlasDesc& desc) noexcept;
    [[nodiscard]] AssetHandle<Atlas> CreateAtlasFromGrid(AssetId id, const AtlasGridDesc& desc) noexcept;
    [[nodiscard]] Result<AssetHandle<Atlas>, EngineError> CreateAtlasGrid(const class AssetPath& path,
                                                                         const AtlasGridDesc& desc) noexcept;
    [[nodiscard]] AssetHandle<Atlas> FindAtlas(AssetId id) const noexcept;
    void UnloadAtlas(AssetHandle<Atlas> handle) noexcept;

    [[nodiscard]] AssetReader Reader() const noexcept;
    [[nodiscard]] AssetStats Stats() const noexcept;

private:
    [[nodiscard]] Result<void, EngineError> InitFallbackTexture() noexcept;
    [[nodiscard]] Result<void, EngineError> InitFallbackAtlas() noexcept;

    ITextureDevice*       m_textureDevice{nullptr};
    AssetTable<Texture>   m_textures;
    AssetTable<Atlas>     m_atlases;
    std::vector<FrameHot> m_frameStore;
    u32                   m_frameCursor{0};
    TextureHandle         m_fallbackGpuTexture{TextureHandle::Invalid()};
};

} // namespace EngineN

#include "asset/AssetManager.h"
#include "asset/AssetPath.h"
#include "core/Log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace EngineN {

Result<void, EngineError> AssetManager::Init(ITextureDevice& textureDevice,
                                             const AssetManagerConfig& config) noexcept {
    m_textureDevice = &textureDevice;
    m_textures.Init(config.textureCapacity);
    m_atlases.Init(config.atlasCapacity);
    m_frameStore.resize(std::max(16u, config.maxFrames));
    m_frameCursor = 1; // Slot 0 in m_frameStore permanently reserved for fallback frame

    auto fallbackTexRes = InitFallbackTexture();
    if (!fallbackTexRes) {
        return Err(fallbackTexRes.Error());
    }

    auto fallbackAtlasRes = InitFallbackAtlas();
    if (!fallbackAtlasRes) {
        return Err(fallbackAtlasRes.Error());
    }

    ENGINE_LOG(Info, Asset, "AssetManager initialized: capacity for {} textures, {} atlases, {} frames",
               config.textureCapacity, config.atlasCapacity, config.maxFrames);
    return {};
}

Result<AssetManager, EngineError> AssetManager::Create(const AssetManagerConfig& config,
                                                       ITextureDevice& textureDevice) noexcept {
    AssetManager manager;
    if (auto r = manager.Init(textureDevice, config); !r) {
        return Err(r.Error());
    }
    return manager;
}

AssetManager::~AssetManager() {
    Shutdown();
}

AssetManager::AssetManager(AssetManager&& other) noexcept {
    *this = std::move(other);
}

AssetManager& AssetManager::operator=(AssetManager&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_textureDevice = other.m_textureDevice;
        m_textures = std::move(other.m_textures);
        m_atlases = std::move(other.m_atlases);
        m_frameStore = std::move(other.m_frameStore);
        m_frameCursor = other.m_frameCursor;
        m_fallbackGpuTexture = other.m_fallbackGpuTexture;

        // Re-anchor fallback atlas frame pointer to the active heap storage
        if (!m_frameStore.empty()) {
            Atlas::Hot fbHot = m_atlases.GetHot(0);
            fbHot.frames = &m_frameStore[0];
            m_atlases.SetFallback(fbHot, m_atlases.GetCold(0));
        }

        other.m_textureDevice = nullptr;
        other.m_fallbackGpuTexture = TextureHandle::Invalid();
        other.m_frameCursor = 0;
    }
    return *this;
}

void AssetManager::Shutdown() noexcept {
    if (m_textureDevice) {
        for (u32 i = 1; i < m_textures.Capacity(); ++i) {
            if (m_textures.GetState(i) == AssetState::Ready) {
                m_textureDevice->Destroy(m_textures.GetHot(i).gpuHandle);
            }
        }
        if (m_fallbackGpuTexture.IsValid()) {
            m_textureDevice->Destroy(m_fallbackGpuTexture);
            m_fallbackGpuTexture = TextureHandle::Invalid();
        }
        m_textureDevice = nullptr;
    }
    m_frameCursor = 0;
}

Result<void, EngineError> AssetManager::InitFallbackTexture() noexcept {
    constexpr u32 kFallbackSize = 8;
    u32 fallbackPixels[kFallbackSize * kFallbackSize];

    for (u32 y = 0; y < kFallbackSize; ++y) {
        for (u32 x = 0; x < kFallbackSize; ++x) {
            const bool isMagenta = ((x ^ y) & 1) != 0;
            // Little-endian RGBA32: (A << 24) | (B << 16) | (G << 8) | R
            fallbackPixels[y * kFallbackSize + x] = isMagenta ? 0xFFFF00FFu : 0xFF000000u;
        }
    }

    TextureDesc desc{
        .width = kFallbackSize,
        .height = kFallbackSize,
        .format = TextureFormat::RGBA8,
        .isStatic = true
    };

    auto createRes = m_textureDevice->Create(desc);
    if (!createRes) {
        return Err(createRes.Error());
    }

    m_fallbackGpuTexture = createRes.Value();
    URect uploadRect{0, 0, kFallbackSize, kFallbackSize};
    auto uploadRes = m_textureDevice->Upload(
        m_fallbackGpuTexture,
        uploadRect,
        fallbackPixels,
        kFallbackSize * sizeof(u32)
    );

    if (!uploadRes) {
        m_textureDevice->Destroy(m_fallbackGpuTexture);
        m_fallbackGpuTexture = TextureHandle::Invalid();
        return Err(uploadRes.Error());
    }

    Texture::Hot fallbackHot{
        .gpuHandle = m_fallbackGpuTexture,
        .width = static_cast<u16>(kFallbackSize),
        .height = static_cast<u16>(kFallbackSize),
        .flags = 0,
        .reserved = 0
    };

    Texture::Cold fallbackCold{
        .id = MakeAssetId("__fallback__"),
        .cpuBytes = sizeof(fallbackPixels),
        .gpuBytes = sizeof(fallbackPixels),
        .debugPath = "__fallback__"
    };

    m_textures.SetFallback(fallbackHot, fallbackCold);
    return {};
}

Result<void, EngineError> AssetManager::InitFallbackAtlas() noexcept {
    if (m_frameStore.empty()) {
        m_frameStore.resize(16);
    }
    m_frameStore[0] = FrameHot(0, 0, 65535, 65535, 8, 8, 0, 0);
    m_frameCursor = std::max(1u, m_frameCursor);

    Atlas::Hot fallbackHot{
        .frames = &m_frameStore[0],
        .texture = AssetHandle<Texture>::Null(),
        .frameCount = 1,
        .reserved = 0
    };

    Atlas::Cold fallbackCold{
        .id = MakeAssetId("__fallback_atlas__"),
        .firstFrameIndex = 0,
        .debugPath = "__fallback_atlas__"
    };

    m_atlases.SetFallback(fallbackHot, fallbackCold);
    return {};
}

void AssetManager::Pump() noexcept {
    // Phase 1 is synchronous; Pump serves as the frame integration seam for Phase 2 async completion rings
}

AssetHandle<Texture> AssetManager::LoadTexture(AssetId id, const char* path) noexcept {
    if (!id.Valid() || !path) {
        return AssetHandle<Texture>::Null();
    }

    // Deduplication check
    auto existing = m_textures.Find(id);
    if (existing.IsValid()) {
        return existing;
    }

    // Allocate table slot
    auto handle = m_textures.Allocate(id);
    if (handle.IsNull()) {
        ENGINE_LOG(Warn, Asset, "AssetTable capacity exhausted; dropped texture request for '{}'", path);
        return handle;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (!pixels) {
        ENGINE_LOG(Error, Asset, "Failed to load image '{}': {}", path, stbi_failure_reason());
        m_textures.SetFailed(handle.Index());
        return handle; // Resolves branchlessly to fallback slot 0
    }

    // Premultiply RGB texels by alpha so sampling matches premultiplied blend state
    const u32 totalPixels = static_cast<u32>(width * height);
    for (u32 p = 0; p < totalPixels; ++p) {
        stbi_uc* px = &pixels[p * 4];
        const float alpha = static_cast<float>(px[3]) / 255.0f;
        px[0] = static_cast<stbi_uc>(std::clamp(static_cast<float>(px[0]) * alpha + 0.5f, 0.0f, 255.0f));
        px[1] = static_cast<stbi_uc>(std::clamp(static_cast<float>(px[1]) * alpha + 0.5f, 0.0f, 255.0f));
        px[2] = static_cast<stbi_uc>(std::clamp(static_cast<float>(px[2]) * alpha + 0.5f, 0.0f, 255.0f));
    }

    TextureDesc desc{
        .width = static_cast<u32>(width),
        .height = static_cast<u32>(height),
        .format = TextureFormat::RGBA8,
        .isStatic = true
    };

    auto createRes = m_textureDevice->Create(desc);
    if (!createRes) {
        ENGINE_LOG(Error, Asset, "Failed to allocate GPU texture for '{}': {}", path, createRes.Error().message);
        stbi_image_free(pixels);
        m_textures.SetFailed(handle.Index());
        return handle;
    }

    TextureHandle gpuHandle = createRes.Value();
    URect uploadRect{0, 0, static_cast<u32>(width), static_cast<u32>(height)};
    auto uploadRes = m_textureDevice->Upload(gpuHandle, uploadRect, pixels, static_cast<u32>(width * 4));
    stbi_image_free(pixels);

    if (!uploadRes) {
        ENGINE_LOG(Error, Asset, "Failed to upload pixel data for '{}': {}", path, uploadRes.Error().message);
        m_textureDevice->Destroy(gpuHandle);
        m_textures.SetFailed(handle.Index());
        return handle;
    }

    Texture::Hot hot{
        .gpuHandle = gpuHandle,
        .width = static_cast<u16>(width),
        .height = static_cast<u16>(height),
        .flags = 0,
        .reserved = 0
    };

    Texture::Cold cold{
        .id = id,
        .cpuBytes = static_cast<u32>(width * height * 4),
        .gpuBytes = static_cast<u32>(width * height * 4),
        .debugPath = {}
    };
    std::snprintf(cold.debugPath, sizeof(cold.debugPath), "%s", path);

    m_textures.SetReady(handle.Index(), hot, cold);
    ENGINE_LOG(Info, Asset, "Loaded texture '{}' ({}x{}, slot {})", path, width, height, handle.Index());
    return handle;
}

AssetHandle<Texture> AssetManager::LoadTexture(const char* path) noexcept {
    if (!path) return AssetHandle<Texture>::Null();
    return LoadTexture(MakeAssetId(path), path);
}

Result<AssetHandle<Texture>, EngineError> AssetManager::LoadTexture(const AssetPath& path) noexcept {
    const std::string pathStr(path.View());
    auto handle = LoadTexture(path.Id(), pathStr.c_str());
    if (handle.IsNull()) {
        return Err(EngineError{"Failed to load texture"});
    }
    return handle;
}

Result<AssetHandle<Atlas>, EngineError> AssetManager::CreateAtlasGrid(
    const AssetPath& path, const AtlasGridDesc& desc) noexcept {
    auto handle = CreateAtlasFromGrid(path.Id(), desc);
    if (handle.IsNull()) {
        return Err(EngineError{"Failed to create atlas grid"});
    }
    return handle;
}

AssetHandle<Texture> AssetManager::Find(AssetId id) const noexcept {
    return m_textures.Find(id);
}

void AssetManager::Unload(AssetHandle<Texture> handle) noexcept {
    const u32 slot = handle.Index();
    if (slot == 0 || slot >= m_textures.Capacity()) return;
    if (m_textures.GetGen(slot) != handle.Generation()) return;

    if (m_textureDevice) {
        m_textureDevice->Destroy(m_textures.GetHot(slot).gpuHandle);
    }
    m_textures.Free(handle);
}

AssetHandle<Atlas> AssetManager::CreateAtlas(AssetId id, const AtlasDesc& desc) noexcept {
    if (!id.Valid() || desc.frames.empty()) {
        return AssetHandle<Atlas>::Null();
    }

    // Deduplication check
    auto existing = m_atlases.Find(id);
    if (existing.IsValid()) {
        return existing;
    }

    const u32 frameCount = static_cast<u32>(desc.frames.size());
    if (m_frameCursor + frameCount > m_frameStore.size()) {
        ENGINE_LOG(Warn, Asset, "FrameStore capacity exhausted; dropped atlas request");
        return AssetHandle<Atlas>::Null();
    }

    auto handle = m_atlases.Allocate(id);
    if (handle.IsNull()) {
        ENGINE_LOG(Warn, Asset, "AssetTable<Atlas> capacity exhausted; dropped atlas request");
        return handle;
    }

    const u32 firstFrame = m_frameCursor;
    std::memcpy(&m_frameStore[firstFrame], desc.frames.data(), frameCount * sizeof(FrameHot));
    m_frameCursor += frameCount;

    Atlas::Hot hot{
        .frames = &m_frameStore[firstFrame],
        .texture = desc.texture,
        .frameCount = static_cast<u16>(frameCount),
        .reserved = 0
    };

    Atlas::Cold cold{
        .id = id,
        .firstFrameIndex = firstFrame,
        .debugPath = {}
    };

    m_atlases.SetReady(handle.Index(), hot, cold);
    ENGINE_LOG(Info, Asset, "Created atlas with {} frames (slot {})", frameCount, handle.Index());
    return handle;
}

AssetHandle<Atlas> AssetManager::CreateAtlasFromGrid(AssetId id, const AtlasGridDesc& desc) noexcept {
    if (!id.Valid() || desc.frameCount == 0 || desc.frameWidth == 0 || desc.frameHeight == 0) {
        return AssetHandle<Atlas>::Null();
    }

    // Deduplication check
    auto existing = m_atlases.Find(id);
    if (existing.IsValid()) {
        return existing;
    }

    // Resolve texture dimensions to compute unorm16 UV rectangles
    const auto& texHot = m_textures.GetHot(desc.texture.Index());
    if (texHot.width == 0 || texHot.height == 0) {
        ENGINE_LOG(Error, Asset, "Texture handle invalid or zero-dimensioned for grid atlas creation");
        return AssetHandle<Atlas>::Null();
    }

    const u32 texW = texHot.width;
    const u32 texH = texHot.height;
    const u32 cols = std::max(1u, static_cast<u32>(texW / desc.frameWidth));

    if (m_frameCursor + desc.frameCount > m_frameStore.size()) {
        ENGINE_LOG(Warn, Asset, "FrameStore capacity exhausted; dropped grid atlas request");
        return AssetHandle<Atlas>::Null();
    }

    auto handle = m_atlases.Allocate(id);
    if (handle.IsNull()) {
        ENGINE_LOG(Warn, Asset, "AssetTable<Atlas> capacity exhausted; dropped grid atlas request");
        return handle;
    }

    const u32 firstFrame = m_frameCursor;
    for (u32 i = 0; i < desc.frameCount; ++i) {
        const u32 col = i % cols;
        const u32 row = i / cols;
        const u32 px0 = col * desc.frameWidth;
        const u32 py0 = row * desc.frameHeight;
        const u32 px1 = px0 + desc.frameWidth;
        const u32 py1 = py0 + desc.frameHeight;

        const u16 u0 = static_cast<u16>((static_cast<u64>(px0) * 65535ULL) / texW);
        const u16 v0 = static_cast<u16>((static_cast<u64>(py0) * 65535ULL) / texH);
        const u16 u1 = static_cast<u16>((static_cast<u64>(px1) * 65535ULL) / texW);
        const u16 v1 = static_cast<u16>((static_cast<u64>(py1) * 65535ULL) / texH);

        m_frameStore[firstFrame + i] = FrameHot(u0, v0, u1, v1, desc.frameWidth, desc.frameHeight, desc.pivotX, desc.pivotY);
    }
    m_frameCursor += desc.frameCount;

    Atlas::Hot hot{
        .frames = &m_frameStore[firstFrame],
        .texture = desc.texture,
        .frameCount = desc.frameCount,
        .reserved = 0
    };

    Atlas::Cold cold{
        .id = id,
        .firstFrameIndex = firstFrame,
        .debugPath = {}
    };

    m_atlases.SetReady(handle.Index(), hot, cold);
    ENGINE_LOG(Info, Asset, "Created grid atlas with {} frames from {}x{} texture (slot {})",
               desc.frameCount, texW, texH, handle.Index());
    return handle;
}

AssetHandle<Atlas> AssetManager::FindAtlas(AssetId id) const noexcept {
    return m_atlases.Find(id);
}

void AssetManager::UnloadAtlas(AssetHandle<Atlas> handle) noexcept {
    const u32 slot = handle.Index();
    if (slot == 0 || slot >= m_atlases.Capacity()) return;
    if (m_atlases.GetGen(slot) != handle.Generation()) return;
    m_atlases.Free(handle);
}

AssetReader AssetManager::Reader() const noexcept {
    return AssetReader(m_textures, m_atlases);
}

AssetStats AssetManager::Stats() const noexcept {
    return AssetStats{
        .activeTextures = m_textures.ActiveCount(),
        .activeAtlases = m_atlases.ActiveCount(),
        .totalFrames = m_frameCursor,
        .droppedRequests = m_textures.DroppedAllocations() + m_atlases.DroppedAllocations()
    };
}

} // namespace EngineN

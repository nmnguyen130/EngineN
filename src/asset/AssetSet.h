#pragma once

#include "core/Types.h"
#include "core/Result.h"
#include "core/Error.h"
#include "asset/AssetPath.h"
#include "asset/AssetManager.h"

#include <vector>
#include <string_view>

namespace EngineN {

/**
 * AssetSet: Scope-based RAII container managing level, room, or UI asset lifecycles.
 * Unloads atlases and textures in correct reverse-dependency order on destruction.
 */
class AssetSet {
public:
    explicit AssetSet(AssetManager& mgr) noexcept : m_mgr(&mgr) {}
    AssetSet(const AssetSet&) = delete;
    AssetSet& operator=(const AssetSet&) = delete;

    ~AssetSet() {
        Release();
    }

    [[nodiscard]] Result<AssetHandle<Texture>, EngineError> LoadTexture(std::string_view rawPath) noexcept {
        auto p = AssetPath::Make(rawPath);
        if (!p) return Err(p.Error());
        auto h = m_mgr->LoadTexture(*p);
        if (h) m_textures.push_back(*h);
        return h;
    }

    [[nodiscard]] Result<AssetHandle<Atlas>, EngineError> LoadAtlasGrid(
        std::string_view rawPath, const AtlasGridDesc& desc) noexcept {
        auto p = AssetPath::Make(rawPath);
        if (!p) return Err(p.Error());
        auto h = m_mgr->CreateAtlasGrid(*p, desc);
        if (h) m_atlases.push_back(*h);
        return h;
    }

    void Release() noexcept {
        // Atlases first as they reference underlying textures
        for (auto a : m_atlases)  m_mgr->UnloadAtlas(a);
        for (auto t : m_textures) m_mgr->Unload(t);
        m_atlases.clear();
        m_textures.clear();
    }

private:
    AssetManager*                     m_mgr{nullptr};
    std::vector<AssetHandle<Atlas>>   m_atlases;
    std::vector<AssetHandle<Texture>> m_textures;
};

} // namespace EngineN

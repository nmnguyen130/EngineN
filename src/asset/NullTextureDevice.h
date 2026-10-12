#pragma once

#include "asset/ITextureDevice.h"

namespace EngineN {

/**
 * Headless stub implementing ITextureDevice without GPU or windowing context.
 * Useful for automated unit tests, headless CI pipelines, and zero-alloc verification.
 */
class NullTextureDevice final : public ITextureDevice {
public:
    Result<TextureHandle, EngineError> Create(const TextureDesc&) noexcept override {
        m_createCount++;
        if (m_simulateCreateFailure) {
            return Err(EngineError{"Simulated NullTextureDevice creation failure"});
        }
        const u16 slot = m_nextSlot++;
        return TextureHandle{slot, 1};
    }

    Result<void, EngineError> Upload(TextureHandle, const URect&, const void*, u32) noexcept override {
        m_uploadCount++;
        if (m_simulateUploadFailure) {
            return Err(EngineError{"Simulated NullTextureDevice upload failure"});
        }
        return {};
    }

    void Destroy(TextureHandle) noexcept override {
        m_destroyCount++;
    }

    void ResetStats() noexcept {
        m_createCount = 0;
        m_uploadCount = 0;
        m_destroyCount = 0;
    }

    void SetSimulateCreateFailure(bool fail) noexcept { m_simulateCreateFailure = fail; }
    void SetSimulateUploadFailure(bool fail) noexcept { m_simulateUploadFailure = fail; }

    [[nodiscard]] u32 CreateCount() const noexcept { return m_createCount; }
    [[nodiscard]] u32 UploadCount() const noexcept { return m_uploadCount; }
    [[nodiscard]] u32 DestroyCount() const noexcept { return m_destroyCount; }

private:
    u16  m_nextSlot{1};
    u32  m_createCount{0};
    u32  m_uploadCount{0};
    u32  m_destroyCount{0};
    bool m_simulateCreateFailure{false};
    bool m_simulateUploadFailure{false};
};

} // namespace EngineN

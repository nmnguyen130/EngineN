#pragma once

#include "renderer/RenderBackend.h"

namespace EngineN {

/**
 * NullBackend: Headless software backend for automated testing and CI.
 * Computes a bitwise deterministic 64-bit FNV-1a packet hash and audits ordering invariants.
 */
class NullBackend final : public RenderBackend {
public:
    NullBackend() = default;
    ~NullBackend() override = default;

    [[nodiscard]] Result<void, EngineError> Execute(const FramePacket& packet) noexcept override;
    [[nodiscard]] Result<PresentResult, EngineError> Present(const PresentationPolicy& policy) noexcept override {
        (void)policy;
        return PresentResult{};
    }

    [[nodiscard]] u64 LastPacketHash() const noexcept { return m_lastPacketHash; }
    [[nodiscard]] u64 ExecutionCount() const noexcept { return m_executionCount; }
    [[nodiscard]] u32 LastBatchCount() const noexcept { return m_lastBatchCount; }
    [[nodiscard]] u32 LastInstanceCount() const noexcept { return m_lastInstanceCount; }

private:
    u64 m_lastPacketHash{0};
    u64 m_executionCount{0};
    u32 m_lastBatchCount{0};
    u32 m_lastInstanceCount{0};
};

} // namespace EngineN

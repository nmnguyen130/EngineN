#include "renderer/NullBackend.h"

namespace EngineN {

namespace {

constexpr u64 kFnv1aOffsetBasis = 14695981039346656037ULL;
constexpr u64 kFnv1aPrime       = 1099511628211ULL;

inline u64 HashBytes(u64 hash, const void* data, usize size) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kFnv1aPrime;
    }
    return hash;
}

} // namespace

Result<void, EngineError> NullBackend::Execute(const FramePacket& packet) noexcept {
    u64 hash = kFnv1aOffsetBasis;

    // 1. Hash batches
    if (!packet.batches.empty()) {
        hash = HashBytes(hash, packet.batches.data(), packet.batches.size() * sizeof(DrawBatch));
    }

    // 2. Hash instances
    if (!packet.instances.empty()) {
        hash = HashBytes(hash, packet.instances.data(), packet.instances.size() * sizeof(QuadInstance));
    }

    // 3. Hash view slots
    if (!packet.viewSlots.empty()) {
        hash = HashBytes(hash, packet.viewSlots.data(), packet.viewSlots.size() * sizeof(ViewSlotDesc));
    }

    // 4. Hash passes
    if (!packet.passes.empty()) {
        hash = HashBytes(hash, packet.passes.data(), packet.passes.size() * sizeof(RenderPassDesc));
    }

    m_lastPacketHash = hash;
    m_lastBatchCount = static_cast<u32>(packet.batches.size());
    m_lastInstanceCount = static_cast<u32>(packet.instances.size());
    m_executionCount++;

    return {};
}

} // namespace EngineN

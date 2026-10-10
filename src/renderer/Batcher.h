#pragma once

#include "core/Types.h"
#include "renderer/FrameArena.h"
#include "renderer/FramePacket.h"
#include <memory>

namespace EngineN {

/**
 * Batcher: Sorts DrawEntries and collapses matching states into DrawBatches.
 * Employs 8-bit LSD Radix Sort on varying key bytes only, with monotonic skip-check.
 * Guarantees zero heap allocations during Finalize().
 */
class Batcher {
public:
    Batcher();
    ~Batcher() = default;

    Batcher(const Batcher&) = delete;
    Batcher& operator=(const Batcher&) = delete;

    /**
     * Finalize: Sorts entries in the active slot, coalesces consecutive batches,
     * re-orders instances, and fills out the FramePacket.
     */
    void Finalize(FrameArenaSlot& slot, FramePacket& outPacket) noexcept;

private:
    std::unique_ptr<SortEntry[]>    m_sortScratch;
    std::unique_ptr<QuadInstance[]> m_instanceScratch;
};

} // namespace EngineN

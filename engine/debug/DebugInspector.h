#pragma once

#include "debug/DebugBatcher.h"
#include "debug/DebugTypes.h"

#include <algorithm>

namespace en::input { class ActionBuffer; }

namespace en::debug {

// Stateless world/spatial overlay extractor, mouse entity picker, and 64-tick flight recorder.
// Non-owning view over the 4 KB ActorTickRecord ring in DebugSystem's arena.
// Invariant: Never mutates World or SpatialIndex2D state during inspection; executes a single fused pass over entities.
class DebugInspector {
public:
    static constexpr usize kHistoryTicks = 64;

    void BindArena(std::span<ActorTickRecord, kHistoryTicks> tickRing) noexcept;
    void Clear() noexcept;

    void RecordActorTick(const world::World& world,
                         u64 tickIndex,
                         const input::TickInput& tickInput,
                         const input::ActionBuffer& actionBuffer) noexcept;

    world::EntityId PickEntityAtScreenPos(const world::World& world,
                                          time::RenderTimeView timeView,
                                          input::Vec2 mousePos,
                                          platform::Extent2i viewportSize) noexcept;

    constexpr void SetPinnedEntity(world::EntityId id) noexcept { m_pinnedEntity = id; }
    [[nodiscard]] constexpr world::EntityId PinnedEntity() const noexcept { return m_pinnedEntity; }

    constexpr void StepScrubOffset(i32 deltaTicks) noexcept {
        const i32 maxOff = static_cast<i32>(m_recordedCount > 0 ? m_recordedCount - 1 : 0);
        const i32 next   = std::clamp(static_cast<i32>(m_scrubOffset) + deltaTicks, 0, maxOff);
        m_scrubOffset    = static_cast<u8>(next);
    }
    constexpr void ResetScrubOffset() noexcept { m_scrubOffset = 0; }
    [[nodiscard]] constexpr u8 ScrubOffset() const noexcept { return m_scrubOffset; }

    void EmitWorldAndInspectorOverlays(OverlayFlags flags,
                                       const DebugEmitOverlayContext& ctx,
                                       DebugBatcher& worldBatcher,
                                       DebugBatcher& hudBatcher) const noexcept;

    void EmitWorldAndInspectorOverlays(OverlayFlags flags,
                                       const DebugEmitOverlayContext& ctx,
                                       renderer::TextureHandle debugAtlas,
                                       renderer::RenderList& outList) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    void EmitEntityInspectorCard(const world::World& world,
                                 platform::Extent2i viewportSize,
                                 DebugBatcher& hudBatcher) const noexcept;

    void EmitFlightScrubberOverlay(u64 currentTickIndex,
                                   platform::Extent2i viewportSize,
                                   DebugBatcher& worldBatcher,
                                   DebugBatcher& hudBatcher) const noexcept;

    void EmitInputViewerOverlay(const input::FrameInput& frameInput,
                                platform::Extent2i viewportSize,
                                DebugBatcher& hudBatcher) const noexcept;

    std::span<ActorTickRecord> m_tickRing{};
    world::EntityId m_pinnedEntity{world::kNullEntity};
    u8 m_writeCursor{0};
    u8 m_scrubOffset{0};
    u8 m_recordedCount{0};
};

} // namespace en::debug

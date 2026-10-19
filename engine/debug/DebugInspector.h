#pragma once

#include "debug/DebugBatcher.h"
#include "debug/DebugTypes.h"

#include <algorithm>

namespace en::input { class ActionBuffer; }

namespace en::debug {

// Stateless world/spatial overlay extractor, mouse entity picker, and 64-tick flight recorder.
// Non-owning view over the 4 KB ActorTickRecord ring in DebugSystem's arena.
// Invariant: Never mutates World or SpatialIndex2D state during inspection; resolves components via 1-hop bitmasks.
class alignas(32) DebugInspector {
public:
    static constexpr usize kHistoryTicks = 64;

    void BindArena(std::span<ActorTickRecord, kHistoryTicks> tickRing) noexcept;
    void Clear() noexcept;

    void RecordActorTick(const world::World& world,
                         u64 tickIndex,
                         const input::TickInput& tickInput,
                         const input::ActionBuffer& actionBuffer) noexcept;

    world::EntityId PickEntityAtScreenPos(const world::World& world,
                                          input::Vec2 mousePos,
                                          renderer::ViewportExtent viewportSize) noexcept;

    world::EntityId PickEntityAtScreenPos(const world::World& world,
                                          const time::RenderTimeView&,
                                          input::Vec2 mousePos,
                                          renderer::ViewportExtent viewportSize) noexcept {
        return PickEntityAtScreenPos(world, mousePos, viewportSize);
    }

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
                                       renderer::RenderList& outList) const noexcept {
        DebugBatcher worldBatcher(outList, debugAtlas, kDebugWorldPassId, 0);
        DebugBatcher hudBatcher(outList, debugAtlas, kDebugHudPassId, kDebugHudViewSlot);
        EmitWorldAndInspectorOverlays(flags, ctx, worldBatcher, hudBatcher);
    }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    void EmitEntityInspectorCard(const world::World& world,
                                 renderer::ViewportExtent viewportSize,
                                 DebugBatcher& hudBatcher) const noexcept;

    void EmitFlightScrubberOverlay(renderer::ViewportExtent viewportSize,
                                   DebugBatcher& worldBatcher,
                                   DebugBatcher& hudBatcher) const noexcept;

    void EmitInputViewerOverlay(const input::FrameInput& frameInput,
                                renderer::ViewportExtent viewportSize,
                                DebugBatcher& hudBatcher) const noexcept;

    std::span<ActorTickRecord> m_tickRing{};
    world::EntityId m_pinnedEntity{world::kNullEntity};
    u8 m_writeCursor{0};
    u8 m_scrubOffset{0};
    u8 m_recordedCount{0};
};
static_assert(sizeof(DebugInspector) == 32);

} // namespace en::debug

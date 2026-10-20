#pragma once

#include "debug/DebugBatcher.h"
#include "gameplay/GameplayPipeline.h"

namespace en {

// Read-only 1-hop entity inspector, screen-to-world picker, and 64-tick combat flight recorder.
// Non-owning view over arena memory; zero side effects on simulation state.
// Invariant: m_history ring stores the most recent 64 ticks of the pinned entity.
class alignas(32) DebugInspector {
public:
    static constexpr usize kFlightHistoryTicks = 64;

    [[nodiscard]] bool Init(AlignedArena& arena) noexcept;
    void Clear() noexcept;

    void RecordActorTick(
        const World& world,
        u64 tickIndex,
        const TickInput& tickInput,
        const ActionBuffer& actionBuffer) noexcept;

    [[nodiscard]] EntityId PickEntityAtWorldPos(const World& world, Vec2 worldPos) noexcept;

    constexpr void SetPinnedEntity(EntityId id) noexcept { m_pinnedEntity = id; }
    [[nodiscard]] constexpr EntityId PinnedEntity() const noexcept { return m_pinnedEntity; }

    constexpr void StepScrubOffset(i32 deltaTicks) noexcept {
        m_scrubOffset = static_cast<u8>(std::clamp(static_cast<i32>(m_scrubOffset) + deltaTicks, 0, 63));
    }
    constexpr void ResetScrubOffset() noexcept { m_scrubOffset = 0; }
    [[nodiscard]] constexpr u8 ScrubOffset() const noexcept { return m_scrubOffset; }

    void EmitWorldAndInspectorOverlays(
        OverlayFlags flags,
        const DebugFrameContext& ctx,
        DebugBatcher& worldBatcher,
        DebugBatcher& hudBatcher) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    std::span<ActorTickRecord> m_history{};
    EntityId m_pinnedEntity{kNullEntity};
    u8 m_writeCursor{0};
    u8 m_validRecords{0};
    u8 m_scrubOffset{0};
};

} // namespace en

#pragma once

#include "debug/DebugBatcher.h"

namespace en {

// Read-only World/Physics/Gameplay/Audio introspector, picker, and flight recorder.
class alignas(16) DebugInspector {
public:
    constexpr void Init(std::span<ActorTickRecord, kActorFlightHistoryTicks> historyStorage) noexcept {
        m_history = historyStorage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        if (m_history) {
            for (u32 i = 0U; i < kActorFlightHistoryTicks; ++i) {
                m_history[i] = ActorTickRecord{};
            }
        }
        m_pinnedEntity = kNullEntity;
        m_writeIdx = 0U;
        m_validCount = 0U;
        m_scrubOffset = 0U;
    }

    void RecordActorTick(
        const World& world,
        u32 tickIndex,
        const TickInput* tickInput = nullptr,
        const ActionBuffer* actionBuffer = nullptr) noexcept;

    [[nodiscard]] EntityId PickEntityAtWorldPos(
        const World& world,
        Vec2 worldPos,
        f32 pickRadius = 1.0f) noexcept;

    [[nodiscard]] EntityId PickEntityAtScreenPos(
        const World& world,
        Vec2 screenPos,
        ViewportExtent viewportSize,
        const Camera2D& worldCam = {}) noexcept;

    constexpr void SetPinnedEntity(EntityId entity) noexcept {
        if (m_pinnedEntity != entity) {
            m_pinnedEntity = entity;
            m_writeIdx = 0U;
            m_validCount = 0U;
            m_scrubOffset = 0U;
        }
    }

    [[nodiscard]] constexpr EntityId PinnedEntity() const noexcept { return m_pinnedEntity; }

    constexpr void StepScrubOffset(i32 deltaTicks) noexcept {
        if (m_validCount == 0U) {
            m_scrubOffset = 0U;
            return;
        }
        const i32 maxOff = static_cast<i32>(m_validCount) - 1;
        const i32 next = std::clamp(static_cast<i32>(m_scrubOffset) + deltaTicks, 0, maxOff);
        m_scrubOffset = static_cast<u8>(next);
    }

    constexpr void ResetScrubOffset() noexcept { m_scrubOffset = 0U; }
    [[nodiscard]] constexpr u8 ScrubOffset() const noexcept { return m_scrubOffset; }
    [[nodiscard]] constexpr u8 RecordedTickCount() const noexcept { return m_validCount; }

    [[nodiscard]] const ActorTickRecord* InspectedRecord() const noexcept;

    void EmitWorldAndInspectorOverlays(
        OverlayFlags flags,
        const DebugFrameContext& ctx,
        DebugBatcher& worldBatcher,
        DebugBatcher& hudBatcher) const noexcept;

private:
    ActorTickRecord* m_history{nullptr};
    EntityId m_pinnedEntity{kNullEntity};
    u16 m_writeIdx{0U};
    u8 m_validCount{0U};
    u8 m_scrubOffset{0U};
};
static_assert(sizeof(DebugInspector) == 16);

} // namespace en

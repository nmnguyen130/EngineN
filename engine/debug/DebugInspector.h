#pragma once

#include "debug/DebugBatcher.h"

namespace en {

class alignas(16) DebugInspector {
public:
    constexpr void Init(std::span<ActorSample, kFlightTicks> storage) noexcept {
        m_hist = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        if (m_hist) {
            for (u32 i = 0U; i < kFlightTicks; ++i) m_hist[i] = ActorSample{};
        }
        m_pin = kNullEntity;
        m_write = 0U;
        m_valid = 0U;
        m_scrub = 0U;
    }

    void Record(
        const World& world,
        u32 tick,
        const TickInput* input = nullptr,
        const ActionBuffer* buffer = nullptr) noexcept;

    [[nodiscard]] EntityId Pick(const World& world, Vec2 worldPos, f32 radius = 1.0f) noexcept;

    [[nodiscard]] EntityId PickScreen(
        const World& world,
        Vec2 screen,
        Viewport vp,
        const Camera2D& cam = {}) noexcept;

    constexpr void Pin(EntityId e) noexcept {
        if (m_pin != e) {
            m_pin = e;
            m_write = 0U;
            m_valid = 0U;
            m_scrub = 0U;
        }
    }

    [[nodiscard]] constexpr EntityId Pinned() const noexcept { return m_pin; }

    constexpr void Scrub(i32 delta) noexcept {
        if (m_valid == 0U) { m_scrub = 0U; return; }
        const i32 maxOff = static_cast<i32>(m_valid) - 1;
        m_scrub = static_cast<u8>(std::clamp(static_cast<i32>(m_scrub) + delta, 0, maxOff));
    }

    constexpr void ResetScrub() noexcept { m_scrub = 0U; }
    [[nodiscard]] constexpr u8 ScrubOffset() const noexcept { return m_scrub; }
    [[nodiscard]] constexpr u8 Recorded() const noexcept { return m_valid; }

    [[nodiscard]] const ActorSample* Current() const noexcept;

    void Emit(Overlay flags, const FrameCtx& ctx, DebugDraw& world, DebugDraw& hud) const noexcept;

private:
    ActorSample* m_hist{nullptr};
    EntityId m_pin{kNullEntity};
    u16 m_write{0U};
    u8  m_valid{0U};
    u8  m_scrub{0U};
};
static_assert(sizeof(DebugInspector) == 16);

} // namespace en

#pragma once

#include "core/Types.h"
#include "math/Vec2.h"

#include <array>
#include <span>

namespace EngineN {

enum class SimEventKind : u8 {
    AttackStarted = 0,
    HitLanded,
    DashStarted,
    Landed
};

struct SimEvent {
    SimEventKind kind{};
    u32  tick = 0;
    Vec2 pos{};
    f32  magnitude = 0.0f;
};

/**
 * SimEvents: Fixed-capacity ring buffer conveying events from simulation to presentation.
 * Has frame lifetime, zero heap allocations, and tracks dropped event counts.
 */
class SimEvents {
public:
    static constexpr usize kCapacity = 256;

    bool Push(const SimEvent& event) noexcept {
        if (m_count >= kCapacity) {
            ++m_dropped;
            return false;
        }

        m_events[m_count++] = event;
        return true;
    }

    [[nodiscard]] std::span<const SimEvent> View() const noexcept {
        return {m_events.data(), m_count};
    }

    [[nodiscard]] u32 Dropped() const noexcept { return m_dropped; }
    void Clear() noexcept { m_count = 0; }

private:
    std::array<SimEvent, kCapacity> m_events{};
    usize m_count = 0;
    u32 m_dropped = 0;
};

} // namespace EngineN

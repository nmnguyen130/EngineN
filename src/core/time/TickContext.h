#pragma once

#include "core/time/SimEvents.h"
#include "core/time/TimeManager.h"

#include <type_traits>

namespace EngineN {

/**
 * TickContext: Scope parameter passed into simulation Tick() calls.
 * Isolates simulation logic from wall-clock time and rendering details.
 */
class TickContext {
public:
    TickContext(TimeManager& time, SimEvents& events) noexcept
        : m_time(time), m_events(events) {}

    template <typename C = u8>
    [[nodiscard]] f32 Dt(C channel = kDefaultTimeChannel) const noexcept {
        return m_time.GetScaledDt(channel);
    }

    [[nodiscard]] u64 TickIndex() const noexcept {
        return m_time.GetSimTickCount();
    }

    template <typename C = u8>
    void RequestHitstop(C channel, u32 ticks) noexcept {
        m_time.Hitstop().Request(channel, ticks);
    }

    template <typename C = u8>
    void SetTimeScale(C channel, f32 scale) noexcept {
        m_time.SetTimeScale(channel, scale);
    }

    bool Emit(const SimEvent& event) noexcept {
        return m_events.Push(event);
    }

private:
    TimeManager& m_time;
    SimEvents& m_events;
};

} // namespace EngineN

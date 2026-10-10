#include "platform/input/ActionBuffer.h"

namespace EngineN {

void ActionBuffer::Feed(const TickInput& in, bool frozen) noexcept {
    for (ActionId a = 0; a < kMaxActions; ++a) {
        const u32 bit = (1u << a);

        // Edge arrived this tick: arm pending and reset age
        if (in.Pressed(a)) {
            m_pressPending |= bit;
            m_pressAge[a] = 0;
        } else if (!frozen && (m_pressPending & bit) != 0) {
            // Age existing pending press unless frozen by hitstop
            if (m_pressAge[a] < 255) {
                ++m_pressAge[a];
            }
        }

        if (in.Released(a)) {
            m_releasePending |= bit;
            m_releaseAge[a] = 0;
        } else if (!frozen && (m_releasePending & bit) != 0) {
            if (m_releaseAge[a] < 255) {
                ++m_releaseAge[a];
            }
        }
    }
}

bool ActionBuffer::Peek(ActionId a, u8 windowTicks) const noexcept {
    if (a >= kMaxActions) return false;
    const u32 bit = (1u << a);
    return ((m_pressPending & bit) != 0) && (m_pressAge[a] <= windowTicks);
}

bool ActionBuffer::Consume(ActionId a, u8 windowTicks) noexcept {
    if (!Peek(a, windowTicks)) return false;
    m_pressPending &= ~(1u << a);
    return true;
}

bool ActionBuffer::ConsumeRelease(ActionId a, u8 windowTicks) noexcept {
    if (a >= kMaxActions) return false;
    const u32 bit = (1u << a);
    if (((m_releasePending & bit) != 0) && (m_releaseAge[a] <= windowTicks)) {
        m_releasePending &= ~bit;
        return true;
    }
    return false;
}

void ActionBuffer::Clear() noexcept {
    m_pressPending = 0;
    m_releasePending = 0;
    m_pressAge.fill(0);
    m_releaseAge.fill(0);
}

void ActionBuffer::Clear(ActionId a) noexcept {
    if (a >= kMaxActions) return;
    const u32 bit = (1u << a);
    m_pressPending &= ~bit;
    m_releasePending &= ~bit;
    m_pressAge[a] = 0;
    m_releaseAge[a] = 0;
}

} // namespace EngineN

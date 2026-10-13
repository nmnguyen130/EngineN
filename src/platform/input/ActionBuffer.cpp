#include "platform/input/ActionBuffer.h"
#include <bit>

namespace EngineN {

void ActionBuffer::FeedImpl(Channel& ch, u32 edges, bool frozen) noexcept {
    edges &= kActionMask;

    for (u32 m = edges; m != 0; m &= m - 1) {
        ch.age[static_cast<usize>(std::countr_zero(m))] = 0;
    }
    ch.pending |= edges;

    if (frozen) {
        return;
    }

    for (u32 m = ch.pending & ~edges; m != 0; m &= m - 1) {
        const usize a = static_cast<usize>(std::countr_zero(m));
        if (++ch.age[a] > kMaxBufferTicks) {
            ClearImpl(ch, a);
        }
    }
}

void ActionBuffer::Feed(const TickInput& in, bool frozen) noexcept {
    FeedImpl(m_press, in.pressed, frozen);
    FeedImpl(m_release, in.released, frozen);
}

void ActionBuffer::Clear() noexcept {
    m_press = {};
    m_release = {};
}

bool ActionBuffer::PeekImpl(const Channel& ch, usize a, u8 window) noexcept {
    return a < kMaxActions && ((ch.pending >> a) & 1u) != 0 && ch.age[a] <= window;
}

bool ActionBuffer::ConsumeImpl(Channel& ch, usize a, u8 window) noexcept {
    if (!PeekImpl(ch, a, window)) {
        return false;
    }
    ClearImpl(ch, a);
    return true;
}

void ActionBuffer::ClearImpl(Channel& ch, usize a) noexcept {
    if (a >= kMaxActions) {
        return;
    }
    ch.pending &= ~(1u << a);
    ch.age[a] = 0;
}

} // namespace EngineN

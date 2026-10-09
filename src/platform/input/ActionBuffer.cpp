#include "platform/input/ActionBuffer.h"
#include "core/Assert.h"
#include <SDL3/SDL.h>

namespace EngineN {

void ActionBuffer::RecordAction(PlayerAction action, u64 timestampNs) noexcept {
    m_buffer[m_head] = BufferedAction{
        .action = action,
        .timestampNs = timestampNs,
        .consumed = false
    };

    m_head = (m_head + 1) % kMaxBufferedActions;
    if (m_count < kMaxBufferedActions) {
        m_count++;
    }
}

bool ActionBuffer::ConsumeAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) noexcept {
    // Search in reverse-chronological order (freshest intent first)
    for (usize i = 0; i < m_count; ++i) {
        usize idx = (m_head + kMaxBufferedActions - 1 - i) % kMaxBufferedActions;
        auto& entry = m_buffer[idx];

        if (entry.action == action && !entry.consumed) {
            u64 elapsedNs = (currentTimestampNs >= entry.timestampNs)
                ? (currentTimestampNs - entry.timestampNs)
                : 0;

            if (elapsedNs <= maxWindowNs) {
                entry.consumed = true;

                // Anti-ghosting guarantee: Invalidate all older unconsumed intents for this same action
                // so rapid taps or old buffered presses do not trigger an immediate unintended duplicate.
                for (usize j = i + 1; j < m_count; ++j) {
                    usize olderIdx = (m_head + kMaxBufferedActions - 1 - j) % kMaxBufferedActions;
                    if (m_buffer[olderIdx].action == action) {
                        m_buffer[olderIdx].consumed = true;
                    }
                }

                return true;
            }
        }
    }

    return false;
}

bool ActionBuffer::ConsumeAction(PlayerAction action, u64 maxWindowNs) noexcept {
    return ConsumeAction(action, SDL_GetTicksNS(), maxWindowNs);
}

bool ActionBuffer::HasAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) const noexcept {
    for (usize i = 0; i < m_count; ++i) {
        usize idx = (m_head + kMaxBufferedActions - 1 - i) % kMaxBufferedActions;
        const auto& entry = m_buffer[idx];

        if (entry.action == action && !entry.consumed) {
            u64 elapsedNs = (currentTimestampNs >= entry.timestampNs)
                ? (currentTimestampNs - entry.timestampNs)
                : 0;

            if (elapsedNs <= maxWindowNs) {
                return true;
            }
        }
    }

    return false;
}

bool ActionBuffer::HasAction(PlayerAction action, u64 maxWindowNs) const noexcept {
    return HasAction(action, SDL_GetTicksNS(), maxWindowNs);
}

u64 ActionBuffer::GetActionAgeNs(PlayerAction action, u64 currentTimestampNs) const noexcept {
    for (usize i = 0; i < m_count; ++i) {
        usize idx = (m_head + kMaxBufferedActions - 1 - i) % kMaxBufferedActions;
        const auto& entry = m_buffer[idx];

        if (entry.action == action && !entry.consumed) {
            return (currentTimestampNs >= entry.timestampNs)
                ? (currentTimestampNs - entry.timestampNs)
                : 0;
        }
    }

    return UINT64_MAX;
}

void ActionBuffer::Clear() noexcept {
    m_head = 0;
    m_count = 0;
}

void ActionBuffer::ClearAction(PlayerAction action) noexcept {
    for (usize i = 0; i < m_count; ++i) {
        usize idx = (m_head + kMaxBufferedActions - 1 - i) % kMaxBufferedActions;
        if (m_buffer[idx].action == action) {
            m_buffer[idx].consumed = true;
        }
    }
}

const BufferedAction& ActionBuffer::GetEntry(usize indexFromNewest) const noexcept {
    ENGINE_ASSERT(indexFromNewest < m_count, "Index out of range for ActionBuffer");
    usize idx = (m_head + kMaxBufferedActions - 1 - indexFromNewest) % kMaxBufferedActions;
    return m_buffer[idx];
}

} // namespace EngineN

#include "platform/input/ActionBuffer.h"
#include <SDL3/SDL.h>

namespace EngineN {

void ActionBuffer::RecordAction(InputAction action, u64 timestampNs) noexcept {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return;

    m_entries[idx] = Entry{
        .timestampNs = timestampNs,
        .pending = true
    };
}

bool ActionBuffer::ConsumeAction(
    InputAction action,
    u64 currentTimestampNs,
    u64 maxWindowNs) noexcept {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return false;

    Entry& entry = m_entries[idx];
    if (!entry.pending || currentTimestampNs < entry.timestampNs) {
        return false;
    }

    if (currentTimestampNs - entry.timestampNs <= maxWindowNs) {
        entry.pending = false;
        return true;
    }

    // Expired intent is cleared
    entry.pending = false;
    return false;
}

bool ActionBuffer::ConsumeAction(InputAction action, u64 maxWindowNs) noexcept {
    return ConsumeAction(action, SDL_GetTicksNS(), maxWindowNs);
}

bool ActionBuffer::HasAction(
    InputAction action,
    u64 currentTimestampNs,
    u64 maxWindowNs) const noexcept {
    usize idx = static_cast<usize>(action);
    if (idx >= kActionCount) return false;

    const Entry& entry = m_entries[idx];
    if (!entry.pending || currentTimestampNs < entry.timestampNs) {
        return false;
    }

    return (currentTimestampNs - entry.timestampNs <= maxWindowNs);
}

bool ActionBuffer::HasAction(InputAction action, u64 maxWindowNs) const noexcept {
    return HasAction(action, SDL_GetTicksNS(), maxWindowNs);
}

void ActionBuffer::Clear() noexcept {
    m_entries.fill(Entry{});
}

void ActionBuffer::ClearAction(InputAction action) noexcept {
    usize idx = static_cast<usize>(action);
    if (idx < kActionCount) {
        m_entries[idx] = Entry{};
    }
}

} // namespace EngineN

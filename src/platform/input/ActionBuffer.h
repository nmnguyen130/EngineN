#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

namespace EngineN {

/**
 * ActionBuffer: Input forgiveness and action intent buffering for Metroidvania actions.
 * Retains the latest timestamped intent per action within a sliding window (e.g. 100ms),
 * enforcing single-consumption anti-ghosting to prevent duplicate triggers.
 */
class ActionBuffer {
public:
    ActionBuffer() = default;

    /// Record a press event timestamp into the ring buffer.
    void RecordAction(InputAction action, u64 timestampNs) noexcept;

    /// Attempts to consume an intent within [currentTimestampNs - maxWindowNs, currentTimestampNs].
    [[nodiscard]] bool ConsumeAction(
        InputAction action,
        u64 currentTimestampNs,
        u64 maxWindowNs) noexcept;
    [[nodiscard]] bool ConsumeAction(
        InputAction action,
        u64 maxWindowNs = kDefaultBufferWindowNs) noexcept;

    [[nodiscard]] bool HasAction(
        InputAction action,
        u64 currentTimestampNs,
        u64 maxWindowNs) const noexcept;
    [[nodiscard]] bool HasAction(
        InputAction action,
        u64 maxWindowNs = kDefaultBufferWindowNs) const noexcept;

    void Clear() noexcept;
    void ClearAction(InputAction action) noexcept;

private:
    struct Entry {
        u64 timestampNs = 0;
        bool pending = false;
    };

    std::array<Entry, kActionCount> m_entries{};
};

} // namespace EngineN

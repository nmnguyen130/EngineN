#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include <array>

namespace EngineN {

/// A recorded player intent stored with hardware timestamp and consumption state.
struct BufferedAction {
    PlayerAction action = PlayerAction::Jump;
    u64 timestampNs = 0;   // SDL3 hardware timestamp in nanoseconds
    bool consumed = false;  // True if game logic has already consumed this intent
};

/// High-Level Metroidvania Action Buffering & Forgiveness Subsystem.
/// Implements an allocation-free circular ring buffer with timestamped action intents.
/// Supports sliding window consumption (e.g., Jump Buffering before landing, Dash Buffering
/// during attack recovery) with anti-ghosting guarantees.
/// Fully compatible with 120Hz deterministic simulation ticks. Zero heap allocations.
class ActionBuffer {
public:
    ActionBuffer() = default;
    ~ActionBuffer() = default;

    /// Records an action press event with its sub-frame hardware timestamp.
    void RecordAction(PlayerAction action, u64 timestampNs) noexcept;

    /// Attempts to consume a buffered action intent within a maximum forgiveness window.
    /// Matches the freshest unconsumed intent within [currentTimestampNs - maxWindowNs, currentTimestampNs].
    /// Automatically invalidates older unconsumed intents of the same action to prevent ghost double-actions.
    /// Returns true if an intent was found, valid, and successfully consumed.
    [[nodiscard]] bool ConsumeAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) noexcept;

    /// Convenience overload using SDL3 current hardware ticks as current timestamp.
    [[nodiscard]] bool ConsumeAction(PlayerAction action, u64 maxWindowNs = kDefaultBufferWindowNs) noexcept;

    /// Checks if a buffered action intent exists within the window without consuming it.
    [[nodiscard]] bool HasAction(PlayerAction action, u64 currentTimestampNs, u64 maxWindowNs) const noexcept;

    /// Convenience overload for HasAction using SDL3 current hardware ticks.
    [[nodiscard]] bool HasAction(PlayerAction action, u64 maxWindowNs = kDefaultBufferWindowNs) const noexcept;

    /// Returns the age in nanoseconds of the freshest unconsumed intent for an action.
    /// Returns UINT64_MAX if no unconsumed intent exists.
    [[nodiscard]] u64 GetActionAgeNs(PlayerAction action, u64 currentTimestampNs) const noexcept;

    /// Invalidate all active buffered intents (e.g. upon entering Hurt / Stunned state).
    void Clear() noexcept;

    /// Invalidate all active buffered intents for a specific action.
    void ClearAction(PlayerAction action) noexcept;

    [[nodiscard]] usize GetCount() const noexcept { return m_count; }

    /// Access buffered entries in reverse-chronological order (0 = newest recorded, 1 = second newest, etc.).
    [[nodiscard]] const BufferedAction& GetEntry(usize indexFromNewest) const noexcept;

private:
    std::array<BufferedAction, kMaxBufferedActions> m_buffer{};
    usize m_head = 0;   // Index where next action will be written
    usize m_count = 0;  // Number of active entries currently stored in ring buffer
};

} // namespace EngineN

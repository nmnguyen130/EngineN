#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "platform/input/InputBindings.h"
#include "platform/input/InputTypes.h"
#include <array>
#include <span>

union SDL_Event;

namespace EngineN {

/**
 * FixedRingQueue: Bounded ring buffer for timestamped transitions.
 * Pre-allocated with zero heap allocation.
 */
template <typename T, usize Capacity>
class FixedRingQueue {
    static_assert(Capacity >= 2, "ring must hold at least one down/up pair");

public:
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_count == 0; }
    [[nodiscard]] constexpr bool Full() const noexcept { return m_count == Capacity; }

    [[nodiscard]] const T& Front() const noexcept { return m_data[m_head]; }
    [[nodiscard]] const T& Back() const noexcept {
        return m_data[(m_head + m_count - 1) % Capacity];
    }

    T Pop() noexcept {
        T val = m_data[m_head];
        m_head = (m_head + 1) % Capacity;
        --m_count;
        return val;
    }

    bool Push(const T& val) noexcept {
        const bool dropped = Full();
        if (dropped) {
            DropOldestPair();
        }
        m_data[(m_head + m_count) % Capacity] = val;
        ++m_count;
        return dropped;
    }

    void DropOldestPair() noexcept {
        const usize drop = (m_count >= 2) ? 2 : m_count;
        m_head = (m_head + drop) % Capacity;
        m_count -= drop;
    }

    void Clear() noexcept {
        m_head = 0;
        m_count = 0;
    }

private:
    std::array<T, Capacity> m_data{};
    usize m_head = 0;
    usize m_count = 0;
};

/**
 * InputSystem: Central facade owning bindings, device states, context stack, and frame snapshots.
 * Single-threaded: Ingest, CutTick, PublishFrameInput, and bindings must run on the same thread.
 */
class InputSystem {
public:
    InputSystem() noexcept;

    void Ingest(const SDL_Event& event) noexcept;

    // Simulation tick slicing
    [[nodiscard]] TickInput CutTick() noexcept;
    [[nodiscard]] TickInput CutTick(u64 tickIndex, u64 boundaryNs) noexcept;

    // Fast-forwards simulation to newBoundaryNs without emitting frame events.
    void Coalesce(u64 newBoundaryNs) noexcept;

    // Frame snapshots
    [[nodiscard]] const FrameInput& PublishFrameInput() noexcept;
    [[nodiscard]] const FrameInput& GetCurrentFrameInput() const noexcept { return m_publishedFrame; }

    // Context stack
    template <typename C = ContextId>
    [[nodiscard]] Result<void, InputError> PushContext(C ctx, bool consumes = true) noexcept {
        return PushContextImpl(static_cast<ContextId>(ctx), consumes);
    }

    [[nodiscard]] Result<void, InputError> PopContext() noexcept;

    template <typename C = ContextId>
    [[nodiscard]] C GetActiveContext() const noexcept {
        return static_cast<C>((m_contextStackDepth == 0) ? 0 : m_contextStack[m_contextStackDepth - 1].context);
    }

    // Bindings
    [[nodiscard]] const InputBindings& GetBindings() const noexcept { return m_bindings; }
    [[nodiscard]] InputBindings& GetBindings() noexcept { return m_bindings; }

    // Re-evaluates all held actions against current bindings and context stack.
    void ResyncActionStates(u64 nowNs = 0) noexcept;

    // Diagnostics
    [[nodiscard]] u32 GetDroppedTransitionsCount() const noexcept { return m_droppedTransitions; }
    [[nodiscard]] bool IsSourcePhysicallyDown(Source src) const noexcept;

private:
    static constexpr usize kMaxGamepads = 4;

    struct Transition {
        u64 tsNs = 0;
        bool down = false;
    };

    struct ContextLayer {
        ContextId context = 0;
        bool consumes = true;
    };

    struct GamepadDevice {
        static constexpr usize kMaxAxes = 6; // SDL axis count

        u32 instanceId = 0;
        bool connected = false;
        u32 buttonsDown = 0;
        std::array<i16, kMaxAxes> axes{};
    };

    [[nodiscard]] Result<void, InputError> PushContextImpl(ContextId ctx, bool consumes) noexcept;

    // Raw device state
    void SetButton(Source src, bool down, u64 tsNs) noexcept;
    [[nodiscard]] usize FindGamepad(u32 instanceId) const noexcept;
    [[nodiscard]] usize FindOrCreateGamepad(u32 instanceId) noexcept;
    void RemoveGamepad(u32 instanceId, u64 tsNs) noexcept;
    void ReleaseAll(u64 tsNs) noexcept;

    // Contexts and routing
    [[nodiscard]] bool IsContextActive(ContextId ctx) const noexcept;
    [[nodiscard]] bool ActionHasActiveSource(ActionId action) const noexcept;
    void RouteSource(Source src, bool down, u64 tsNs) noexcept;
    [[nodiscard]] bool RouteInContext(ContextId ctx, Source src, bool down, u64 tsNs) noexcept;
    void SyncBindings() noexcept;

    // Action state ledgers
    void SetAction(ActionId action, bool down, u64 tsNs, bool synthetic) noexcept;
    void PushTransition(ActionId action, u64 tsNs, bool down) noexcept;
    void Drain(u64 boundaryNs, u32& pressed, u32& released) noexcept;

    // Axis evaluation
    void ResolveContextAxes(ContextId ctx, u32& resolved, std::array<i16, kMaxAxes>& out) const noexcept;
    [[nodiscard]] std::array<i16, kMaxAxes> ResolveAxes() const noexcept;
    [[nodiscard]] f32 EvaluateAxis(const AxisBinding& b) const noexcept;

    InputBindings m_bindings{};
    u32 m_seenRevision = 0;

    std::array<ContextLayer, kMaxContexts> m_contextStack{};
    usize m_contextStackDepth = 0;

    std::array<bool, Source::kMaxScancodes> m_keysDown{};
    std::array<bool, Source::kMaxMouseButtons> m_mouseDown{};
    std::array<GamepadDevice, kMaxGamepads> m_gamepads{};
    std::array<i8, kMaxAxes> m_lastAxisDir{};

    std::array<FixedRingQueue<Transition, 8>, kMaxActions> m_queues{};
    u32 m_droppedTransitions = 0;

    u32 m_simHeld = 0;
    std::array<u16, kMaxActions> m_simHeldTicks{};
    u64 m_autoTickIndex = 0;
    u64 m_autoBoundaryNs = 0;

    FrameInput m_publishedFrame{};
    FrameInput m_accumFrame{};
};

/**
 * ScopedContext: RAII helper for scoped context activation.
 * Automatically pops on destruction if activation succeeded.
 */
class ScopedContext {
public:
    template <typename C = ContextId>
    ScopedContext(InputSystem& input, C ctx, bool consumes = true) noexcept
        : m_input(input), m_active(input.PushContext(ctx, consumes).HasValue()) {}

    ~ScopedContext() noexcept {
        if (m_active) {
            (void)m_input.PopContext();
        }
    }

    [[nodiscard]] bool IsActive() const noexcept { return m_active; }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    InputSystem& m_input;
    bool m_active = false;
};

} // namespace EngineN

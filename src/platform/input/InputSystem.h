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
 * Ring buffer queue for timestamped action transitions.
 * Strictly bounded and pre-allocated to guarantee zero heap allocations.
 */
template <typename T, usize Capacity>
class FixedRingQueue {
public:
    [[nodiscard]] constexpr bool Empty() const noexcept { return m_count == 0; }
    [[nodiscard]] constexpr bool Full() const noexcept { return m_count == Capacity; }

    [[nodiscard]] const T& Front() const noexcept { return m_data[m_head]; }
    [[nodiscard]] const T& Back() const noexcept {
        const usize tail = (m_head + m_count - 1 + Capacity) % Capacity;
        return m_data[tail];
    }

    T Pop() noexcept {
        T val = m_data[m_head];
        m_head = (m_head + 1) % Capacity;
        --m_count;
        return val;
    }

    void Push(const T& val) noexcept {
        const usize tail = (m_head + m_count) % Capacity;
        m_data[tail] = val;
        ++m_count;
    }

    void DropOldestPair() noexcept {
        if (m_count >= 2) {
            m_head = (m_head + 2) % Capacity;
            m_count -= 2;
        }
    }

private:
    std::array<T, Capacity> m_data{};
    usize m_head = 0;
    usize m_count = 0;
};

struct Transition {
    u64 tsNs = 0;
    bool down = false;
};

struct ContextLayer {
    ContextId context = 0;
    bool consumes = true;
};

/**
 * Central Input Subsystem Facade (EngineN::InputSystem / EngineN::Input).
 * Owns bindings, device states, context stack, transition slicing, and frame snapshots.
 */
class InputSystem {
public:
    InputSystem() noexcept;

    // Direct event ingestion from SDL event pump
    void Ingest(const SDL_Event& event) noexcept;
    void ProcessEvent(const SDL_Event& event) noexcept { Ingest(event); }

    // Simulation tick cutting at fixed boundary (120Hz)
    [[nodiscard]] TickInput CutTick() noexcept;
    [[nodiscard]] TickInput CutTick(u64 tickIndex) noexcept;
    [[nodiscard]] TickInput CutTick(u64 tickIndex, u64 boundaryNs) noexcept;

    // Spiral-of-death catch-up coalescing
    void Coalesce(u64 newBoundaryNs) noexcept;

    // Render frame input snapshot publisher
    [[nodiscard]] const FrameInput& PublishFrameInput() noexcept;
    [[nodiscard]] const FrameInput& GetCurrentFrameInput() const noexcept { return m_frameInput; }

    // Context management stack
    [[nodiscard]] Result<void, InputError> PushContext(ContextId ctx, bool consumes = true) noexcept;
    [[nodiscard]] Result<void, InputError> PopContext() noexcept;
    [[nodiscard]] ContextId GetActiveContext() const noexcept;

    template <typename EContext>
    requires std::is_enum_v<EContext>
    [[nodiscard]] Result<void, InputError> PushContext(EContext ctx, bool consumes = true) noexcept {
        return PushContext(static_cast<ContextId>(ctx), consumes);
    }

    // Direct access to binding tables
    [[nodiscard]] const InputBindings& GetBindings() const noexcept { return m_bindings; }
    [[nodiscard]] InputBindings& GetBindings() noexcept { return m_bindings; }

    // Resynchronize actions after focus change or rebind
    void ResyncActionStates(u64 nowNs) noexcept;

    // Diagnostic queries
    [[nodiscard]] u32 GetDroppedTransitionsCount() const noexcept { return m_droppedTransitions; }
    [[nodiscard]] bool IsSourcePhysicallyDown(Source src) const noexcept;

private:
    [[nodiscard]] bool IsContextActive(ContextId ctx) const noexcept;
    void RouteSource(Source src, bool down, u64 timestampNs) noexcept;
    void RecordFrameEvent(u64 timestampNs, ActionId action, Edge edge, bool synthetic) noexcept;
    void PushTransition(ActionId action, u64 timestampNs, bool down) noexcept;

    InputBindings m_bindings{};

    std::array<ContextLayer, kMaxContexts> m_contextStack{};
    usize m_contextStackDepth = 0;

    // Physical device states and latch masks to prevent synthetic edges
    std::array<bool, kMaxKeyboardKeys> m_keysDown{};
    std::array<bool, kMaxKeyboardKeys> m_keysLatched{};
    std::array<bool, kMaxMouseButtons> m_mouseDown{};
    std::array<bool, kMaxMouseButtons> m_mouseLatched{};
    std::array<bool, kMaxGamepadButtons> m_gamepadDown{};
    std::array<bool, kMaxGamepadButtons> m_gamepadLatched{};

    // TakeNewer digital axis timestamps
    std::array<u64, kMaxAxes> m_lastNegTs{};
    std::array<u64, kMaxAxes> m_lastPosTs{};

    // Per-action transition queues
    std::array<FixedRingQueue<Transition, 8>, kMaxActions> m_queues{};
    u32 m_droppedTransitions = 0;

    // Simulation tick state
    u32 m_simHeld = 0;
    std::array<u16, kMaxActions> m_simHeldTicks{};
    u64 m_autoTickIndex = 0;
    u64 m_autoBoundaryNs = 0;

    // Frame state
    FrameInput m_frameInput{};
    u32 m_frameHeld = 0;
    u32 m_framePressed = 0;
    u32 m_frameReleased = 0;
    Vec2 m_currentMousePos{0.0f, 0.0f};
    Vec2 m_accumMouseDelta{0.0f, 0.0f};
    Vec2 m_accumMouseWheel{0.0f, 0.0f};
    u64 m_frameIndex = 0;
};

/**
 * RAII helper for scoped context activation.
 */
class ScopedContext {
public:
    ScopedContext(InputSystem& input, ContextId ctx, bool consumes = true) noexcept
        : m_input(input) {
        (void)m_input.PushContext(ctx, consumes);
    }

    template <typename EContext>
    requires std::is_enum_v<EContext>
    ScopedContext(InputSystem& input, EContext ctx, bool consumes = true) noexcept
        : ScopedContext(input, static_cast<ContextId>(ctx), consumes) {}

    ~ScopedContext() noexcept {
        (void)m_input.PopContext();
    }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    InputSystem& m_input;
};

} // namespace EngineN

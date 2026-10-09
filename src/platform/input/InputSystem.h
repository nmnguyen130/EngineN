#pragma once

#include "core/Types.h"
#include "platform/input/InputTypes.h"
#include "platform/input/ActionBuffer.h"
#include "platform/input/InputBindings.h"
#include <array>

union SDL_Event;

namespace EngineN {

/**
 * InputSystem: Keyboard and mouse input coordinator for EngineN.
 * Maps physical events to logical actions, handles fixed-tick latching,
 * and manages sliding-window action buffering.
 */
class InputSystem {
public:
    InputSystem();
    ~InputSystem() = default;

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;
    InputSystem(InputSystem&&) noexcept = default;
    InputSystem& operator=(InputSystem&&) noexcept = default;

    // Frame & Tick Lifecycle
    void BeginFrame() noexcept;
    void ProcessEvent(const SDL_Event& event) noexcept;
    void BeginFixedTick() noexcept;

    // Action Queries
    [[nodiscard]] bool IsActionDown(InputAction action) const noexcept;
    [[nodiscard]] bool WasActionPressedThisFrame(InputAction action) const noexcept;
    [[nodiscard]] bool WasActionReleasedThisFrame(InputAction action) const noexcept;
    [[nodiscard]] bool WasActionPressedThisTick(InputAction action) const noexcept;
    [[nodiscard]] bool WasActionReleasedThisTick(InputAction action) const noexcept;

    [[nodiscard]] const ActionFrameState& GetActionFrameState(InputAction action) const noexcept;
    [[nodiscard]] const ActionFrameState& GetActionTickState(InputAction action) const noexcept;

    // Virtual Axes
    [[nodiscard]] f32 GetAxis(AxisId axis) const noexcept;

    // Hardware Polling
    [[nodiscard]] bool IsKeyDown(u32 scancode) const noexcept;
    [[nodiscard]] bool IsKeyUp(u32 scancode) const noexcept;
    [[nodiscard]] bool IsKeyJustPressed(u32 scancode) const noexcept;
    [[nodiscard]] bool IsKeyJustReleased(u32 scancode) const noexcept;

    [[nodiscard]] bool IsMouseButtonDown(u8 button) const noexcept;
    [[nodiscard]] bool IsMouseButtonJustPressed(u8 button) const noexcept;
    [[nodiscard]] bool IsMouseButtonJustReleased(u8 button) const noexcept;
    [[nodiscard]] f32 GetMouseX() const noexcept;
    [[nodiscard]] f32 GetMouseY() const noexcept;
    [[nodiscard]] f32 GetMouseDeltaX() const noexcept;
    [[nodiscard]] f32 GetMouseDeltaY() const noexcept;
    [[nodiscard]] f32 GetMouseWheelX() const noexcept;
    [[nodiscard]] f32 GetMouseWheelY() const noexcept;

    // Metroidvania Action Buffering
    [[nodiscard]] bool ConsumeAction(InputAction action, u64 maxWindowNs = kDefaultBufferWindowNs) noexcept;
    [[nodiscard]] bool ConsumeAction(InputAction action, u64 currentTimestampNs, u64 maxWindowNs) noexcept;
    [[nodiscard]] bool HasBufferedAction(InputAction action, u64 maxWindowNs = kDefaultBufferWindowNs) const noexcept;
    [[nodiscard]] bool HasBufferedAction(InputAction action, u64 currentTimestampNs, u64 maxWindowNs) const noexcept;
    void ClearActionBuffer() noexcept;
    void ClearBufferedAction(InputAction action) noexcept;

    // Sub-Frame Timestamped Action Log
    [[nodiscard]] usize GetTimestampedActionCount() const noexcept;
    [[nodiscard]] const TimestampedAction& GetTimestampedAction(usize index) const noexcept;

    // Context & Bindings
    bool SetActiveContext(InputContext context) noexcept;
    [[nodiscard]] InputContext GetActiveContext() const noexcept;

    void SetDefaultBindings() noexcept;
    void ClearBindings() noexcept;
    bool BindKey(u32 scancode, InputAction action, InputContext context = InputContext::Gameplay) noexcept;
    bool UnbindKey(u32 scancode, InputAction action, InputContext context = InputContext::Gameplay) noexcept;
    bool BindMouseButton(u8 button, InputAction action, InputContext context = InputContext::Gameplay) noexcept;
    bool UnbindMouseButton(u8 button, InputAction action, InputContext context = InputContext::Gameplay) noexcept;
    bool BindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context = InputContext::Gameplay) noexcept;
    bool UnbindAxisKeys(AxisId axis, u32 negativeScancode, u32 positiveScancode, InputContext context = InputContext::Gameplay) noexcept;

    [[nodiscard]] const InputBindings& GetBindings() const noexcept { return m_bindings; }
    [[nodiscard]] InputBindings& GetBindings() noexcept { return m_bindings; }

    [[nodiscard]] const ActionBuffer& GetActionBuffer() const noexcept { return m_actionBuffer; }
    [[nodiscard]] ActionBuffer& GetActionBuffer() noexcept { return m_actionBuffer; }

private:
    void ReleaseKeyboardAndMouse(u64 timestampNs) noexcept;
    void RecomputeActionStates(u64 timestampNs) noexcept;
    void PushTimestampedAction(InputAction action, u64 timestampNs, bool pressed) noexcept;

    // Subsystems
    ActionBuffer m_actionBuffer;
    InputBindings m_bindings;

    // Physical snapshots
    std::array<bool, kMaxKeyboardKeys> m_keysDown{};
    std::array<bool, kMaxKeyboardKeys> m_keysPressedFrame{};
    std::array<bool, kMaxKeyboardKeys> m_keysReleasedFrame{};

    std::array<bool, kMaxMouseButtons> m_mouseButtonsDown{};
    std::array<bool, kMaxMouseButtons> m_mousePressedFrame{};
    std::array<bool, kMaxMouseButtons> m_mouseReleasedFrame{};
    f32 m_mouseX = 0.0f;
    f32 m_mouseY = 0.0f;
    f32 m_mouseDeltaX = 0.0f;
    f32 m_mouseDeltaY = 0.0f;
    f32 m_mouseWheelX = 0.0f;
    f32 m_mouseWheelY = 0.0f;

    // Context & Action states
    InputContext m_activeContext = InputContext::Gameplay;
    std::array<ActionFrameState, kActionCount> m_frameStates{};
    std::array<ActionFrameState, kActionCount> m_tickStates{};
    std::array<bool, kActionCount> m_pendingPressed{};
    std::array<bool, kActionCount> m_pendingReleased{};

    // Sub-frame event log
    std::array<TimestampedAction, kMaxFrameLogActions> m_frameLog{};
    usize m_frameLogCount = 0;
};

} // namespace EngineN

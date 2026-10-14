# EngineN — Platform Input Subsystem Context (`platform/input/`)

## 1. Architecture & Invariants
- **Standard:** C++23, zero heap allocation, 100% `constexpr` header-only core (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`) + isolated SDL3 event adapter (`SdlInput.h/.cpp`), SOLID/ISP/DOD compliant.
- **Pull-Based Evaluation (Zero Stuck-Key Guarantee):** Physical hardware states (`m_keyBits`, `m_mouseBits`, `m_pads`) are strictly decoupled from logical actions. `EvaluateLiveHeld()` computes active actions via bitwise operations against `InputMap` and `m_actionMask` on each tick/frame, eliminating event-routing state machines and preventing stuck inputs on binding or context changes.
- **Sub-Frame Tap Preservation (`m_latchedTap`):** Rapid press-and-release transitions occurring within the same render frame (< 8.33ms) are latched on physical key-down and consumed on the next `PollTick()`. This guarantees `pressed = 1, held = 1, released = 0` on Tick $N$ and `released = 1, held = 0` on Tick $N+1$ (`(pressed & released) == 0` invariant).
- **SOCD & Deadzones:** Built-in Simultaneous Opposite Cardinal Directions (`SocdPolicy::LastInputWins` and `SocdPolicy::Neutral`), per-axis rescaled axial deadzone (`ApplyAxialDeadzone`), and 2D stick radial deadzone (`ApplyRadialDeadzone`).
- **Encapsulation:** Zero gameplay action names or hardcoded keybindings inside the engine layer; `<SDL3/SDL.h>` is strictly confined to `SdlInput.cpp`.

## 2. Core Types (`platform/input/InputTypes.h`)
- **Constants & Limits:** `kMaxActions = 16`, `kMaxAxes = 4`, `kMaxBindingsPerSlot = 4`, `kMaxGamepads = 4`.
- **Concept & Helpers:**
  - `InputId<T>`: Accepts any unsigned integral or `u8`/`u16`/`u32`-backed scoped enum. Converted via `constexpr u8 ToInputIndex(InputId auto id) noexcept`.
  - `constexpr u16 MakeActionMask(InputId auto... actions) noexcept`: Builds bitmasks for context filtering.
- **Physical Descriptor (`sizeof(Source) == 4`):**
  - `struct Source { Device device; u8 gamepadIndex; u16 code; };` (`kAnyGamepad = 0xFF`, `kMaxScancodes = 512`, `kMaxMouseButtons = 8`, `kMaxGamepadButtons = 32`).
  - Factories: `Source::Key(u16 scancode)`, `Source::Mouse(u16 button)`, `Source::Pad(u16 button, u8 padSlot = kAnyGamepad)`.
- **Direct-Indexed Binding Table (`InputMap`):**
  - `std::array<std::array<Source, 4>, 16> actions` & `std::array<AxisBinding, 4> axes`.
  - Fluent builders: `Bind(action, {sources...})`, `BindSlot(action, slot, src)`, `BindAxis(axis, AxisBinding{...})`.
- **L1 Cache-Aligned Snapshots (`sizeof == 64`):**
  - `struct alignas(64) TickInput`: Immutable 120Hz simulation snapshot (`u64 tick`, `u16 held/pressed/released`, `std::array<i16, 4> axis`, `std::array<u16, 16> heldTicks`).
  - `struct alignas(64) FrameInput`: Render-frame UI/cursor snapshot (`u64 frameIndex`, `u16 held/pressed/released`, `Device activeDevice`, `Source lastPressed`, `Vec2 mousePos/mouseDelta/mouseWheel`).

## 3. Public Interfaces & Signatures
### `TickInput` & `FrameInput` Queries (`platform/input/InputTypes.h`)
- `constexpr bool Held(InputId auto a) const noexcept`
- `constexpr bool Pressed(InputId auto a) const noexcept`
- `constexpr bool Released(InputId auto a) const noexcept`
- `constexpr u16 HeldTicks(InputId auto a) const noexcept` (`TickInput` only — monotonic hold counter `[0..65535]`)
- `constexpr f32 Axis(InputId auto ax) const noexcept` (`TickInput` only — normalized `[-1.0f, 1.0f]` from quantized `i16`)
- `Vec2 Stick(InputId auto axX, InputId auto axY, f32 radialDeadzone = 0.0f) const noexcept` (`TickInput` only)

### `ActionBuffer` (`platform/input/ActionBuffer.h` — Deterministic Intent Forgiveness)
- `static constexpr u8 kMaxBufferTicks = 60;` (500ms at 120Hz)
- `constexpr void Feed(const TickInput& in, bool channelFrozen = false) noexcept` (pauses buffer aging when `step.IsFrozen(ch)` is true)
- `constexpr bool Peek(InputId auto a, u8 windowTicks) const noexcept` / `PeekRelease(a, windowTicks)`
- `constexpr bool Consume(InputId auto a, u8 windowTicks) noexcept` / `ConsumeRelease(a, windowTicks)`
- `constexpr void Clear() noexcept`

### `InputPipeline` & `SdlInput` (`platform/input/InputPipeline.h` & `SdlInput.h`)
- `using InputSystem = InputPipeline;`
- Frame & Tick Lifecycle:
  - `constexpr void BeginFrame() noexcept` (resets per-frame deltas and edges prior to event polling)
  - `void IngestSdlEvent(InputPipeline& pipeline, const SDL_Event& event) noexcept` (translates OS events into physical state)
  - `[[nodiscard]] constexpr const FrameInput& EndFrame() noexcept` / `GetFrameView() const noexcept`
  - `[[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept` (consumes sub-frame taps, advances `heldTicks`, evaluates axes)
- Configuration & Hardware State:
  - `constexpr InputMap& Map() noexcept`, `constexpr void SetActionMask(u16 mask) noexcept`, `constexpr u16 GetActionMask() const noexcept`
  - `constexpr void FeedButton(Source src, bool down) noexcept`, `FeedGamepadButton(u32 id, u16 btn, bool down) noexcept`, `FeedGamepadAxis(u32 id, u8 ax, i16 val) noexcept`, `FeedMouseMotion(Vec2 pos, Vec2 delta) noexcept`, `FeedMouseWheel(Vec2 wheel) noexcept`
  - `constexpr void DisconnectGamepad(u32 id) noexcept`, `constexpr void ClearPhysicalState() noexcept`, `constexpr bool IsPhysicallyDown(Source src) const noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_input.BeginFrame();
SDL_Event ev;
while (SDL_PollEvent(&ev)) {
    IngestSdlEvent(m_input, ev);
}
const auto& frameInput = m_input.EndFrame();

m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const TickInput tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(TimeChannel::Player));
    m_world.Tick(step, tickInput, m_actionBuffer);
}
m_renderer.Render(m_world, m_time.GetRenderView(), frameInput);
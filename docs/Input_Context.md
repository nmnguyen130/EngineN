# EngineN — Input Subsystem Context (`engine/input/`)

## 1. Design & Invariants
- **Core:** C++23 header-only (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`), zero allocation, pull-evaluated.
- **Pull-Based State:** Physical bits (`m_keyBits`, `m_mouseBits`, `m_pads`) are evaluated on demand against `InputMap` and `m_actionMask`. No stuck keys on rebind or mask changes.
- **Sub-Frame Tap Latch (`m_latchedTap`):** Taps within a frame (< 8.33ms) are latched on key-down. `PollTick()` guarantees `pressed=1` at Tick $N$ and `released=1` at Tick $N+1$.
- **SOCD & Deadzone:** `SocdPolicy::LastInputWins` / `Neutral`; rescaled axial deadzone (`ApplyAxialDeadzone`) and stick radial deadzone (`ApplyRadialDeadzone`).

## 2. Core Layouts & Types (`InputTypes.h`)
- `Source` (4B POD): `Device device`, `u8 gamepadIndex` (`0xFF = Any`), `u16 code` (512 scancodes, 8 mouse btns, 32 pad btns).
- `InputMap`: Flat table `actions[16][4]` (up to 4 alternate `Source` per action) + `axes[4]` (`AxisBinding`).
- `TickInput` (`alignas(64)`, 64B): 120Hz snapshot. Fields: `tick`, `held`, `pressed`, `released`, `axis[4]`, `heldTicks[16]`. Invariant: `(pressed & released) == 0`.
- `FrameInput` (`alignas(64)`, 64B): UI/render snapshot. Fields: `frameIndex`, `held`, `pressed`, `released`, `activeDevice`, `lastPressed`, `mousePos`, `mouseDelta`, `mouseWheel`.

## 3. Subsystem APIs
### `ActionBuffer` (40B, Intent Forgiveness)
- `Feed(TickInput, channelFrozen)`: Ages pending intent up to 60 ticks. If `channelFrozen == true` (hitstop), age does not advance.
- `Peek(action, window)` / `PeekRelease`: Query without consuming.
- `Consume(action, window)` / `ConsumeRelease`: Query and clear.
- `ComputeAuditHash()`: 64-bit FNV-1a hash for netcode/replay validation.

### `InputPipeline` (Pull Evaluator)
- **Lifecycle:**
  - `BeginFrame()`: Clears per-frame deltas and edge flags.
  - `EndFrame()` $\to$ `const FrameInput&`: Evaluates variable-rate frame input.
  - `PollTick(tickIndex)` $\to$ `TickInput`: Consumes `m_latchedTap`, steps `heldTicks`, generates 120Hz tick input.
- **Ingestion (called by `Platform::Pump`):**
  - `FeedButton(Source, down)`, `FeedMouseMotion(pos, delta)`, `FeedMouseWheel(wheel)`
  - `FeedGamepadButton(instId, btn, down)`, `FeedGamepadAxis(instId, axis, val)`, `DisconnectGamepad(instId)`
- **Config & State:**
  - `Map()`: Accesses `InputMap`.
  - `SetActionMask(mask)` / `GetActionMask()`: Filters active actions.
  - `ClearPhysicalState()`, `IsPhysicallyDown(Source)`, `ComputeAuditHash()`.

## 4. Main Loop Integration
```cpp
// 1. OS Event pump (Platform::Pump calls input.BeginFrame/EndFrame and scales mouse by pixelDensity)
m_platform.Pump(m_window, m_input);

// 2. Deterministic 120Hz fixed simulation loop
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const TickInput tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);
}
```
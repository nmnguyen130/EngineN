# Phase 4 — `input` Subsystem (LOCKED — Final Industry Production v3)

## Overview
- **Namespace:** `en`
- **Dependencies:** `core/Math.h` only (zero `platform/` or OS header includes in `input/`)
- **Footprint:** 3 header-only files (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`) + 1 verification suite (`tests/InputTests.cpp`)
- **Zero Heap Allocations & Zero Compiler Padding (`0B` padding holes across the entire subsystem):**
  - `InputSource`: `4 bytes` (`alignas(4)`)
  - `AxisBinding`: `32 bytes` (`alignas(32)` — 2 per 64-byte cache line, zero cache-line straddling, zero internal/tail padding)
  - `InputMap`: `384 bytes` (`alignas(64)` — 6 cache lines exact: `256B` action matrix + `128B` axis array, zero tail padding)
  - `TickInput`: `64 bytes` (`alignas(64)` — 1 cache line exact)
  - `FrameInput`: `64 bytes` (`alignas(64)` — 1 cache line exact)
  - `ActionBuffer`: `64 bytes` (`alignas(64)` — 1 cache line exact: `40B` action press/release/double-tap buffer + `24B` 11-entry Numpad motion & charge ring buffer)
  - `InputReplayStream::Segment`: `12 bytes` (`alignas(4)`)
  - `InputPipeline`: `640 bytes` (`alignas(64)` — 10 cache lines exact: `64B` Cache Line 0 hot tick state + `64B` `FrameInput` + `64B` 512-key bitset + `64B` `4x16B` `GamepadState` + `384B` `InputMap`, zero hidden compiler tail padding)

## Locked Files & Public Contracts

### 1. `input/InputTypes.h`
- **`InputDevice`** (`u8`): `None`, `Keyboard`, `Mouse`, `Gamepad` (drives automatic UI prompt switching).
- **`SocdPolicy`** (`u8`): Simultaneous Opposite Cardinal Directions resolution:
  - `Neutral` (`0`): Opposite held keys cancel to `0.0f` (tournament hitbox standard).
  - `LastInputWins` (`1`): Most recently pressed direction wins; releasing it falls back to the still-held opposite direction.
  - `FirstInputWins` (`2`): First held direction retains priority when the opposite direction is pressed.
- **`InputSource`** (`4 bytes`, `alignas(4)`):
  - `device` (`InputDevice`), `gamepadIndex` (`u8`: `0..3` or `kAnyGamepad = 0xFF`), `code` (`u16`).
  - Factories:
    - `InputSource::Key(scancode)` (`[1..511]`)
    - `InputSource::Mouse(button)` (`[1..7]`)
    - `InputSource::Pad(button, padSlot)` (`[0..19]` physical buttons, `[20..31]` virtual axis/trigger buttons)
    - `InputSource::PadAxisButton(axisIdx, positive, padSlot)` (`[20..31]`, maps analog stick/trigger directions to digital buttons with Schmitt-trigger hysteresis: `code = 20 + axisIdx * 2 + (positive ? 1 : 0)`)
    - `InputSource::PadTrigger(rightTrigger, padSlot)` (`L2` = axis 4 positive = button `29`, `R2` = axis 5 positive = button `31`).
- **`AxisBinding`** (`32 bytes`, `alignas(32)`):
  - `negSource` (`4B`) + `posSource` (`4B`): Primary digital negative/positive sources (e.g. `A` / `D`).
  - `altNegSource` (`4B`) + `altPosSource` (`4B`): Alternate digital negative/positive sources (e.g. `Left` / `Right` or `D-Pad Left` / `D-Pad Right`), evaluated seamlessly alongside primary sources in SOCD resolution and sub-frame axis tap latching.
  - `deadzone` (`f32`, default `0.15f`): Inner deadzone threshold `[0, outerDeadzone)`.
  - `outerDeadzone` (`f32`, default `0.95f`): Outer saturation threshold `(deadzone, 1.0f]`.
  - `sensitivity` (`f32`, default `1.0f`): Output multiplier.
  - `gamepadAxis` (`u8`, `0..5` or `0xFF`), `padSlot` (`u8`), `socd` (`SocdPolicy`), `invert` (`bool`).
- **`InputMap`** (`384 bytes`, `alignas(64)`):
  - `BindSlot(action, slot, InputSource)` / `Bind(action, initializer_list<InputSource>)` / `UnbindAction(action)`
  - `BindAxis(axis, const AxisBinding&)` (sanitizes `negSource`, `posSource`, `altNegSource`, `altPosSource`, `deadzone`, and `outerDeadzone`)
  - `FindConflictingActions(InputSource, checkMask)` (returns 16-bit mask of conflicting actions)
  - `SerializeBindings(span<u8>)` / `DeserializeBindings(span<const u8>)` (`384 bytes` exact, sanitizes on load).
- **`TickInput`** (`64 bytes`, `alignas(64)`):
  - Immutable deterministic fixed-tick snapshot (`tick`, `held`, `pressed`, `released`, `retriggered`, `axis[4]`, `heldTicks[16]`).
  - Unreal Enhanced Input trigger queries:
    - `Held(a)`, `Pressed(a)`, `Released(a)`, `Retriggered(a)`, `HeldTicks(a)`, `Axis(ax)`
    - `HeldOneShot(a, holdThresholdTicks)`: Fires once on the exact tick `heldTicks[a] == holdThresholdTicks`.
    - `TapReleased(a, maxTapTicks)`: Fires on the release tick if `heldTicks[a] <= maxTapTicks` (distinguishes quick tap from hold on the same button, e.g. Dodge on tap vs Sprint on hold).
    - `Pulsed(a, initialDelayTicks, repeatIntervalTicks)`: Fires on initial press (`heldTicks == 1`), waits `initialDelayTicks`, and pulses every `repeatIntervalTicks` thereafter while held.
    - `ChargeRatio(a, fullChargeTicks)` & `WasChargedRelease(a, minChargeTicks)`: Continuous `[0..1]` charge ratio and release-tick charge trigger (`heldTicks[a]` preserves final held duration on pure release ticks).
  - 2D Stick & Directional helpers:
    - `ApplyRadialDeadzone(raw, innerDz, outerDz, responseCurve)` / `ApplyAxialDeadzone(raw, dzX, dzY)`
    - `SnapOctagonal(raw, deadzone)` / `OctagonalToNumpad(oct)`
    - `Stick(axX, axY, extraRadialDeadzone, outerDeadzone, responseCurve)`: Unit-circle clamped 2D vector without double-deadzoning by default.
    - `StickOctagonal(axX, axY, deadzone)`: Quantizes 2D stick into 8-way `IVec2`.
    - `StickNumpad(axX, axY, deadzone)`: Quantizes 2D stick into fighting-game Numpad notation (`1..9`, `5 = neutral`).
- **`FrameInput`** (`64 bytes`, `alignas(64)`):
  - Per-render-frame cursor, UI & text input (`frameIndex`, `mousePos`, `mouseDelta`, `mouseWheel`, `lastPressed`, `held`, `pressed`, `released`, `activeDevice`, `hasFocus`, `textLen`, `textInput[19]`).
  - `TextView()` returns `std::string_view` of up to 18 UTF-8 bytes typed this frame.

### 2. `input/ActionBuffer.h`
- **`ActionBuffer`** (`64 bytes`, `alignas(64)`):
  - **Part 1 — Action Press, Release, Chord & Double-Tap Window (`40B`):**
    - Records press ages (`m_pressAge[16]`), release ages (`m_releaseAge[16]`), pending masks (`m_pendingPressMask`, `m_pendingReleaseMask`), release-gated armed mask (`m_armedDoubleTapMask`), and double-tap mask (`m_doubleTapMask`).
    - **Multi-Key Double-Tap Invariant:** `m_armedDoubleTapMask` only arms an action for double-tap after that action has been **released** (`in.released`) within `doubleTapMaxGapTicks`, and `InputPipeline::RecordTapAndSocd` only latches a press edge when no other binding of that action is already physically held — preventing false double-taps or retrigger resets when holding Primary Key 1 and pressing Secondary Key 2.
    - `Peek(a, windowTicks)` / `Consume(a, windowTicks)` / `PeekRelease(a, windowTicks)` / `ConsumeRelease(a, windowTicks)`
    - `WasDoubleTapped(a)` / `ConsumeDoubleTap(a)`
    - `ConsumeChord(chordMask, currentlyHeldMask, slopWindowTicks)`
    - `ConsumeHighestPriority(priorityOrder, windowTicks)`
    - `TryConsumeJump(jumpAction, isGrounded, ticksSinceGrounded, jumpBufferTicks, coyoteTicks)`
  - **Part 2 — Numpad Command Motion & Charge Ring Buffer (`24B`):**
    - 11-entry circular buffer (`m_dirRing[11]`, `m_dirTicks[11]`, `m_dirHead`, `m_dirCount`) recording Numpad transitions (`1..9`) and dwell ticks per direction (saturating at `255`).
    - `CheckMotion(motionNumpad, facingRight, maxTotalTicks)` / `ConsumeMotion(motionNumpad, attackAction, facingRight, maxMotionTicks, attackWindowTicks)`: Matches command sequences (e.g., `{2, 3, 6}` Quarter-Circle Forward, `{6, 2, 3}` Dragon Punch, `{6, 5, 6}` Dash) within `maxTotalTicks`, automatically flipping horizontal sequence steps (`4 <-> 6`, `1 <-> 3`, `7 <-> 9`) when `facingRight == false`.
    - `CheckChargeMotion(chargeNumpadDir, releaseNumpadDir, minChargeTicks, facingRight, maxReleaseGapTicks)`: Matches charge specials (e.g. `[4]6` Back-Forward, `[2]8` Down-Up), where holding Down-Back (`1`) satisfies charging for both Back (`4`) and Down (`2`).

### 3. `input/InputPipeline.h`
- **`InputPipeline`** (`640 bytes`, `alignas(64)`):
  - **Event Ingestion:**
    - `IngestPlatformFrame(const FrameBatchT& batch)`: Single-call ingestion from `PlatformFrameEvents` with focus-loss physical state wipe, mid-frame gamepad disconnect cleanup, `> 2.0px` mouse movement hysteresis before switching `activeDevice` to `Mouse`, 18-byte UTF-8 text forwarding, and event dispatch.
    - `FeedButton(InputSource, down)` / `FeedGamepadButton(padSlot, button, down)`: Multi-binding aware rising-edge detection (pressing a second key mapped to an already-held action does not falsely retrigger or reset `heldTicks`).
    - `FeedGamepadAxis(padSlot, axisIdx, value)`:
      - **Schmitt-Trigger Analog-to-Digital Hysteresis:** Automatically updates virtual buttons `[20..31]` using dual thresholds (`kAxisPressThreshold = 18000` `~0.55f` to press, `kAxisReleaseThreshold = 12000` `~0.37f` to release), eliminating boundary chatter on analog triggers (`L2`/`R2`) and stick directions bound to digital actions.
  - **Deterministic Fixed-Tick & Render-Frame Sampling:**
    - `EndFrame()`: Produces `FrameInput` using dedicated `m_frameTap` / `m_prevFrameHeld` latches independent of simulation tick rate.
    - `PollTick(tickIndex)`: Produces `TickInput` using dedicated `m_tickTap` / `m_prevTickHeld` latches, preserves `out.heldTicks[idx]` on pure release ticks while resetting `m_heldTicks[idx] = 0`, and resolves all 4 axes with primary + alternate digital SOCD pairs (`IsAxisDirDown`), sub-frame digital axis tap latches (`m_axisState`), and `[deadzone, outerDeadzone] -> [0.0f, 1.0f]` analog remapping.
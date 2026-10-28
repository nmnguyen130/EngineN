# `engine/time/` — Contracts & Invariants

## Files
- `engine/time/TimeTypes.h` (Layer 1: `constexpr` types and channel modulation slot)
- `engine/time/TimePipeline.h` (Layer 1: `constexpr` 120 Hz clock, `SimStepView`, and `FramePacer` declaration)
- `engine/time/SdlClock.h`, `engine/time/SdlClock.cpp` (Layer 2: SDL3 timer wrapper and `FramePacer::EndFrameAndPace`)

## Contracts & Invariants

### 1. Fixed-Timestep & Harmonic Accumulator (`TimeTypes.h`, `TimePipeline.h`)
- **Constants:** `kSimHz = 120`, `kFixedDtNs = 8,333,333`, `kFixedDtSec = 1.0f / 120.0f`, `kHarmonicSubdivisions = 12`, `kHarmonicHz = 1440`, `kTickUnits = 12 * 10^9`.
- **Harmonic VSync Snapping & VRR Filter:**
  - Frame deltas within `+-150 us` (`kVsyncSnapMarginNs`) of a valid 1440 Hz divisor (`30, 40, 48, 60, 72, 80, 90, 120, 144, 160, 180, 240, 360, 480 Hz`) snap to exact integer tick subdivisions.
  - Unsnapped deltas within `+-20%` of `m_smoothedDeltaNs` are filtered with a 3:1 integer EWMA.
  - **Spiral-of-Death Guard:** Frame deltas clamp to `kMaxFrameDeltaNs` (`250 ms`), and `m_accUnits` clamps to `maxCatchupTicks * kTickUnits` (default `8` ticks), recording excess in `TimeTelemetry::droppedTicks`.

### 2. Channel Hierarchy & Time Ramps (`ChannelModSlot`, `TimePipeline`)
- **Channel Hierarchy:** 8 channels (`TimeChannel`). `World` (`0`) is the parent of gameplay channels `Player` (`1`), `Enemy` (`2`), `Projectile` (`3`), `Vfx` (`4`), and `Camera` (`5`). `Ui` (`6`) and `Custom` (`7`) are independent root channels scaled only by `m_masterScale`.
- **`ChannelModSlot` (`16B`, `alignas(16)`):**
  - Fields: `f32 baseScale`, `f32 rampTargetScale`, `u16 hitstopTicks`, `u16 rampHoldTicks`, `u16 rampBlendTicks`, `u16 rampElapsedTicks`.
  - **Ramp Progression Invariant:** During `AdvanceChannelModulationTick()`, `rampHoldTicks` counts down to `0` first while `rampElapsedTicks` stays `0`. Once `rampHoldTicks == 0`, `rampElapsedTicks` increments each tick up to `rampBlendTicks` using a C1 Hermite curve (`t^2 * (3 - 2t)`). This prevents `u16` overflow even when `holdTicks + blendTicks > 65535`, without increasing `sizeof(ChannelModSlot)` beyond 16 bytes.

### 3. Tick Snapshot Stability (`SimStepView`, `TimePipeline::PollTick`)
- **`SimStepView` (`32B`, `alignas(32)`):**
  - Fields: `const BakedTickState* baked`, `TimePipeline* pipeline`, `u64 tickIndex`, `u32 catchupIndex`, `u16 substepTotal`, `u8 frozenMask`, `bool valid`.
  - **Snapshot Stability Invariant:** `PollTick()` returns `SimStepView` pointing to `m_bakedTick` without advancing/rebaking channel modulation at the end of `PollTick()`. Instead, the pending modulation advance is flushed via `FlushPendingAdvance()` at the start of the next `PollTick()`, `BeginFrameManual()`, or channel mutator call, keeping `m_bakedTick` stable while the current tick's `SimStepView` is read.

### 4. Struct Sizes (`static_assert` Locked)
- `BakedTickState`: `64B` (`alignas(64)`)
- `ChannelModSlot`: `16B` (`alignas(16)`)
- `RenderTimeView`: `48B` (`alignas(16)`)
- `TimeTelemetry`: `24B` (`alignas(8)`)
- `SimStepView`: `32B` (`alignas(32)`)
- `TimePipeline`: `256B` (`alignas(64)`)
- `FramePacer`: `32B` (`alignas(8)`)

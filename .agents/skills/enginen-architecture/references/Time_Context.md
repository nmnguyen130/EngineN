# EngineN — Phase 2: Time Subsystem Context (`time/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocation, 4-file two-layer footprint (`TimeTypes.h`, `TimePipeline.h`, `SdlClock.h`, `SdlClock.cpp`) + `tests/TimeTests.cpp` (`namespace en`).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`TimeTypes.h`, `TimePipeline.h`):** 100% `constexpr` C++23 simulation clock, 1440 Hz LCM harmonic snapper, VRR micro-jitter EWMA filter, 8-channel modulator, `SimStepView`, `RenderTimeView`, and 64-bit SplitMix `ComputeAuditHash()`. Zero OS, `<chrono>`, `<thread>`, or `<SDL3/...>` includes. Verified at compile time via `static_assert`.
  - **Layer 2 (`SdlClock.h`, `SdlClock.cpp`):** Thin SDL3 hardware timer and sleep adapter (`SdlClock::NowNs()`, `SdlClock::SleepNs()`, `SdlClock::YieldThread()`) + `FramePacer::EndFrameAndPace()`. `<SDL3/SDL_timer.h>` (`SDL_GetTicksNS`, `SDL_DelayNS`) is strictly confined to `SdlClock.cpp`, sharing the exact monotonic nanosecond timeline of SDL3 input events (`SDL_Event::common.timestamp`).
- **1440 Hz LCM Harmonic Rational Accumulator & VRR Smoothing:**
  - `kSimHz = 120`, `kHarmonicSubdivisions = 12`, `kHarmonicHz = 1440` (`kTickUnits = 12 * 10^9` units per 120 Hz tick).
  - Because `LCM(30, 40, 48, 60, 72, 80, 90, 120, 144, 160, 180, 240, 360, 480) = 1440`, every standard refresh rate maps to an exact integer multiple `k = 1440 / Hz` of `kNsPerSecond` units with zero integer division truncation.
  - **O(1) Bitmask Harmonic Snapping:** Snaps frame deltas within `±150µs` (`kVsyncSnapMarginNs`) via 64-bit `kValidHarmonicMask` lookup with zero loops.
  - **VRR Micro-Jitter Filter (`FilterVrrDeltaUnits`):** Smooths unsnapped variable-refresh deltas within `±20%` via a 3:1 integer EWMA while passing larger frame-rate transitions immediately.
  - **Spiral-of-Death Protection:** Clamps frame delta (`kMaxFrameDeltaNs = 250ms`) and substeps (`maxCatchupTicks = 8`).
- **256-Byte L1 Cache Layout (`TimePipeline`, 4 Cache Lines Exact, Zero Padding):**
  - **Cache Line 0 (`offset 0..63`, `64B`):** `BakedTickState m_bakedTick` (`scaledDt[8]`, `effectiveScale[8]`).
  - **Cache Lines 1 & 2 (`offset 64..191`, `128B`):** `std::array<ChannelModSlot, 8> m_slots` (`16B` per channel: `baseScale`, `rampTargetScale`, `hitstopTicks`, `rampHoldTicks`, `rampBlendTicks`, `rampElapsedTicks`).
  - **Cache Line 3 (`offset 192..255`, `64B`):** `m_accUnits` (`8B`), `m_lastWallNs` (`8B`), `TimeTelemetry m_telemetry` (`24B`), `m_smoothedDeltaNs` (`8B`), `m_masterScale` (`4B`), `m_realDtSec` (`4B`), `m_stepAllowanceTicks` (`2B`), `m_plannedSubsteps` (`2B`), `m_activeHitstopMask` (`1B`), `m_activeRampMask` (`1B`), `m_frozenMask` (`1B`), `m_paused` (`1B`).
- **Hierarchical Channel Modulation & True Pause vs Gameplay Freeze:**
  - Channel `0` (`TimeChannel::World`) cascades to Gameplay Channels `1..5` (`Player`, `Enemy`, `Projectile`, `Vfx`, `Camera`). `Ui` (`6`) and `Custom` (`7`) are independent root channels.
  - Modifying channels `1..7` rebakes only the modified channel in O(1) (`BakeSingleChannel(idx)`); modifying `World` (`0`) rebakes `0..7`.
  - `RequestTimeRamp` blends out using a C1-continuous Hermite `SmoothStep` curve (`t^2 * (3 - 2t)`).
  - `SetPaused(true)` halts `m_accUnits` accumulation (preserving active hitstops and time ramps on gameplay channels while updating `realDt` for UI). `RequestStep(ticks)` injects `ticks * kTickUnits` for exact debugger frame-stepping while paused. `RequestGameplayFreeze(ticks)` hitstops `kGameplayChannelsMask` (`0x3FU`) while `Ui` and `Custom` continue ticking at 120 Hz.

## 2. Locked Struct Byte Layouts (Zero Internal or Tail Padding)
- `BakedTickState`: `64 B` (`alignas(64)`) — `scaledDt[8]`, `effectiveScale[8]`.
- `ChannelModSlot`: `16 B` (`alignas(16)`) — `baseScale`, `rampTargetScale`, `hitstopTicks`, `rampHoldTicks`, `rampBlendTicks`, `rampElapsedTicks`.
- `RenderTimeView`: `48 B` (`alignas(16)`) — `channelAlpha[8]` (locked to `1.0f` when channel is frozen), `tickIndex`, `rawAlpha`, `realDt`.
- `TimeTelemetry`: `24 B` (`alignas(8)`) — `totalSimTicks`, `totalFrames`, `droppedTicks`, `ticksLastFrame`, `maxCatchupTicks`.
- `SimStepView`: `32 B` (`alignas(32)`) — `baked`, `pipeline`, `tickIndex`, `catchupIndex`, `substepTotal`, `frozenMask`, `valid`.
- `TimePipeline`: `256 B` (`alignas(64)`) — 4 cache lines exact.
- `FramePacer`: `32 B` (`alignas(8)`) — `m_frameStartNs`, `m_targetFrameNs`, `m_lastSleepNs`, `m_lastSpinNs`, EWMA `m_sleepMarginNs` (`[250µs, 4ms]`), `m_targetFps`.

## 3. Public Interfaces & Signatures
- **`SimStepView`:** `operator bool()`, `Dt(ch)`, `EffectiveScale(ch)`, `IsFrozen(ch)`, `RequestHitstop(ch, ticks)`, `RequestMaskHitstop(mask, ticks)`, `RequestTimeRamp(ch, targetScale, holdTicks, blendTicks)`.
- **`TimePipeline`:** `Reset()`, `BeginFrame()`, `BeginFrameManual(rawDeltaNs)`, `PollTick()`, `GetRenderView()`, `SetMasterScale(scale)`, `MasterScale()`, `SetBaseScale(ch, scale)`, `BaseScale(ch)`, `RequestHitstop(ch, ticks)`, `RequestMaskHitstop(mask, ticks)`, `RequestGameplayFreeze(ticks)`, `RequestTimeRamp(ch, targetScale, holdTicks, blendTicks)`, `ClearChannelEffects(ch)`, `SetPaused(paused)`, `IsPaused()`, `RequestStep(ticks)`, `OnWindowRefocus()`, `FrozenMask()`, `EffectiveScales()`, `AccumulatorNs()`, `Telemetry()`, `ComputeAuditHash()`.
- **`FramePacer` & `SdlClock`:** `SdlClock::NowNs()`, `SdlClock::SleepNs(ns)`, `SdlClock::YieldThread()`, `FramePacer::SetTargetFps(fps)`, `BeginFrame()`, `EndFrameAndPace()`, `TargetFps()`, `TargetFrameNs()`, `LastSleepNs()`, `LastSpinNs()`, `SleepMarginNs()`.
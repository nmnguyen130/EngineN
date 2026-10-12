---
name: enginen-architecture
description: Architectural standards, boundaries, and runtime pipeline guidelines for EngineN (C++20/SDL3 2D Action Metroidvania Engine).
---

# EngineN — Architecture & Runtime Pipeline

Core design principles, golden boundaries, determinism rules, and pipeline lifecycle for EngineN.

---

## 1. Golden Boundaries

1. **Input != State:** Raw window events are ingested into frame history. Gameplay reads only tick-sliced inputs (`CutTick()`). Input never directly mutates character position or state.
2. **State != Physics:** FSM state behaviors submit `MotionRequest` objects to the `MotionArbiter`. They do NOT overwrite body velocities directly.
3. **Simulation != Presentation:** Physics and gameplay run at fixed 120Hz. Rendering runs at display refresh rate, consuming transform snapshots interpolated unconditionally via `alpha`.
4. **Physics Correctness != Player Forgiveness:** Pure swept AABB collision math is decoupled from the platformer "Forgiveness Layer" (Coyote Time, Corner Correction, Jump Buffering, Ledge Pop).

---

## 2. Timing & Determinism

- **Exact Integer Accumulator:** 
  - Units: `kNsPerSecond = 1'000'000'000ULL`, `kSimHz = 120`.
  - Accumulation: `m_accUnits += deltaNs * kSimHz`. Consume tick when `m_accUnits >= kNsPerSecond`.
  - Guarantees zero rational float drift over arbitrary execution spans.
- **Spiral-of-Death Protection:** Clamped to `kMaxCatchupTicks = 8` (~66.7ms). Backlog discarded while preserving remainder sub-tick phase.
- **Tick-Counted Hitstop:** Hitstop durations are measured in integer sim ticks (`u32 ticks`), never wall-clock milliseconds. Hitstop modulates channel `scaledDt` to `0.0f` without skipping ticks or stalling collision checks.
- **Time Channels:**
  - `RealTime (0)`: Unscaled (UI, hitstop decay, profiler).
  - `WorldTime (1)`: Environment, dynamic hazards, platforms.
  - `PlayerTime (2)`: Player locomotion, combat, states.
  - `EnemyTime (3)`: AI, enemies, bosses.

---

## 3. Runtime Lifecycle Pipeline

```
[Window Events] -> Ingest / PublishFrameInput
  -> Fixed Simulation Loop (120Hz)
       * BeginFrame / ConsumeTick
       * Input CutTick (per-tick slice)
       * Simulation & FSM Tick
       * Unconditional Transform Snapshot (Interpolated<T>)
       * HitstopEngine AdvanceTick
  -> Render Extraction & Interpolation (Display Hz)
       * Alpha lerp: prev + alpha * (curr - prev)
       * Extract QuadInstances to FrameArena
  -> Batching & Sorting
       * 64-bit Radix SortKey (Layer, Texture, BlendMode)
  -> GPU Submission & Presenter
       * Premultiplied Alpha (Additive via alpha-0)
       * Flip-model presentation pacing
```

---

## 4. Performance & Memory Rules

- **Zero-Allocation Hot Path:** No heap allocations (`new`, `malloc`, `std::vector::push_back` reallocation) in `Tick`, `Extract`, `Batch`, `Execute`, or `Present`.
- **Arena-Backed Render Frames:** All frame transients allocate from a linear reset-per-frame `FrameArena`.
- **Pure Swept AABB:** Custom kinematic Swept AABB platformer physics only. Never link Box2D or rigid-body physics engines.
- **Header Purity:** Keep header files lightweight. Defer standard math/algorithms (`<cmath>`, `<algorithm>`) and platform/SDL headers into `.cpp` files.

---

## 5. Architectural References

- [`docs/EngineN_Runtime_Architecture_Audit_Claude.md`](file:///d:/Coding/Game/EngineN/docs/EngineN_Runtime_Architecture_Audit_Claude.md): Runtime audit, defect registry, and subsystem redesign plan.
- [`architecture.md`](file:///d:/Coding/Game/EngineN/architecture.md): Game system specifications.
- [`TASKS.md`](file:///d:/Coding/Game/EngineN/TASKS.md): Roadmap and implementation milestones.

---
name: enginen-architecture
description: Architectural standards, conventions, and implementation guidelines for EngineN (C++20/SDL3 2D Action Metroidvania Engine).
---

# EngineN — Architecture & Development Skill

This skill contains the design principles, architectural boundaries, coding standards, and implementation workflows for the **EngineN** codebase.

## 1. Core Principles & Golden Boundaries

Always uphold the **4 Golden Boundaries**:
1. **Input ≠ State:** Input events are timestamped and queued in an `InputBuffer`. They never alter player transform or state directly.
2. **State ≠ Physics:** States in the FSM submit `MotionRequest` objects to the `MotionArbiter`. They do NOT overwrite velocities directly.
3. **Gameplay ≠ Presentation:** Physics and simulation are decoupled from rendering. The renderer only consumes interpolated visual transforms via `alpha`.
4. **Physics Correctness ≠ Player Forgiveness:** Pure swept AABB math is isolated from the "Forgiveness Layer" (Coyote Time, Corner Correction, Jump Buffering, Ledge Pop).

## 2. Timing & Determinism Rules

- **Fixed Timestep:** Simulation MUST run at 120Hz fixed timestep (`FIXED_DT_NS = 8'333'333 ns`, `1/120s`).
- **Internal Time Unit:** All internal time measurements, accumulators, and durations MUST use `uint64_t` nanoseconds to avoid floating-point drift over time.
- **Spiral-of-Death Protection:** Accumulator clamped at `MAX_CATCHUP_TICKS = 8` ticks (~66.7ms).
- **Time Channels:**
  - `RealTime`: Never scaled (UI, profiler, hitstop decay).
  - `WorldTime`: Environmental entities, platforms, hazards.
  - `PlayerTime`: Player locomotion and actions.
  - `EnemyTime`: Enemies and bosses.
- **Hitstop Rule:** Hitstop NEVER pauses the simulation loop or skips ticks. It modulates the channel's `scaledDt` to `0.0`, keeping collision checks, input buffers, and tick counts completely deterministic.

## 3. Code Standards & C++20 Conventions

- **Language:** C++20 standard (`-std=c++20`).
- **Memory Safety:** Prefer value semantics and stack allocation. Avoid unnecessary dynamic heap allocations (`new`/`delete`) in the per-tick hot path.
- **No Heavy Physics Engine:** Never link or use Box2D or rigid-body physics. EngineN uses custom kinematic swept AABB specifically designed for platformers.
- **Naming Conventions:**
  - Types / Classes / Structs: `PascalCase` (e.g., `TimeManager`, `KinematicBody2D`)
  - Methods: `PascalCase` (e.g., `BeginFrame()`, `ConsumeTick()`)
  - Member variables: `m_camelCase` (e.g., `m_accumulatorNs`, `m_alpha`)
  - Enums: `enum class EnumName : uint8_t { MemberOne, MemberTwo };`

## 4. Primary Architecture Documents

- [`architecture.md`](file:///d:/Coding/Game/EngineN/architecture.md): Complete game system specification.
- [`TASKS.md`](file:///d:/Coding/Game/EngineN/TASKS.md): Task roadmap and status tracking.

# EngineN — Gameplay Subsystem Context (`engine/gameplay/`)

## 1. Architecture & Invariants
- **Core:** C++23, zero heap allocation in hot paths (single 512 KB pre-allocated arena slab at `Init()`), zero virtual dispatch (`GameplayPolicyTable` function pointers), minimal footprint (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp`).
- **Two-Layer Policy Separation:**
  - **Layer 1 (Authoritative Solvers):** Two-level `u64` tile collision bitplane, 3-pass counting-sort dynamic spatial grid (`SpatialIndex2D`), continuous swept-AABB move-and-slide solver (`KinematicSolver2D`), Q8.8 fixed-point priority motion stack, cancel-window FSM, 64-bit SplitMix Bloom filter hit arbitration, and lockstep `AnimationSubsystem`.
  - **Layer 2 (`GameplayPipeline`):** Binds `assets::BlobRecord` (`Residency::Sim`) collision/rig maps, invokes user policy callbacks, steps analytic 2nd-order `CameraController2D`, manages 4096-bit persistent `m_worldFlags`, and outputs lockless `SimCue` presentation records (audio/VFX).
- **6-Stage 120Hz Pipeline Execution (`GameplayPipeline::Tick`):**
  - **Stage 1 (Carriers):** Advance moving platforms & transfer delta displacements onto grounded riders (`KinematicCarrier2D`).
  - **Stage 2 (Animations):** Step `AnimationSubsystem` & dispatch `.erig` frame directives (0-tick hitbox latency, TimeChannel freeze gating).
  - **Stage 3 (State & Policies):** Advance state machine timers and evaluate policy callbacks against `cancelMask`.
  - **Stage 4 (Kinematics):** Integrate ambient velocities, rebuild spatial grid, solve swept motors (`KinematicMotor2D`), propagate transforms.
  - **Stage 5 (Combat):** Rebuild combat grid at updated world transforms, gather overlaps via `hitBloomMask`, arbitrate damage/hits.
  - **Stage 6 (Camera):** Step critically-damped spring camera director on `TimeChannel::Camera` (unfrozen during combat hitstop).
- **Double-Integration Prevention:** Colliding entities use `KinematicMotor2D` (`kTypeId = 16`) and never attach `world::Velocity2D` (`kTypeId = 1`).

## 2. Core Components & Layouts (`GameplayTypes.h`)
- `GameplayErrc`: `InvalidConfig`, `OutOfMemory`, `CorruptedBlobHeader`, `CorruptedBlobPayload`, `ArenaExhausted`, `WorldRegistrationFailed`.
- `CompactAabb2D` (8B): Q8.8 signed meter bounds (`offsetX_q8`, `offsetY_q8`, `halfW_q8`, `halfH_q8`).
- `KinematicMotor2D` (`alignas(32)`, 32B, `kTypeId = 16`): `vx`, `vy`, `bounds`, `carrierId`, `slopeNormalX/Y_q14`, `solidMask`, `contacts`, `flags`.
- `KinematicCarrier2D` (`alignas(16)`, 16B, `kTypeId = 17`): `deltaX`, `deltaY`, `bounds`.
- `MotionStack2D` (`alignas(32)`, 32B, `kTypeId = 18`): `targetWalkVx`, `overrideVx/Vy`, `impulseVx/Vy`, `rootMotionDx/Dy_q8`, `gravityScale_q8`, `overrideTicks`, `coyoteTicks`, `packedSigns`.
- `StateMachine2D` (`alignas(16)`, 16B, `kTypeId = 19`): `rigBlob` (`BlobHandle`), `stateTicks`, `cancelMask`, `onHitCancelMask`, `stateId`, `animClipId`, `hurtboxProfileId`.
- `HitboxRig2D` (`alignas(32)`, 32B, `kTypeId = 20`): `hitBloomMask` (64-bit SplitMix filter), `hurtbox`, `primaryHitbox`, `hitLayerMask`, `hurtLayerMask`, `swingId`.
- `DamageReceiver2D` (`alignas(16)`, 16B, `kTypeId = 21`): `hp`, `maxHp`, `iframeTicks`, `stunTicks`, `poise`, `teamId`, `receiverFlags`.
- `SafeGroundBreadcrumb2D` (`alignas(16)`, 16B, `kTypeId = 22`): `safeX/Y`, `prevSafeX/Y` (verified non-hazard anchors).
- `CameraController2D` (`alignas(64)`, 64B, `kTypeId = 23`): `primaryTarget`, `secondaryTarget`, `velX/Y`, `activeRoomBounds`, `lookaheadX_q8`, `trauma_q16`, `springFreqX/Y_q4`.

## 3. Subsystem APIs
### `SpatialIndex2D` (`engine/gameplay/SpatialIndex2D.h`)
- `BindArena(span<u8>)`, `BindCollisionMapBlob(BlobRecord)` $\to$ `Result<void, GameplayErrc>`, `ClearCollisionMap()`
- `RebuildDynamicGrid(World)`, `SweepAabb(...)` $\to$ `SlabImpact2D`, `Raycast(...)` $\to$ `RayHit2D`
- `GatherCombatOverlaps(World, AssetBank, span<ResolvedHitEvent>)` $\to$ `u32`.

### `KinematicSolver2D` & `StateAndCombatSolver` (`engine/gameplay/`)
- `KinematicSolver2D::StepCarriersAndRiders(World&, SpatialIndex2D&, SimStepView)`
- `KinematicSolver2D::StepMotorsAndMotionStacks(World&, SpatialIndex2D&, SimStepView, KinematicStepConfig)`
- `AnimationSubsystem::StepAnimationsAndDirectives(World&, AssetBank&, SimStepView, GameplayPipeline&)`
- `StateAndCombatSolver::StepStateMachinesAndPolicies(World&, AssetBank&, SimStepView, TickInput&, ActionBuffer&, GameplayPolicyTable&, GameplayPipeline&)`
- `StateAndCombatSolver::ResolveCombatHits(World&, SimStepView, span<const ResolvedHitEvent>, GameplayPolicyTable&, GameplayPipeline&)`.

### `GameplayPipeline` (`engine/gameplay/GameplayPipeline.h` — 120Hz Coordinator)
- `Init(World&, GameplayPolicyTable, GameplayPipelineConfig)` $\to$ `Result<void, GameplayErrc>`, `Shutdown()`
- `SetActiveRoomCollision(AssetBank&, BlobHandle)` $\to$ `Result<void, GameplayErrc>`
- `Tick(World&, AssetBank&, SimStepView, TickInput&, ActionBuffer&)`: Runs Stages 1..6.
- `EmitCue(SimCue)`, `DrainCues()` $\to$ `span<const SimCue>`
- `SetWorldFlag(flagId, value)`, `TestWorldFlag(flagId)` $\to$ `bool`, `ClearWorldFlags()`
- `Spatial()` $\to$ `const SpatialIndex2D&`, `ComputeAuditHash(World)` $\to$ `u64`.

## 4. Main Loop Integration
```cpp
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);
}

for (const auto& cue : m_gameplay.DrainCues()) {
    // Audio / VFX / Haptics handling
}
```
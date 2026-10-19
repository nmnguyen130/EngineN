# EngineN — Gameplay & Kinematic Simulation Subsystem Context (`gameplay/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single pre-allocated 64B-aligned arena slab), zero virtual dispatch (`GameplayPolicyTable` function-pointer table), locked 5-module / 9-file footprint (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp`) + `tests/GameplayTests.cpp`, two-layer pure C++23 separation (zero OS or SDL3 dependencies; `namespace en::gameplay`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Pollution:**
  - **Layer 1 (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`):** Policy-free authoritative simulation mechanisms: two-level hierarchical `u64` tile bitplane, 1-cache-line `SpatialIndex2D` (`sizeof == 64`, `alignas(64)`) with 24B `GridBox` records and 0B stack-allocation 3-pass Counting-Sort Prefix-Sum dynamic spatial grid, continuous Minkowski Swept-AABB move-and-slide solver (`KinematicSolver2D`), unified single-pass `StateAndCombatSolver` (`using AnimationSubsystem = StateAndCombatSolver;`), priority motion stack, bitmask cancel-window & on-hit promotion FSM, and $O(1)$ 64-bit SplitMix Bloom-deduplicated combat overlap engine. Includes `"core/time/TimePipeline.h"` and defines `GameplayBitmaskEnum`, `SimCueKind`, `SimCue` (`32B`, `alignas(32)`), and `FastSinCos` directly in `GameplayTypes.h`.
  - **Layer 2 (`GameplayPipeline.h/.cpp`):** L1 cache-line-partitioned (`sizeof == 704`, `alignas(64)`) move-only RAII 120 Hz orchestrator, camera director, and persistent world-flag container. Binds `assets::BlobRecord` (`Residency::Sim`) collision/rig tables, invokes user-registered `GameplayPolicyTable` callbacks, steps the drift-free 2nd-order spring `CameraController2D` (propagating transforms immediately when any camera moves), manages the 4096-bit persistent world-flag bitset via a 64-bit sparse summary mask (`m_activeFlagWordsMask`), and outputs lockless `SimCue` records via `DrainCues()`.
- **L1 Cache Line Partitioning, Zero View-Template Bloat & Zero Redundant Loops:**
  - **`GameplayPipeline` (`sizeof == 704`, `alignas(64)`, Zero Dummy Padding Members):**
    - **Cache Line 0 (`offset 0..63`, `64B` exact):** Hot 120 Hz dispatch state (`m_policies`, `m_kinematicCfg`, `m_invPixelsPerMeter`, `m_hitScratch`, `m_cueBuffer`).
    - **Cache Line 1 (`offset 64..127`, `64B` exact):** `SpatialIndex2D m_spatial` (`64B` exact).
    - **Cache Line 2 (`offset 128..191`, `64B`):** `m_arena`, `m_arenaBytes`, `m_activeFlagWordsMask` (`u64` summary mask so `ClearWorldFlags()` and `ComputeAuditHash()` touch only non-zero words in $O(1)$), and `m_cueCount`.
    - **Cache Lines 3..10 (`offset 192..703`, `512B`):** `alignas(64) std::array<u64, 64> m_worldFlags{}`.
  - **`SpatialIndex2D` (`sizeof == 64`, `alignas(64)`, 1 Cache Line Exact):**
    - Replaces 8 disjoint SoA arrays with a single contiguous `GridBox` (`24B`, `alignas(8)`) array `m_boxes`, reducing cell-probe memory streams from 8 cache lines to 1 and deriving `TotalRefs()` in $O(1)$ from `m_cellOffsets[kGridCells]`.
    - Staging buffers (`stagingBoxes` and `stagingCells`) are carved in the second half of `m_boxes` inside the arena slab (`0B` stack allocation), computing `ComputeGridSpan` and `HashCellCoords` once in Pass 1 and scattering in Pass 3 via a 3-line 1D loop.
    - Stage 4 calls `m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::KinematicsOnly)` (indexing only `Carrier` and `Pushbox` bodies), and Stage 5 calls `m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::CombatOnlyIfActive)`, which skips the second grid rebuild completely on ticks where no `HitboxRig2D` has `HitboxActive` (preserving Stage 4's carrier grid for `Raycast` queries!).
    - Analytical slope evaluation uses a 6-entry branchless lookup table (`kSlopeTable`, `h(u) = baseH + slopeH * u`), and ray-AABB intersection uses a shared `ClipSlabAxis` helper.
  - **Zero `World::View<>` Instantiations & Single-Pass Direct Pool Iteration:**
    - All solvers (`SpatialIndex2D`, `KinematicSolver2D`, `StateAndCombatSolver`, `GameplayPipeline`) iterate dense `ComponentPool<T>` spans directly and resolve sibling components in 1 hop via `world.GetComponentMaskByIndex(idx)` + `pool.DirectAtEntityIndex(idx)` (`0` `world.View<>()` template wrappers or repeated `world.TryGet<T>()` lookups).
    - `KinematicSolver2D::StepCarriersAndRiders` exits in $O(1)$ when `world.GetPool<KinematicCarrier2D>().Size() == 0`, preserves `ContactFlags::Ground | OnSlope | OnOneWay` and `carrierId` across carrier sweeps so ascending platforms never strip grounded state prior to Stage 3 FSM evaluation, and sets `MotorFlags::Crushed` when squeezed against opposing solid geometry.
    - `GameplayPipeline::StepCameraControllers` uses branchless `std::clamp` for horizontal/vertical deadzones, unified `SoftCushionClamp`, and a unified 2nd-order critically damped spring (`kickOffsetX`, `kickOffsetY`) for both directional combat kicks and deterministic hash-noise trauma targets (`0` permanent drift on mid-tick `AddTrauma()`).

## 2. Core Types & Components (`gameplay/GameplayTypes.h`)
- **Primitives, Bitmasks, Cues & Fast Math:**
  - `concept GameplayBitmaskEnum<E>`: Unifies `|`, `&`, `~`, `|=`, `&=`, and `HasFlag(flags, test)` across `ContactFlags`, `MotorFlags`, `CancelCategory`, `TileMaterialFlags`, `HitboxRigFlags`, and `CameraCtrlFlags`.
  - `enum class GameplayErrc : u8`: `InvalidConfig`, `OutOfMemory`, `CorruptedBlobHeader`, `CorruptedBlobPayload`, `ArenaExhausted`, `WorldRegistrationFailed`.
  - `enum class SimCueKind : u8`: `AudioOneShot = 0`, `SpawnPrefabEffect`, `CameraTraumaPulse`, `GamepadRumblePulse`, `CustomGameEvent`.
  - `struct alignas(32) SimCue` (`sizeof == 32`): `assetId`, `worldX`, `worldY`, `emitter` (`world::EntityId`), `magnitude_q16`, `durationTicks`, `kind`, `channel`, `userTag`.
  - `constexpr void FastSinCos(f32 rad, f32& outCos, f32& outSin) noexcept`.
  - `struct CompactAabb2D` (`sizeof == 8`): Quantized Q8.8 signed meter local bounds (`offsetX_q8`, `offsetY_q8`, `halfW_q8`, `halfH_q8`).
- **Gameplay ECS Components (Zero-Padding Layouts):**
  - `struct alignas(32) KinematicMotor2D` (`kTypeId = 16`, `sizeof == 32`): `vx`, `vy`, `bounds` (`CompactAabb2D`), `carrierId`, `slopeNormalX_q14`, `slopeNormalY_q14`, `solidMask`, `bodyLayer`, `pushboxGroup`, `contacts`, `flags`, `timeChannel`, `oneWayIgnoreTicks`.
  - `struct alignas(16) KinematicCarrier2D` (`kTypeId = 17`, `sizeof == 16`): `deltaX`, `deltaY`, `bounds` (`CompactAabb2D`).
  - `struct alignas(32) MotionStack2D` (`kTypeId = 18`, `sizeof == 32`): `targetWalkVx`, `overrideVx`, `overrideVy`, `impulseVx`, `impulseVy`, `rootMotionDx_q8`, `rootMotionDy_q8`, `gravityScale_q8`, `overrideTicks`, `activePriority`, `groundCoyoteTicks`, `wallCoyoteTicks`, `packedSigns` (branchless bit-packed facing, wall sign, carrier launch, facing lock).
  - `struct alignas(16) StateMachine2D` (`kTypeId = 19`, `sizeof == 16`): `rigBlob` (`assets::BlobHandle`), `stateTicks`, `cancelMask`, `onHitCancelMask`, `lastAnimFrame`, `stateId`, `prevStateId`, `animClipId`, `hurtboxProfileId`.
  - `struct alignas(32) HitboxRig2D` (`kTypeId = 20`, `sizeof == 32`): `hitBloomMask` (`u64` 2-bit SplitMix filter), `hurtbox`, `primaryHitbox`, `hitLayerMask`, `hurtLayerMask`, `swingId`, `activeSliceIndex`, `rigFlags`.
  - `struct alignas(16) DamageReceiver2D` (`kTypeId = 21`, `sizeof == 16`): `hp`, `maxHp`, `iframeTicks`, `stunTicks`, `poise`, `teamId`, `receiverFlags`.
  - `struct alignas(16) SafeGroundBreadcrumb2D` (`kTypeId = 22`, `sizeof == 16`): `safeX`, `safeY`, `prevSafeX`, `prevSafeY` (3-ray verified static non-hazard anchors).
  - `struct alignas(64) CameraController2D` (`kTypeId = 23`, `sizeof == 64`): `primaryTarget`, `secondaryTarget`, `velX`, `velY`, `latchedGroundY`, `kickOffsetX`, `kickOffsetY`, `activeRoomBounds`, `prevRoomBounds`, `lookaheadX_q8`, `peekOffsetY_q8`, `kickVelX_q8`, `kickVelY_q8`, `trauma_q16`, `roomBlendTicks`, `roomBlendDuration`, `sustainedMoveTicks`, `secondaryWeight_q8`, `deadzoneHalfW_q4`, `deadzoneHalfH_q4`, `springFreqX_q4`, `springFreqY_q4`, `cameraFlags`.

## 3. Public Interfaces & Signatures
### `SpatialIndex2D` (`gameplay/SpatialIndex2D.h` — 64B 1-Cache-Line Accelerator)
- `void BindArena(std::span<u8> arenaSlice) noexcept`
- `[[nodiscard]] Result<void, GameplayErrc> BindCollisionMapBlob(const assets::BlobRecord& blob) noexcept`
- `void RebuildDynamicGrid(const world::World& world, SpatialRebuildMode mode = SpatialRebuildMode::All) noexcept`
- `[[nodiscard]] SlabImpact2D SweepAabb(world::EntityId ignoreEntity, f32 centerX, f32 centerY, f32 halfW, f32 halfH, f32 deltaX, f32 deltaY, u16 solidMask, bool ignoreOneWay) const noexcept`
- `[[nodiscard]] RayHit2D Raycast(f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist, u16 layerMask, world::EntityId ignoreId = world::kNullEntity) const noexcept`
- `[[nodiscard]] u32 GatherCombatOverlaps(const world::World& world, const assets::AssetBank& assetBank, std::span<ResolvedHitEvent> outHits) const noexcept`
- `[[nodiscard]] constexpr u32 TotalRefs() const noexcept`

### `KinematicSolver2D` & `StateAndCombatSolver` (`gameplay/KinematicSolver2D.h` & `gameplay/StateAndCombatSolver.h`)
- `static void KinematicSolver2D::StepCarriersAndRiders(world::World& world, const SpatialIndex2D& spatial, time::SimStepView step, const KinematicStepConfig& cfg = {}) noexcept`
- `static void KinematicSolver2D::StepMotorsAndMotionStacks(world::World& world, const SpatialIndex2D& spatial, time::SimStepView step, const KinematicStepConfig& cfg = {}) noexcept`
- `static void StateAndCombatSolver::DispatchAnimationFrameDirectives(world::World& world, const assets::AssetBank& assetBank, time::SimStepView step, GameplayPipeline& pipeline) noexcept` (aliased by `StepAnimationsAndDirectives` and `using AnimationSubsystem = StateAndCombatSolver;`)
- `static void StateAndCombatSolver::StepStateMachinesAndPolicies(world::World& world, const assets::AssetBank& assetBank, time::SimStepView step, const input::TickInput& tickInput, input::ActionBuffer& actionBuffer, const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept`
- `static void StateAndCombatSolver::ResolveCombatHits(world::World& world, time::SimStepView step, std::span<const ResolvedHitEvent> hits, const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept`

### `GameplayPipeline` (`gameplay/GameplayPipeline.h` — 704B L1-Aligned Move-Only RAII 120 Hz Coordinator)
- `[[nodiscard]] Result<void, GameplayErrc> Init(world::World& world, const GameplayPolicyTable& policies = {}, const GameplayPipelineConfig& cfg = {}) noexcept`
- `void Shutdown() noexcept`, `constexpr void SetPolicyTable(const GameplayPolicyTable& policies) noexcept`
- `[[nodiscard]] Result<void, GameplayErrc> SetActiveRoomCollision(const assets::AssetBank& bank, assets::BlobHandle collisionMapBlob) noexcept`
- `void Tick(world::World& world, const assets::AssetBank& assetBank, time::SimStepView step, const input::TickInput& tickInput, input::ActionBuffer& actionBuffer) noexcept`
- `constexpr void EmitCue(const SimCue& cue) noexcept`, `[[nodiscard]] constexpr std::span<const SimCue> DrainCues() noexcept`
- `constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept`, `[[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept`, `constexpr void ClearWorldFlags() noexcept`
- `[[nodiscard]] constexpr const SpatialIndex2D& Spatial() const noexcept`, `[[nodiscard]] u64 ComputeAuditHash(const world::World& world) const noexcept`
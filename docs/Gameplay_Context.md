# EngineN — Gameplay & Kinematic Simulation Subsystem Context (`gameplay/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocation in hot paths (single 512 KB pre-allocated contiguous arena slab at `Init()`), zero virtual dispatch (`GameplayPolicyTable` function-pointer table), minimal 5-file footprint (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp`), two-layer pure C++ separation (zero OS or SDL3 dependencies; `namespace en::gameplay`).
- **Two-Layer Zero-Virtual Seam & Policy Separation:**
  - **Layer 1 (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`):** Policy-free authoritative simulation mechanisms: two-level hierarchical `u64` tile bitplane, 3-pass Counting-Sort Prefix-Sum dynamic spatial grid, continuous Minkowski Swept-AABB move-and-slide solver, priority motion stack, bitmask cancel-window & on-hit promotion FSM, and 64-bit SplitMix Bloom-filter combat overlap engine.
  - **Layer 2 (`GameplayPipeline.h/.cpp`):** Stateless asset, policy, camera, and presentation bridge. Binds `assets::BlobRecord` (`Residency::Sim`) collision/rig tables, invokes user-registered `GameplayPolicyTable` callbacks, steps the analytic 2nd-order `CameraController2D`, manages the 4096-bit persistent world-flag bitset (`m_worldFlags`), and outputs lockless `SimCue` records for audio/VFX/haptics.
- **Zero-Tick Hitbox Latency & Channel Freeze Synchronization:**
  - Animation stepping (`World::StepKinematicsAndAnimations()`) and `CookedAnimRigHeader` frame-edge directive dispatching execute in **Stage 2** (prior to Stage 4 kinematics and Stage 5 combat overlaps), guaranteeing 0-tick latency between a frame entering its active window and its hitbox striking a target.
  - Entities on frozen `TimeChannel`s (`step.IsFrozen(ch)`) have `SpriteAnimator2D::playing` gated off before `StepKinematicsAndAnimations()` and restored immediately afterward (`HitboxRigFlags::LatchedAnimPlay`), preventing hitbox/frame desynchronization during hitstop.
- **Double-Integration Prevention & Transform Propagation:**
  - Colliding actors use `KinematicMotor2D` (`kTypeId = 16`) and never attach `world::Velocity2D` (`kTypeId = 1`).
  - `world.PropagateTransforms()` is invoked explicitly at the end of Stage 4 so hierarchical child hitboxes hold current-tick `WorldTransform2D` coordinates before Stage 5 combat queries run.

## 2. Core Types & Components (`gameplay/GameplayTypes.h`)
- **Primitives, Bitmasks & Compact Bounds:**
  - `enum class GameplayErrc : u8`: `InvalidConfig`, `OutOfMemory`, `CorruptedBlobHeader`, `CorruptedBlobPayload`, `ArenaExhausted`, `WorldRegistrationFailed`.
  - `enum class ContactFlags : u8`: `None`, `Ground`, `Ceiling`, `WallLeft`, `WallRight`, `OnSlope`, `OnOneWay`, `LedgeGrabLeft`, `LedgeGrabRight`.
  - `enum class MotorFlags : u8`: `None`, `SnapToGround`, `CornerCorrection`, `ProjectSlopes`, `DetectLedges`, `SoftPushbox`, `TrackSafeAnchor`, `Crushed`, `Submerged`.
  - `enum class CancelCategory : u16`: `None`, `MovementTurn`, `Jump`, `WallJump`, `EvadeDash`, `PrimaryAttack`, `SecondarySkill`, `DefensiveParry`, `UtilityInteract`, `CrouchSlide`, `LedgeMantle`, `CustomCategory0`, `CustomCategory1`, `AnyAction`.
  - `struct CompactAabb2D` (`sizeof == 8`): Quantized Q8.8 signed meter local bounds (`offsetX_q8`, `offsetY_q8`, `halfW_q8`, `halfH_q8`).
- **Gameplay ECS Components (Zero-Padding Layouts, `128B`/Actor Total):**
  - `struct alignas(32) KinematicMotor2D` (`kTypeId = 16`, `sizeof == 32`): `vx`, `vy`, `bounds` (`CompactAabb2D`), `carrierId`, `slopeNormalX_q14`, `slopeNormalY_q14`, `solidMask`, `bodyLayer`, `pushboxGroup`, `contacts`, `flags`, `timeChannel`, `oneWayIgnoreTicks`.
  - `struct alignas(16) KinematicCarrier2D` (`kTypeId = 17`, `sizeof == 16`): `deltaX`, `deltaY`, `bounds` (`CompactAabb2D`).
  - `struct alignas(32) MotionStack2D` (`kTypeId = 18`, `sizeof == 32`): `targetWalkVx`, `overrideVx`, `overrideVy`, `impulseVx`, `impulseVy`, `rootMotionDx_q8`, `rootMotionDy_q8`, `gravityScale_q8`, `overrideTicks`, `activePriority`, `groundCoyoteTicks`, `wallCoyoteTicks`, `packedSigns` (facing, wall sign, carrier launch momentum flag).
  - `struct alignas(16) StateMachine2D` (`kTypeId = 19`, `sizeof == 16`): `rigBlob` (`assets::BlobHandle`), `stateTicks`, `cancelMask`, `onHitCancelMask`, `lastAnimFrame`, `stateId`, `prevStateId`, `animClipId`, `hurtboxProfileId`.
  - `struct alignas(32) HitboxRig2D` (`kTypeId = 20`, `sizeof == 32`): `hitBloomMask` (`u64` 2-bit SplitMix filter), `hurtbox`, `primaryHitbox`, `hitLayerMask`, `hurtLayerMask`, `swingId`, `activeSliceIndex`, `rigFlags`.
  - `struct alignas(16) DamageReceiver2D` (`kTypeId = 21`, `sizeof == 16`): `hp`, `maxHp`, `iframeTicks`, `stunTicks`, `poise`, `teamId`, `receiverFlags`.
  - `struct alignas(16) SafeGroundBreadcrumb2D` (`kTypeId = 22`, `sizeof == 16`): `safeX`, `safeY`, `prevSafeX`, `prevSafeY` (3-ray verified static non-hazard anchors).
  - `struct alignas(64) CameraController2D` (`kTypeId = 23`, `sizeof == 64`): `primaryTarget`, `secondaryTarget`, `velX`, `velY`, `latchedGroundY`, `kickOffsetX`, `kickOffsetY`, `activeRoomBounds`, `prevRoomBounds`, `lookaheadX_q8`, `peekOffsetY_q8`, `kickVelX_q8`, `kickVelY_q8`, `trauma_q16`, `roomBlendTicks`, `roomBlendDuration`, `secondaryWeight_q8`, `deadzoneHalfW_q4`, `deadzoneHalfH_q4`, `springFreqX_q4`, `springFreqY_q4`, `cameraFlags`.
- **Cooked Binary Formats (`Residency::Sim` Blobs):**
  - `struct alignas(16) CookedCollisionMapHeader` (`32B`, magic `'ECOL'`), `CookedCollisionChunk8x8` (`24B`), `CookedTileCell` (`2B`), `EnvironmentalZone2D` (`24B`).
  - `struct alignas(16) CookedAnimRigHeader` (`32B`, magic `'ERIG'`), `CookedFrameDirective` (`8B`), `CookedHitboxSlice` (`16B`), `CookedHitboxSocket` (`16B`).

## 3. Public Interfaces & Signatures
### `SpatialIndex2D` (`gameplay/SpatialIndex2D.h`)
- `void BindArena(std::span<u8> arenaSlice) noexcept`
- `[[nodiscard]] std::expected<void, GameplayErrc> BindCollisionMapBlob(const assets::BlobRecord& blob) noexcept`
- `void RebuildDynamicGrid(const world::World& world) noexcept`
- `[[nodiscard]] SlabImpact2D SweepAabb(world::EntityId ignoreEntity, f32 centerX, f32 centerY, f32 halfW, f32 halfH, f32 deltaX, f32 deltaY, u16 solidMask, bool ignoreOneWay) const noexcept`
- `[[nodiscard]] RayHit2D Raycast(f32 originX, f32 originY, f32 dirX, f32 dirY, f32 maxDist, u16 layerMask, world::EntityId ignoreId = world::kNullEntity) const noexcept`
- `[[nodiscard]] u32 GatherCombatOverlaps(const world::World& world, const assets::AssetBank& assetBank, std::span<ResolvedHitEvent> outHits) const noexcept`

### `KinematicSolver2D` & `StateAndCombatSolver` (`gameplay/KinematicSolver2D.h` & `StateAndCombatSolver.h`)
- `static void KinematicSolver2D::StepCarriersAndRiders(world::World& world, const SpatialIndex2D& spatial, time::SimStepView step) noexcept`
- `static void KinematicSolver2D::StepMotorsAndMotionStacks(world::World& world, const SpatialIndex2D& spatial, time::SimStepView step, const KinematicStepConfig& cfg = {}) noexcept`
- `static void StateAndCombatSolver::DispatchAnimationFrameDirectives(world::World& world, const assets::AssetBank& assetBank, time::SimStepView step, GameplayPipeline& pipeline) noexcept`
- `static void StateAndCombatSolver::ResolveCombatHits(world::World& world, time::SimStepView step, std::span<const ResolvedHitEvent> hits, const GameplayPolicyTable& policies, GameplayPipeline& pipeline) noexcept`

### `GameplayPipeline` (`gameplay/GameplayPipeline.h` — Move-Only RAII 120 Hz Coordinator)
- `[[nodiscard]] std::expected<void, GameplayErrc> Init(world::World& world, const GameplayPolicyTable& policies = {}, const GameplayPipelineConfig& cfg = {}) noexcept`
- `void Shutdown() noexcept`, `void SetPolicyTable(const GameplayPolicyTable& policies) noexcept`
- `[[nodiscard]] std::expected<void, GameplayErrc> SetActiveRoomCollision(const assets::AssetBank& bank, assets::BlobHandle collisionMapBlob) noexcept`
- `void Tick(world::World& world, const assets::AssetBank& assetBank, time::SimStepView step, const input::TickInput& tickInput, input::ActionBuffer& actionBuffer) noexcept`
- `void EmitCue(const SimCue& cue) noexcept`, `[[nodiscard]] constexpr std::span<const SimCue> DrainCues() noexcept`
- `constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept`, `[[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept`
- `[[nodiscard]] u64 ComputeAuditHash(const world::World& world) const noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_assets.Pump(m_renderer);
m_input.BeginFrame();
SDL_Event ev;
while (SDL_PollEvent(&ev)) {
    en::input::IngestSdlEvent(m_input, ev);
}
const auto& frameInput = m_input.EndFrame();

m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);
}
for (const auto& cue : m_gameplay.DrainCues()) {
    m_presentation.ConsumeCue(cue, m_world, m_assets.Bank());
}
en::renderer::RenderList& list = m_renderer.BeginFrame();
en::world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), list);
m_renderer.Submit(list);
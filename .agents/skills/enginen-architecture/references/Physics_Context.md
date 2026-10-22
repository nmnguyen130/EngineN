# Phase 8: `physics` Subsystem (v2 Final Production Architecture — Locked)

## 1. Architectural Overview & Strict 2-Layer Decoupling
The `physics` subsystem implements a pure, general-purpose 2D spatial accelerator, continuous collision detector (Speculative Contacts + Swept AABB / CircleCast), Box2D v3 Soft-Step (TGS harmonic oscillator) rotational & linear sequential impulse solver, distance/spring/rope joint solver, and kinematic `MoveAndSlide` character controller with zero game-specific bloat (no fighting-game pushbox groups, no hardcoded tile slope angles, no input countdown timers).

- **Layer 1 (`physics/PhysicsTypes.h`, `physics/Narrowphase2D.h/.cpp`, `physics/SpatialIndex2D.h/.cpp`, `physics/DynamicsSolver2D.h/.cpp`)**: Pure C++23 algorithmic layer depending only on `core`, `time/TimeTypes.h`, and `world` (`World.h`). Zero dependency on `renderer`, `assets`, `platform`, or `input`.
- **Layer 2 (`physics/PhysicsSystem.h/.cpp`)**: `192B` (3 cache lines exact) RAII orchestrator over a single `AlignedArena` slab that binds `.ecol` cooked collision maps from `AssetBank`, registers physics ECS components (`Collider2D`, `RigidBody2D`, `CharacterMotor2D`, `DistanceJoint2D`) in `World`, steps the 5-stage simulation pipeline, drains `ContactEvent2D` transitions, and computes deterministic `ComputeAuditHash()`.

## 2. Exact Byte-Tight Memory Layouts & Invariants (`0B` Padding)
All structs are laid out with zero hidden compiler padding and verified by compile-time `static_assert`:
- `CompactAabb2D` (`8B`, `alignas(4)`): Quantized Q8.8 local geometry descriptor (`offsetX_q8`, `offsetY_q8`, `halfW_q8`, `halfH_q8` for box/circle/capsule/OBB; endpoints `(x0, y0) -> (x1, y1)` via `FromSegment` for `ShapeKind2D::Segment`).
- `CollisionFilter2D` (`8B`, `alignas(4)`): `u16 categoryBits`, `u16 maskBits`, `i16 groupIndex` (Box2D signed group precedence), `u16 materialTag`.
- ECS Physics Components (Type IDs `8..11`):
  - `Collider2D` (`24B`, `alignas(8)`, `kTypeId = 8`): `CompactAabb2D localBounds (8B)`, `CollisionFilter2D filter (8B)`, `u16 friction_q16`, `u16 restitution_q16`, `u16 density_q8`, `ShapeKind2D shape` (`Aabb`, `Circle`, `CapsuleY`, `Obb`, `Segment`), `ColliderFlags2D flags` (`IsSensor`, `IsOneWay`).
  - `RigidBody2D` (`32B`, `alignas(32)`, `kTypeId = 9` — 2 per cache line): `f32 vx`, `f32 vy`, `f32 angularVel`, `f32 invMass`, `f32 invInertia`, `f32 gravityScale`, `u8 linearDamping_q4`, `u8 angularDamping_q4`, `u8 sleepTimerTicks`, `BodyType2D bodyType` (`Static`, `Kinematic`, `Dynamic`), `BodyFlags2D flags`, `ContactFlags contacts`, `u8 islandId`, `u8 userTag`. Includes `ComputeMassFromCollider`, `ApplyLinearImpulse`, `ApplyAngularImpulse`, `ApplyImpulseAtOffset`.
  - `CharacterMotor2D` (`32B`, `alignas(32)`, `kTypeId = 10` — 2 per cache line): `f32 vx`, `f32 vy`, `f32 groundNormalX`, `f32 groundNormalY`, `CompactAabb2D bounds`, `EntityId carrierId`, `u16 solidMask`, `ContactFlags contacts`, `BodyFlags2D flags`.
  - `DistanceJoint2D` (`32B`, `alignas(32)`, `kTypeId = 11` — 2 per cache line): `EntityId entityA`, `EntityId entityB`, `f32 minLength`, `f32 maxLength`, `f32 impulse`, `i16 localAnchorAX_q8`, `i16 localAnchorAY_q8`, `i16 localAnchorBX_q8`, `i16 localAnchorBY_q8`, `u16 hertz_q6`, `u8 dampingRatio_q6`, `u8 enabled`.
- Query & Solver Records:
  - `RayHit2D` (`32B`, `alignas(32)`), `SweepHit2D` (`32B`, `alignas(32)`), `ContactEvent2D` (`32B`, `alignas(32)`).
  - `ContactManifold2D` (`64B`, `alignas(64)`, 1 cache line exact): 2-point contact manifold with `separation[2]` (`< 0` penetration, `> 0` speculative contact gap) and warm-started `normalImpulse[2]` / `tangentImpulse[2]`.
  - `SpatialBox2D` (`24B`, `alignas(8)`), `SpatialIndex2D` (`64B`, `alignas(64)`, 1 cache line exact), `PhysicsSystem` (`192B`, `alignas(64)`, 3 cache lines exact).

## 3. Core 2026 Industry Mechanisms
1. **O(N) Cell-Bucket Broadphase + 2-Level Tile Bitplane (`SpatialIndex2D`, `64B`)**: Traverses 1024-cell counting-sort buckets for broadphase pair generation (`GatherManifolds`), `Raycast`, `SweepAabb`, `CastCircle`, and `QueryAabb` instead of $O(N^2)$ loops, expanding bounds for rotated OBBs and `BulletCCD` velocity vectors.
2. **Full 2D Rotational + Linear Soft-Step (TGS) Contact & Joint Solver**: Computes contact lever arms $r_A = p - c_A$, $r_B = p - c_B$, rotational effective normal/tangent masses $K^{-1} = m_A^{-1} + m_B^{-1} + (r_A \times u)^2 I_A^{-1} + (r_B \times u)^2 I_B^{-1}$, and applies angular impulses $\omega \pm I^{-1} (r \times P)$ alongside `DistanceJoint2D` harmonic springs/rods.
3. **2-Point Sutherland-Hodgman OBB Face Clipping & Arbitrary `Segment` Terrain Edges**: `CollideObbObb` clips the incident face against the reference face side planes to produce 2 contact points for stable rotated stacking; `ShapeKind2D::Segment` supports arbitrary slope angles via exact normal/edge geometry.
4. **Unified Kinematic Rigid Bodies & 3-Iteration `MoveAndSlide` Character Controller**: `BodyType2D::Kinematic` `RigidBody2D` entities act as moving platforms carrying `CharacterMotor2D` riders; `MoveAndSlide` projects horizontal velocity along arbitrary surface normals (`normalY >= minGroundNormalY`), snaps downward onto slopes (`groundSnapDist`), and steps up stairs (`maxStepHeight`).

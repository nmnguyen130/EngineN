# EngineN — World Subsystem Context (`engine/world/`)

## 1. Architecture & Invariants
- **Core:** C++23, zero heap allocation in hot paths (single contiguous arena slab at `Init()`), zero virtual dispatch (`PoolOps` function pointers), minimal 8-file footprint (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp`).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`World` Core):** Authoritative simulation container, FIFO generational entity allocator, split SoA spatial spine (`LocalTransform2D`, `WorldTransform2D`, `PrevWorldSnapshot2D`), intrusive hierarchy forest + flat topological order (`TransformSolver`), arena-backed sparse-set component pools (`ComponentPool<T>`), and deferred command tape. 100% testable headless via `ComputeAuditHash()`.
  - **Layer 2 (`WorldExtractor`):** Stateless presentation & asset bridge. Extracts interpolated world state + 1-hop `assets::AssetBank::GetSprite()` into `renderer::RenderList::PushQuad` / `SetView`, and instantiates binary prefabs (`CookedPrefabHeader`) from `assets::BlobRecord`.
- **1:1 Direct-Indexed Spatial Spine & 64-Bit Bitsets:**
  - `m_local[]`, `m_world[]`, and `m_prev[]` are indexed directly by `EntityId::index` (zero indirection).
  - Empty, dead, or clean slots are skipped in blocks of 64 via `std::countr_zero` across 64-bit word bitsets (`m_aliveWords`, `m_spatialAliveWords`, `m_rootDirtyWords`, `m_childDirtyWords`).
- **Zero-Padding Spatial Layouts & Trig-Free Fast Path:**
  - `LocalTransform2D` (24B), `WorldTransform2D` (32B, caches `cosRot`, `sinRot`, `worldRotRad`, `worldDepth`), and `PrevWorldSnapshot2D` (24B) contain zero padding.
  - Stationary/frozen entities copy cached `(cosRot, sinRot)` with zero math. Rotating entities interpolate shortest-arc radians (`ShortestAngleDelta`) and evaluate a branchless 5th-order minimax polynomial (`FastSinCos`, ~6 cycles, zero `libm` calls).
- **Intrusive Forest & Flat Topological Solver:**
  - Combines intrusive tree links (`HierarchyNode`: `parent, firstChild, nextSibling, prevSibling`, 8B) with a flat topological execution array (`HierarchyStep { childIdx, parentIdx }`, 4B) where parents strictly precede descendants.
- **Permanent Slot 0 Sentinel & Teleport Synchronization:**
  - Slot `0` (`kNullEntity = {0, 0}`) is permanently reserved as the invalid/root identity transform sentinel.
  - Newly spawned or teleported entities set `EntityFlags::Teleported`; `World::EndTick()` synchronizes `m_prev[idx] = m_world[idx]` to prevent interpolation streaking.

## 2. Core Layouts & Types (`WorldTypes.h`)
- `EntityId`: `{u16 index, u16 generation}` (`kNullEntity = {0, 0}`).
- `WorldErrc`: `InvalidConfig`, `OutOfMemory`, `EntityPoolExhausted`, `ComponentPoolExhausted`, `CommandBufferOverflow`, `InvalidEntity`, `ComponentNotRegistered`, `HierarchyCycleDetected`, `HierarchyDepthExceeded`, `CorruptedPrefabBlob`.
- `ComponentMask`: `u64` bitmask (up to 64 component types via `consteval MakeComponentMask<Ts...>()`).
- `concept Component<T>`: Trivially copyable, destructible, standard layout, `alignof(T) <= 64`, `T::kTypeId < 64`.
- **Built-In Components:**
  - `LocalTransform2D` (`kTypeId = 0`, 24B): `{x, y, scaleX, scaleY, rotationRad, depthOffset}`.
  - `WorldTransform2D` (`alignas(32)`, 32B): `{x, y, scaleX, scaleY, cosRot, sinRot, worldRotRad, worldDepth}`.
  - `PrevWorldSnapshot2D` (24B): `{x, y, scaleX, scaleY, worldRotRad, worldDepth}`.
  - `Velocity2D` (`kTypeId = 1`, `alignas(16)`, 16B): `{vx, vy, angularVelRad, linearDamping}`.
  - `Sprite2D` (`kTypeId = 2`, `alignas(32)`, 32B): `{tint, sprite, baseDepth, ySortScale, localOffsetX, localOffsetY, passId, layer, viewSlot, blendAndFlags}`.
  - `SpriteAnimator2D` (`kTypeId = 3`, `alignas(16)`, 16B): `{atlas, firstFrame, frameCount, currentFrame, ticksPerFrame, subTickAccum, loop, playing}`.
  - `CameraRig2D` (`kTypeId = 4`, `alignas(8)`, 40B): `{viewport, scissor, zoom, viewSlot, screenSpace, active}`.

## 3. Subsystem APIs
### `ComponentPool<T>` (`engine/world/ComponentPool.h` — Sparse-Set Storage)
- `Contains(id)`, `TryGet(id)` $\to$ `T*`, `DirectAtEntityIndex(idx)` $\to$ `T&`
- `Emplace(id, args...)` $\to$ `Result<T*, WorldErrc>`, `Remove(id)` $\to$ `bool`
- `Size()`, `GroupSize()`, `Entities()` $\to$ `span<const EntityId>`, `Components()` $\to$ `span<T>`.

### `World` (`engine/world/World.h` — Simulation Container)
- **Lifecycle & Pools:**
  - `Init(WorldConfig)` $\to$ `Result<void, WorldErrc>`, `Shutdown()`, `Clear()`
  - `RegisterComponentPool<T>(capacity)` $\to$ `Result<void, WorldErrc>`
- **Entity & Component Operations:**
  - `Spawn(EntitySpawnDesc)` $\to$ `Result<EntityId, WorldErrc>`, `Destroy(id)`, `IsAlive(id)`
  - `Emplace<T>(id, args...)` $\to$ `Result<T*, WorldErrc>`, `Remove<T>(id)`, `TryGet<T>(id)`, `Has<T>(id)`
  - `DeferDestroy(id)`, `DeferSetParent(child, parent)`, `DeferEmplace<T>(id, comp)`, `DeferRemove<T>(id)`
  - `FlushCommands()` $\to$ `Result<void, WorldErrc>`
- **Spatial & Hierarchy:**
  - `GetLocalTransform(id)`, `SetLocalTransform(id, local)`, `Teleport(id, local)`
  - `GetWorldTransform(id)`, `SetParent(child, parent)` $\to$ `Result<void, WorldErrc>`, `PropagateTransforms()`
  - `View<IncludeTs...>(Exclude<ExcludeTs...> = {})`: Multi-component bitmask iterator selecting smallest lead pool.
- **120Hz Simulation Tick:**
  - `BeginTick(SimStepView)`: Snapshots previous transforms for active channels.
  - `StepAmbientVelocities(SimStepView)`: Integrates `Velocity2D` for unparented/ambient entities.
  - `EndTick(SimStepView)`: Flushes deferred commands, propagates dirty transforms, syncs teleports.
  - `ComputeAuditHash()` $\to$ `u64`: Deterministic FNV-1a state hash.

### `WorldExtractor` (`engine/world/WorldExtractor.h` — Render & Prefab Bridge)
- `Extract(world, assetBank, timeView, renderList)`: Interpolates transforms and emits `QuadCmd`s into `RenderList`.
- `InstantiatePrefab(world, assetBank, blobHandle, rootTransform, outEntities)` $\to$ `Result<EntityId, WorldErrc>`.

## 4. Main Loop Integration
```cpp
// 120Hz deterministic simulation tick
m_world.BeginTick(step);
m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer); // Stages 1..6
m_world.EndTick(step); // Flushes commands, propagates transforms, syncs teleports

// Render frame extraction
auto& renderList = m_renderer.BeginFrame();
world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), renderList);
m_renderer.Submit(renderList);
```
# Phase 7: `world` Subsystem (v2 Final Production Architecture — Locked)

## 1. Architectural Overview & Strict 2-Layer Decoupling
The `world` subsystem implements a zero-allocation, deterministic, data-oriented 2D Entity-Component-System (ECS) and Left-Child Right-Sibling (LCRS) hierarchical scene graph built exclusively on `core` (`Arena`, `Result`, `Hash`, `Math`).

- **Layer 1 (`world/WorldTypes.h`, `world/ComponentPool.h`, `world/TransformSolver.h/.cpp`, `world/World.h/.cpp`)**: Strictly depends ONLY on `core` (`#include "core/..."`). Zero dependency on `renderer`, `assets`, `platform`, `input`, or `time`. Fully `constexpr`-testable in memory.
- **Layer 2 (`world/WorldExtractor.h/.cpp`)**: Stateless presentation and prefab bridge that translates hot ECS component pools into `renderer::RenderBatch` draw commands and instantiates `assets::AssetManager` cooked prefab payloads (`NPRF` v1).

## 2. Exact Byte-Tight Memory Layouts & Invariants (`0B` Padding)
All structs are explicitly laid out with zero hidden compiler padding and verified by compile-time `static_assert`:
- `EntityId` (`4B`, `alignas(4)`): `u16 index` + `u16 generation` (`0` = invalid sentinel; valid generations in `[1, 0xFFFF]`, wrapping `0xFFFF -> 1`).
- `WorldConfig` (`12B`, `alignas(4)`): `u16 maxEntities`, `u16 maxCommandRecords`, `u32 commandPayloadCapBytes`, `u16 defaultPoolCapacity`, `u16 flags`.
- `LocalTransform2D` (`24B`, `alignas(4)`): `Vec2 position (8B)`, `Vec2 scale (8B)`, `f32 rotationRad (4B)`, `f32 zOrder (4B)`.
- `PrevWorldSnapshot2D` (`24B`, `alignas(4)`): `Vec2 worldPosition (8B)`, `Vec2 worldScale (8B)`, `f32 worldRotationRad (4B)`, `f32 worldZOrder (4B)`.
- `WorldTransform2D` (`32B`, `alignas(16)`): `Vec2 worldPosition (8B)`, `Vec2 worldScale (8B)`, `f32 worldRotationRad (4B)`, `f32 worldZOrder (4B)`, `f32 cosRot (4B)`, `f32 sinRot (4B)`.
- `EntitySpawnDesc` (`32B`, `alignas(4)`): `LocalTransform2D local (24B)`, `EntityId parent (4B)`, `u16 flags (2B)`, `u8 keepWorldTransform (1B)`, `u8 TeleportOnSpawn (1B)`.
- Built-in Components (Type IDs `0..7`):
  - `Velocity2D` (`16B`, `alignas(16)`, `kTypeId = 0`)
  - `Sprite2D` (`32B`, `alignas(16)`, `kTypeId = 1`)
  - `SpriteAnimator2D` (`16B`, `alignas(4)`, `kTypeId = 2`)
  - `CameraRig2D` (`32B`, `alignas(16)`, `kTypeId = 3`)
  - `Lifetime2D` (`16B`, `alignas(4)`, `kTypeId = 4`)
  - `PointLight2D` (`32B`, `alignas(16)`, `kTypeId = 5`)
  - `Tag2D` (`16B`, `alignas(4)`, `kTypeId = 6`)
- `RawComponentPool` & `ComponentPool<T>` (`32B`, `alignas(32)`): Non-template type-erased base class + typed derived view (`void* m_denseData`, `u16* m_sparse`, `EntityId* m_denseEntities`, `u16 m_count`, `u16 m_capacity`, `u16 m_maxEntities`, `u16 m_elemSize`). Eliminates `PoolOps` function-pointer table indirection and double-pointer hops.
- `TransformSolver` (`64B`, `alignas(64)`, 1 cache line exact): Flat-array LCRS tree (`m_parent`, `m_firstChild`, `m_nextSibling`, `m_prevSibling`, `m_hierarchyDepth`) + `u64* m_dirtyBits` (`1 bit` per entity) + unified `TraverseSubtree` and `InverseCombineParentChild` for world-transform preservation.
- `World` (`192B`, `alignas(64)`, 3 cache lines exact): Direct `RawComponentPool* m_pools` array (`64 * 32B`), FIFO entity recycling queue (`m_freeIndices`), 64-bit entity component signatures (`m_componentMasks`), and deterministic deferred command buffer.

## 3. Core 2026 Industry Mechanisms
1. **FIFO Generational Entity Recycling**: Minimum reuse distance via circular queue (`m_freeHead`, `m_freeTail`, `m_freeCount`) prevents immediate slot reuse and catches stale handles across frames.
2. **Non-Template `RawComponentPool` Base Class**: Type-erased operations (`RemoveUnchecked`, `CloneFromEntityIndex`, `EmplaceRaw`, `GetRawByEntityIndex`, `Clear`) execute directly on `m_pools[tid]` without virtual tables or function-pointer tables.
3. **World-Transform Preservation (`keepWorldTransform`)**: `SetParent(child, newParent, keepWorldTransform = true)` and non-cascading `UnlinkSlot` invert the parent affine transform (`InverseCombineParentChild`) so reparented or unparented children maintain their exact world-space pose.
4. **Cascading & Non-Cascading Hierarchy Destruction**: `Destroy(id, destroyChildren = true)` and `DeferDestroy(id, destroyChildren = true)` either destroy the entire LCRS subtree or promote children to root while preserving their world transform.
5. **Immediate Handle Reservation in `DeferSpawn`**: `DeferSpawn(desc)` allocates the generational `EntityId` immediately while keeping `m_componentMasks[idx] == 0ULL` until `FlushCommands()`, enabling same-tick `DeferEmplace(newId, ...)` and `DeferSetParent(child, newId)`.
6. **Subtree Cloning & Multi-Component `View<Cs...>()`**: `CloneEntity(src, overrideLocal, cloneChildren = true)` duplicates an entity or its entire LCRS hierarchy and all components; `View<Cs...>()` provides range-for structured bindings (`for (auto [e, vel, spr] : world.View<Velocity2D, Sprite2D>())`) iterating the smallest pool and checking the `u64` signature mask first.
7. **Binary Prefab Serialization & Instantiation (`NPRF` v1)**: `SerializePrefabTemplate(root, buf)` and `InstantiatePrefab(bytes, rootLocal, keepRootLocal)` support full hierarchical multi-entity prefabs with topologically ordered parent indices (`parentPrefabIndex < i` or `0xFFFF`).
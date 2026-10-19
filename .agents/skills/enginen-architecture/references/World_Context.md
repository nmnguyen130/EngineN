# EngineN — World & Entity Subsystem Context (`world/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single right-sized `4 MB` 64-byte aligned `AlignedArenaLine[]` slab), zero virtual dispatch (`32B` `PoolOps` function-pointer table), minimal 5-file footprint (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp`) + `tests/WorldTests.cpp`, two-layer pure C++23 separation (`namespace en::world`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Leaks:**
  - **Layer 1 (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`):** Pure C++23 `192B` (3-cache-line) authoritative simulation container, intrusive FIFO generational entity allocator (`EntityId` `4B`), single-array `IsAlive` check (`m_generations[idx] != 0`), direct-indexed SoA spatial spine, `64B` 1-cache-line iterative topological hierarchy solver (`TransformSolver`), `32B` arena-backed sparse-set component pools (`ComponentPool<T>`) with C++23 Deduced `this`, and alignment-safe deferred command tape (`EmplaceRaw`). Depends solely on `"core/Result.h"`, `"core/Types.h"`, `"core/time/TimeTypes.h"`, `"core/time/TimePipeline.h"`, `"assets/AssetTypes.h"`, and `"renderer/RendererTypes.h"` — zero `AssetBank.h`, `RenderList.h`, `platform/`, or `<SDL3/SDL_*.h>` includes; 100% testable in headless CI via `World::ComputeAuditHash()`.
  - **Layer 2 (`WorldExtractor.h/.cpp`):** Stateless presentation & prefab bridge. Extracts interpolated world state + 1-hop `assets::AssetBank::GetSprite()` / `GetAtlasFrame()` frames into `renderer::RenderList::PushQuad` / `SetView`, and instantiates zero-copy binary prefabs (`CookedPrefabHeader`) from `assets::BlobRecord` (`Residency::Sim`) with immediate hierarchical transform propagation.
- **L1 Cache Line Partitioning, Right-Sized Arena & Zero Stack Bloat:**
  - **`World` (`sizeof == 192`, `alignas(64)`, 3 Cache Lines Exact):** `m_poolOps` (`2 KB`) is carved inside the arena slab rather than embedded on the stack.
    - **Cache Line 0 (`offset 0..63`):** All hot 120 Hz / 240 Hz pointers and counters (`m_generations`, `m_componentMasks`, `m_timeChannels`, `m_aliveWords`, `m_teleportedWords`, `m_poolOps`, `m_maxEntities`, `m_maxAllocatedIndex`, `m_nextFreshSlot`, `m_freeHead`, `m_freeTail`, `m_defaultPoolCap`, `m_aliveCount`).
    - **Cache Line 1 (`offset 64..127`):** `TransformSolver m_transforms` (`64B` exact).
    - **Cache Line 2 (`offset 128..191`):** Command tape and right-sized `4 MB` 64B-aligned arena slab (`WorldConfig::arenaBytes = 4 MB`, 75% smaller than the legacy `16 MB` slab).
  - **Single-Array `IsAlive(EntityId)` & Intrusive FIFO Freelist (`0` Freelist Array):** Dead/unallocated slots store `m_generations[idx] == 0`. Destroyed slots thread `{nextGen, nextFreeIdx}` directly inside their unused `m_componentMasks[idx]`, eliminating both `m_flags` (`16 KB`) and `m_freeRing` (`64 KB`) as well as modulo `%` division.
  - **`TransformSolver` (`sizeof == 64`, `alignas(64)`, 1 Cache Line Exact, Zero Recursion):** Replaces 5 separate bitset/depth arrays (`m_depths`, `m_spatialDynamicWords`, `m_rootDirtyWords`, `m_childDirtyWords`, `m_subtreeDirtyWords`) with unified `m_dirtyWords` (+ `SpatialAliveWords`) and `m_movedWords`. `MarkDirty(idx)` is a single branchless bitwise OR; `RebuildHierarchyOrder()` and `MaxSubtreeDepth()` are 100% iterative (`32`-entry stack, zero recursion).
  - **`ComponentPool<T>` (`sizeof == 32`, `alignas(32)`) & `PoolOps` (`sizeof == 32`, `alignas(32)`):** Uses C++23 Deduced `this` (`this Self&& self`) with zero `const`/non-`const` duplication. Eliminates `m_presenceWords` and lockstep group swap overhead; `Clear()` resets only `m_size` active sparse entries (`O(m_size)`), `RemoveAtDense()` does a single move from `lastIdx`, and `EmplaceRaw()` uses `std::memcpy` for alignment safety.
- **Moved-Only Snapshot Syncing & Stationary Sprite Fast Path (`m_movedWords` Invariant):**
  - **Invariant:** Whenever bit `idx` in `m_movedWords` is `0`, `m_prev[idx]` is guaranteed identical to `m_world[idx].ToSnapshot()`.
  - `BeginTick()` (`SnapshotPreviousTransforms`) scans only `m_movedWords` and skips `m_timeChannels` loads when `activeChannelMask == 0xFFu`.
  - `WorldExtractor::Extract()` tests `movedWords[idx >> 6] & (1ULL << (idx & 63u))` before touching `prevSpan[idx]` or `chSpan[idx]`, and resolves animated sprites directly via `(world.GetComponentMaskByIndex(idx) & (1ULL << SpriteAnimator2D::kTypeId)) != 0` -> `assetBank.GetAtlasFrame(...)`.

## 2. Core Types (`world/WorldTypes.h` & `world/WorldExtractor.h`)
- **Identifiers, Concepts & Config:**
  - `struct alignas(4) EntityId { u16 index{0}; u16 generation{0}; };` (`sizeof == 4`, `kNullEntity = {0, 0}`, `kInvalidDenseIndex = 0xFFFF`, `kMaxComponentTypes = 64`, `kMaxHierarchyDepth = 32`).
  - `using ComponentMask = u64;` — Constructed at compile time via `consteval MakeComponentMask<Ts...>()`.
  - `concept Component<T>`: Requires trivially copyable, trivially destructible, standard layout, `alignof(T) <= 64`, and `T::kTypeId < 64`.
  - `struct WorldConfig` (`sizeof == 12`): `maxEntities{16384}`, `defaultComponentPoolCapacity{4096}`, `commandBufferBytes{262144}`, `arenaBytes{4 * 1024 * 1024}`.
- **Built-In Spatial & Visual Components (Zero-Padding Layouts):**
  - `struct LocalTransform2D` (`kTypeId = 0`, `sizeof == 24`): `x`, `y`, `scaleX`, `scaleY`, `rotationRad`, `depthOffset`.
  - `struct PrevWorldSnapshot2D` (`sizeof == 24`): `x`, `y`, `scaleX`, `scaleY`, `worldRotRad`, `worldDepth`.
  - `struct alignas(32) WorldTransform2D` (`sizeof == 32`): Leading 24B (`x`, `y`, `scaleX`, `scaleY`, `worldRotRad`, `worldDepth`) matches `PrevWorldSnapshot2D` (`ToSnapshot()`), followed by cached `cosRot`, `sinRot`.
  - `struct alignas(16) Velocity2D` (`kTypeId = 1`, `sizeof == 16`): `vx`, `vy`, `angularVelRad`, `linearDamping`.
  - `struct alignas(32) Sprite2D` (`kTypeId = 2`, `sizeof == 32`): `tint`, `sprite`, `baseDepth`, `ySortScale`, `localOffsetX`, `localOffsetY`, `passId`, `layer`, `viewSlot`, `blendAndFlags`.
  - `struct alignas(16) SpriteAnimator2D` (`kTypeId = 3`, `sizeof == 16`): `atlas`, `firstFrame`, `frameCount`, `currentFrame`, `ticksPerFrame`, `subTickAccum` (`kSubTicksPerTick = 256`), `loop`, `playing`.
  - `struct alignas(8) CameraRig2D` (`kTypeId = 4`, `sizeof == 40`): `viewport`, `scissor`, `zoom`, `viewSlot`, `screenSpace`, `active`, `pad`.
- **Cooked Prefab Binary Formats:**
  - `struct alignas(16) CookedPrefabHeader` (`16B`, magic `'EPFB'`) & `struct alignas(16) CookedPrefabEntityRecord` (`64B`).

## 3. Public Interfaces & Signatures
### `ComponentPool<T>` (`world/ComponentPool.h` — 32B Arena-Backed Sparse-Set Storage)
- `[[nodiscard]] constexpr bool Contains(EntityId id) const noexcept`
- `template <typename Self> [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept`
- `template <typename Self> [[nodiscard]] constexpr auto& DirectAtEntityIndex(this Self&& self, u16 entityIndex) noexcept`
- `template <typename Self> [[nodiscard]] constexpr auto& ComponentAtDense(this Self&& self, usize denseIdx) noexcept`
- `template <typename... Args> [[nodiscard]] constexpr Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept`
- `[[nodiscard]] Result<void, WorldErrc> EmplaceRaw(EntityId id, const void* rawComponent) noexcept`
- `constexpr bool Remove(EntityId id) noexcept`, `constexpr void RemoveUnchecked(EntityId id) noexcept`
- `[[nodiscard]] constexpr u16 Size() const noexcept`, `Capacity() const noexcept`, `DenseIndexOf(u16 entityIndex) const noexcept`
- `[[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept`, `constexpr auto Components(this Self&& self) noexcept`

### `World` (`world/World.h` — 192B 3-Cache-Line Move-Only RAII Simulation Container)
- Lifecycle & Component Registration:
  - `[[nodiscard]] Result<void, WorldErrc> Init(const WorldConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`, `void Clear() noexcept`
  - `template <Component T> [[nodiscard]] Result<void, WorldErrc> RegisterComponentPool(u16 capacity = 0) noexcept`
- Immediate & Deferred Entity/Component Operations:
  - `[[nodiscard]] Result<EntityId, WorldErrc> Spawn(const EntitySpawnDesc& desc = {}) noexcept`
  - `bool Destroy(EntityId id) noexcept`, `[[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept`
  - `[[nodiscard]] constexpr u32 AliveCount() const noexcept`, `MaxAllocatedIndex() const noexcept`
  - `template <Component T, typename... Args> [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept`
  - `template <Component T> bool Remove(EntityId id) noexcept`
  - `template <Component T, typename Self> [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept`
  - `template <Component T> [[nodiscard]] constexpr bool Has(EntityId id) const noexcept`
  - `template <Component T, typename Self> [[nodiscard]] constexpr auto& GetPool(this Self&& self) noexcept`
  - `[[nodiscard]] Result<void, WorldErrc> DeferDestroy(EntityId id) noexcept`, `DeferSetParent(EntityId child, EntityId parent) noexcept`
  - `template <Component T> [[nodiscard]] Result<void, WorldErrc> DeferEmplace(EntityId id, const T& comp) noexcept`, `DeferRemove(EntityId id) noexcept`
  - `[[nodiscard]] Result<void, WorldErrc> FlushCommands() noexcept`
- Spatial, Hierarchy & Query Iteration:
  - `GetLocalTransform(EntityId)`, `ModifyLocalTransformByIndex(u16)`, `SetLocalTransform(EntityId, const LocalTransform2D&)`, `Teleport(EntityId, const LocalTransform2D&)`
  - `GetWorldTransform(EntityId)`, `GetWorldByIndex(u16)`, `GetPrevWorldByIndex(u16)`, `GetTimeChannelByIndex(u16)`, `GetComponentMaskByIndex(u16)`
  - `WorldTransforms()`, `PrevWorldTransforms()`, `TimeChannels()`, `MovedWords()`
  - `SetParent(EntityId child, EntityId parent)`, `PropagateTransforms()`
  - `template <Component... IncludeTs, Component... ExcludeTs, typename Self> [[nodiscard]] constexpr auto View(this Self&& self, Exclude<ExcludeTs...> = {}) noexcept`
- 120 Hz Simulation Tick & Determinism Audit:
  - `void BeginTick(time::SimStepView step) noexcept`
  - `void StepAmbientVelocities(time::SimStepView step) noexcept`
  - `void StepAnimations(time::SimStepView step) noexcept`
  - `void EndTick(time::SimStepView step = {}) noexcept`
  - `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `WorldExtractor` (`world/WorldExtractor.h` — Stateless Render & Prefab Bridge)
- `static void Extract(const World& world, const assets::AssetBank& assetBank, const time::RenderTimeView& timeView, renderer::RenderList& outRenderList) noexcept`
- `[[nodiscard]] static Result<EntityId, WorldErrc> InstantiatePrefab(World& world, const assets::AssetBank& assetBank, assets::BlobHandle prefabBlob, const LocalTransform2D& rootTransform = {}, std::span<EntityId> outSpawnedEntities = {}) noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_assets.Pump(m_renderer);
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.StepAmbientVelocities(step);
    m_world.StepAnimations(step);
    m_world.EndTick(step);
}
renderer::RenderList& list = m_renderer.BeginFrame(m_window.State().pixelSize);
world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), list);
m_renderer.Submit(list);
```
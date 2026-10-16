# EngineN — World & Entity Subsystem Context (`world/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocation in hot paths (single 16 MB pre-allocated contiguous arena slab at `Init()`), zero virtual dispatch (`PoolOps` function-pointer table), minimal 5-file footprint (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp`), two-layer pure C++ separation (zero OS or SDL3 dependencies).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`):** Authoritative simulation container, FIFO generational entity allocator, split SoA metadata planes, direct-indexed spatial spine, intrusive topological hierarchy solver, arena-backed sparse-set component pools with lockstep owning groups, and deferred command tape. 100% testable in headless CI via `World::ComputeAuditHash()`.
  - **Layer 2 (`WorldExtractor.h/.cpp`):** Stateless presentation & asset bridge. Extracts interpolated world state + 1-hop `assets::AssetBank::GetSprite()` frames into `renderer::RenderList::PushQuad` / `SetView`, and instantiates zero-copy binary prefabs (`CookedPrefabHeader`) from `assets::BlobRecord` (`Residency::Sim`).
- **1:1 Direct-Indexed Spatial Spine & 64-Bit Hierarchical Word Bitsets:**
  - `m_local[]`, `m_world[]`, and `m_prev[]` are indexed directly by `EntityId::index` (zero indirection hops).
  - Empty, dead, static, or clean slots are skipped in $O(1)$ blocks of 64 entities per CPU instruction (`std::countr_zero`) using parallel 64-bit word bitsets (`m_aliveWords`, `m_spatialAliveWords`, `m_spatialDynamicWords`, `m_rootDirtyWords`, `m_childDirtyWords`, `m_teleportedWords`).
- **Zero-Padding Spatial Layout (`80B`/Entity Total) & Trig-Free Fast Path:**
  - `LocalTransform2D` (`24B`), `WorldTransform2D` (`32B`, `alignas(32)`, caches `cosRot`, `sinRot`, `worldRotRad`, `worldDepth`), and `PrevWorldSnapshot2D` (`24B`) contain zero padding bytes.
  - Stationary or hitstop-frozen entities (`alpha == 1.0f` or `t0.worldRotRad == t1.worldRotRad`) copy cached `(cosRot, sinRot)` directly into `QuadCmd` with zero math. Rotating entities interpolate shortest-arc radians (`ShortestAngleDelta`) and evaluate a branchless 5th-order minimax polynomial (`FastSinCos`, ~6 cycles, zero `sqrt` or `libm` calls).
- **Intrusive Doubly-Linked Forest & Flat Topological Hierarchy Array:**
  - Combines per-slot intrusive tree links (`HierarchyNode`: `parent, firstChild, nextSibling, prevSibling`, `8B`) with a flat breadth-first/DFS execution array (`m_hierarchyOrder[]` of `HierarchyStep { childIdx, parentIdx }`, `4B`) where parents strictly precede descendants.
  - Cycle and depth validation (`kMaxHierarchyDepth = 32`) runs in $O(\text{depth})$ on `SetParent()`.
- **Lockstep Full-Owning Group (`Sprite2D` + `SpriteAnimator2D`) & Q8.8 Sub-Tick Animation:**
  - Within `ComponentPool<Sprite2D>`, the leading `[0 .. GroupSize())` dense slots are maintained in 1:1 lockstep index alignment with `ComponentPool<SpriteAnimator2D>` via `PromoteToGroup()` / `DemoteFromGroup()`.
  - `StepKinematicsAndAnimations()` updates animated sprites in a single linear loop (`sprites[i].sprite = bank.GetSpriteHandle(...)`) with zero sparse lookups. `SpriteAnimator2D` uses a Q8.8 fixed-point sub-tick accumulator (`kSubTicksPerTick = 256`) for drift-free playback across fractional `TimeChannel` slow-motion ramps.
- **Permanent Slot 0 Sentinel & Teleport Synchronization:**
  - Slot `0` (`kNullEntity = {0, 0}`) is permanently initialized as an invalid/root identity transform sentinel (`generation = 0`). Recycled indices use a FIFO ring buffer (`m_freeRing`), and generation wrap-around skips `0`.
  - Newly spawned or teleported entities set `EntityFlags::Teleported`; `World::EndTick()` synchronizes `m_prev[idx] = m_world[idx]` after transform propagation to prevent interpolation streaking.

## 2. Core Types (`world/WorldTypes.h` & `world/WorldExtractor.h`)
- **Identifiers, Concepts & Masks:**
  - `struct EntityId { u16 index{0}; u16 generation{0}; };` (`kNullEntity = {0, 0}`, `kInvalidDenseIndex = 0xFFFF`, `kMaxComponentTypes = 64`, `kMaxHierarchyDepth = 32`).
  - `using ComponentMask = u64;` — Constructed at compile time via `consteval MakeComponentMask<Ts...>()`.
  - `concept Component<T>`: Requires trivially copyable, trivially destructible, standard layout, `alignof(T) <= 64`, and `T::kTypeId < 64`.
  - `enum class WorldErrc : u8`: `InvalidConfig`, `OutOfMemory`, `EntityPoolExhausted`, `ComponentPoolExhausted`, `CommandBufferOverflow`, `InvalidEntity`, `ComponentNotRegistered`, `HierarchyCycleDetected`, `HierarchyDepthExceeded`, `CorruptedPrefabBlob`.
  - `enum class EntityFlags : u8`: `None`, `Alive`, `Enabled`, `Spatial`, `StaticSpatial`, `Teleported`.
- **Built-In Spatial & Gameplay Components (Zero-Padding Layouts):**
  - `struct LocalTransform2D` (`kTypeId = 0`, `sizeof == 24`): `x`, `y`, `scaleX`, `scaleY`, `rotationRad`, `depthOffset`.
  - `struct alignas(32) WorldTransform2D` (`sizeof == 32`): `x`, `y`, `scaleX`, `scaleY`, `cosRot`, `sinRot`, `worldRotRad`, `worldDepth`.
  - `struct PrevWorldSnapshot2D` (`sizeof == 24`): `x`, `y`, `scaleX`, `scaleY`, `worldRotRad`, `worldDepth`.
  - `struct alignas(16) Velocity2D` (`kTypeId = 1`, `sizeof == 16`): `vx`, `vy`, `angularVelRad`, `linearDamping`.
  - `struct alignas(32) Sprite2D` (`kTypeId = 2`, `sizeof == 32`): `tint` (`renderer::Color` at offset `0`), `sprite` (`assets::SpriteHandle` at offset `16`), `baseDepth`, `ySortScale`, `localOffsetX`, `localOffsetY`, `passId`, `layer`, `viewSlot`, `blendAndFlags`.
  - `struct alignas(16) SpriteAnimator2D` (`kTypeId = 3`, `sizeof == 16`): `atlas` (`assets::AtlasHandle`), `firstFrame`, `frameCount`, `currentFrame`, `ticksPerFrame`, `subTickAccum` (`kSubTicksPerTick = 256`), `loop`, `playing`.
  - `struct alignas(8) CameraRig2D` (`kTypeId = 4`, `sizeof == 40`): `viewport`, `scissor`, `zoom`, `viewSlot`, `screenSpace`, `active`.
- **Cooked Prefab Binary Formats:**
  - `struct alignas(16) CookedPrefabHeader` (`16B`, magic `'EPFB'`) & `struct alignas(16) CookedPrefabEntityRecord` (`64B`).

## 3. Public Interfaces & Signatures
### `ComponentPool<T>` (`world/ComponentPool.h` — Arena-Backed Sparse-Set & Lockstep Group Storage)
- `[[nodiscard]] constexpr bool Contains(EntityId id) const noexcept`
- `template <typename Self> [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept`
- `template <typename Self> [[nodiscard]] constexpr auto& DirectAtEntityIndex(this Self&& self, u16 entityIndex) noexcept`
- `template <typename... Args> [[nodiscard]] std::expected<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept`
- `bool Remove(EntityId id) noexcept`
- `[[nodiscard]] constexpr u16 Size() const noexcept`, `[[nodiscard]] constexpr u16 GroupSize() const noexcept`
- `[[nodiscard]] constexpr std::span<const EntityId> Entities() const noexcept`, `constexpr auto Components(this Self&& self) noexcept`

### `World` (`world/World.h` — Move-Only RAII Authoritative Simulation Container)
- Lifecycle & Component Registration:
  - `[[nodiscard]] std::expected<void, WorldErrc> Init(const WorldConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`, `void Clear() noexcept`
  - `template <Component T> [[nodiscard]] std::expected<void, WorldErrc> RegisterComponentPool(u16 capacity = 0) noexcept`
- Immediate & Deferred Entity/Component Operations:
  - `[[nodiscard]] std::expected<EntityId, WorldErrc> Spawn(const EntitySpawnDesc& desc = {}) noexcept`
  - `bool Destroy(EntityId id) noexcept`, `[[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept`
  - `template <Component T, typename... Args> [[nodiscard]] std::expected<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept`
  - `template <Component T> bool Remove(EntityId id) noexcept`
  - `template <Component T, typename Self> [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept`
  - `template <Component T> [[nodiscard]] constexpr bool Has(EntityId id) const noexcept`
  - `[[nodiscard]] std::expected<void, WorldErrc> DeferDestroy(EntityId id) noexcept`, `DeferSetParent(EntityId child, EntityId parent) noexcept`
  - `template <Component T> [[nodiscard]] std::expected<void, WorldErrc> DeferEmplace(EntityId id, const T& comp) noexcept`, `DeferRemove(EntityId id) noexcept`
  - `[[nodiscard]] std::expected<void, WorldErrc> FlushCommands() noexcept`
- Spatial, Hierarchy & Query Iteration:
  - `[[nodiscard]] constexpr const LocalTransform2D& GetLocalTransform(EntityId id) const noexcept`
  - `void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept`, `void Teleport(EntityId id, const LocalTransform2D& local) noexcept`
  - `[[nodiscard]] constexpr const WorldTransform2D& GetWorldTransform(EntityId id) const noexcept`
  - `[[nodiscard]] std::expected<void, WorldErrc> SetParent(EntityId child, EntityId parent) noexcept`, `void PropagateTransforms() noexcept`
  - `template <Component... IncludeTs, Component... ExcludeTs, typename Self> [[nodiscard]] constexpr auto View(this Self&& self, Exclude<ExcludeTs...> = {}) noexcept`
- 120 Hz Simulation Tick & Determinism Audit:
  - `void BeginTick(time::SimStepView step) noexcept`
  - `void StepKinematicsAndAnimations(time::SimStepView step, const assets::AssetBank& assetBank) noexcept`
  - `void EndTick(time::SimStepView step) noexcept`
  - `void Tick(time::SimStepView step, const assets::AssetBank& assetBank) noexcept`
  - `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `WorldExtractor` (`world/WorldExtractor.h` — Stateless Render & Prefab Bridge)
- `static void Extract(const World& world, const assets::AssetBank& assetBank, time::RenderTimeView timeView, renderer::RenderList& outRenderList) noexcept`
- `[[nodiscard]] static std::expected<EntityId, WorldErrc> InstantiatePrefab(World& world, const assets::AssetBank& assetBank, assets::BlobHandle prefabBlob, const LocalTransform2D& rootTransform = {}, std::span<EntityId> outSpawnedEntities = {}) noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_assets.Pump(m_renderer);
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, step); // Queries m_world.View<...>() and records Defer*() commands
    m_world.StepKinematicsAndAnimations(step, m_assets.Bank());
    m_world.EndTick(step);          // Flushes command tape, propagates hierarchy, syncs teleports
}
renderer::RenderList& list = m_renderer.BeginFrame();
world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), list);
m_renderer.Submit(list);
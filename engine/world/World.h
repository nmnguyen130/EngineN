#pragma once

#include "world/ComponentPool.h"
#include "world/TransformSolver.h"

#include <tuple>

namespace en {

class AssetBank;

// 192-byte (3 cache lines exact, zero padding) authoritative 64-component Sparse-Set ECS simulation container.
// Move-only RAII owner of a single 64B-aligned arena slab; zero runtime heap allocations after Init and zero indirect function-pointer calls.
// Invariant: EntityId index 0 is reserved for kNullEntity, destroyed slots recycle in FIFO order, and structural deferred commands flush at EndTick.
class alignas(64) World {
public:
    World() noexcept = default;
    ~World() noexcept { Shutdown(); }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] Result<void, WorldErrc> Init(const WorldConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;
    void Clear() noexcept;

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> RegisterComponentPool(u16 capacity = 0U) noexcept {
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if (m_pools == nullptr) return Err(WorldErrc::InvalidConfig);
        if ((m_registeredPoolsMask & (1ULL << tid)) != 0ULL) return {};
        const u16 cap = (capacity > 0U) ? capacity : m_defaultPoolCap;
        if (!static_cast<ComponentPool<T>&>(m_pools[tid]).Init(m_arena, m_maxEntities, cap)) {
            return Err(WorldErrc::OutOfMemory);
        }
        m_registeredPoolsMask |= (1ULL << tid);
        return {};
    }

    [[nodiscard]] Result<EntityId, WorldErrc> Spawn(const EntitySpawnDesc& desc = {}) noexcept;
    bool Destroy(EntityId id, bool destroyChildren = true) noexcept;
    [[nodiscard]] Result<EntityId, WorldErrc> CloneEntity(
        EntityId source,
        const LocalTransform2D& overrideLocal,
        bool cloneChildren = true) noexcept;

    [[nodiscard]] EntityId FindByTag(StringId tagName) const noexcept;
    [[nodiscard]] u32 FindByCategory(u32 categoryMask, std::span<EntityId> outEntities) const noexcept;

    [[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept {
        return id.index > 0U && id.index < m_maxEntities && m_generations[id.index] == id.generation;
    }
    [[nodiscard]] constexpr u32 AliveCount() const noexcept { return m_aliveCount; }
    [[nodiscard]] constexpr u16 MaxAllocatedIndex() const noexcept { return m_maxAllocatedIndex; }
    [[nodiscard]] constexpr u16 MaxEntities() const noexcept { return m_maxEntities; }
    [[nodiscard]] constexpr u64 RegisteredPoolsMask() const noexcept { return m_registeredPoolsMask; }
    [[nodiscard]] constexpr u16 PendingCommandCount() const noexcept { return m_pendingCmdCount; }
    [[nodiscard]] constexpr u64 TotalSpawnedCount() const noexcept { return m_spawnedTotalCount; }
    [[nodiscard]] constexpr u64 TotalDestroyedCount() const noexcept { return m_destroyedTotalCount; }

    template <Component T, typename... Args>
    [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (!IsAlive(id)) return Err(WorldErrc::DeadEntity);
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if ((m_registeredPoolsMask & (1ULL << tid)) == 0ULL) {
            if (auto r = RegisterComponentPool<T>(); !r) return Err(r.error());
        }
        auto res = GetPool<T>().Emplace(id, std::forward<Args>(args)...);
        if (res) m_componentMasks[id.index] |= (1ULL << tid);
        return res;
    }

    template <Component T>
    bool Remove(EntityId id) noexcept {
        if (!IsAlive(id)) return false;
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if ((m_registeredPoolsMask & (1ULL << tid)) == 0ULL) return false;
        if (!GetPool<T>().Remove(id)) return false;
        m_componentMasks[id.index] &= ~(1ULL << tid);
        return true;
    }

    template <Component T>
    [[nodiscard]] constexpr bool Has(EntityId id) const noexcept {
        return IsAlive(id) &&
               ((m_componentMasks[id.index] & (1ULL << static_cast<u8>(T::kTypeId))) != 0ULL);
    }

    template <Component T>
    [[nodiscard]] constexpr T* TryGet(EntityId id) noexcept {
        return Has<T>(id) ? &GetPool<T>().DirectAtEntityIndex(id.index) : nullptr;
    }

    template <Component T>
    [[nodiscard]] constexpr const T* TryGet(EntityId id) const noexcept {
        return Has<T>(id) ? &GetPool<T>().DirectAtEntityIndex(id.index) : nullptr;
    }

    template <Component T>
    [[nodiscard]] constexpr ComponentPool<T>& GetPool() noexcept {
        return static_cast<ComponentPool<T>&>(m_pools[static_cast<u8>(T::kTypeId)]);
    }

    template <Component T>
    [[nodiscard]] constexpr const ComponentPool<T>& GetPool() const noexcept {
        return static_cast<const ComponentPool<T>&>(m_pools[static_cast<u8>(T::kTypeId)]);
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> excl = {}) noexcept {
        DispatchEach<FirstC, RestCs...>(*this, std::forward<Fn>(fn), excl);
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> excl = {}) const noexcept {
        DispatchEach<FirstC, RestCs...>(*this, std::forward<Fn>(fn), excl);
    }

    // Range-based view over smallest driving pool yielding std::tuple<EntityId, Cs&...>
    template <typename WorldPtr, Component FirstC, Component... RestCs>
    class ViewRange {
    public:
        struct Sentinel {};

        struct Iterator {
            WorldPtr world{nullptr};
            const EntityId* cur{nullptr};
            const EntityId* end{nullptr};
            u64 reqMask{0};
            u64 exclMask{0};

            constexpr void AdvanceToValid() noexcept {
                while (cur != end) {
                    const u64 mask = world->m_componentMasks[cur->index];
                    if ((mask & reqMask) == reqMask && (mask & exclMask) == 0ULL) break;
                    ++cur;
                }
            }

            constexpr Iterator& operator++() noexcept {
                ++cur;
                AdvanceToValid();
                return *this;
            }

            [[nodiscard]] constexpr bool operator==(Sentinel) const noexcept { return cur == end; }

            [[nodiscard]] constexpr auto operator*() const noexcept {
                const EntityId id = *cur;
                return std::tie(
                    *cur,
                    world->template GetPool<FirstC>().DirectAtEntityIndex(id.index),
                    world->template GetPool<RestCs>().DirectAtEntityIndex(id.index)...);
            }
        };

        [[nodiscard]] constexpr Iterator begin() const noexcept {
            Iterator it{world, driver.data(), driver.data() + driver.size(), reqMask, exclMask};
            it.AdvanceToValid();
            return it;
        }
        [[nodiscard]] constexpr Sentinel end() const noexcept { return {}; }

        WorldPtr world{nullptr};
        std::span<const EntityId> driver{};
        u64 reqMask{0};
        u64 exclMask{0};
    };

    template <Component FirstC, Component... RestCs, Component... ExclCs>
    [[nodiscard]] constexpr auto View(Exclude<ExclCs...> excl = {}) noexcept {
        return MakeView<FirstC, RestCs...>(*this, excl);
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs>
    [[nodiscard]] constexpr auto View(Exclude<ExclCs...> excl = {}) const noexcept {
        return MakeView<FirstC, RestCs...>(*this, excl);
    }

    // Reserves an EntityId slot immediately (with 0 components attached) so DeferEmplace / DeferSetParent can target it in the same tick.
    [[nodiscard]] Result<EntityId, WorldErrc> DeferSpawn(const EntitySpawnDesc& desc = {}) noexcept {
        return Spawn(desc);
    }

    [[nodiscard]] Result<void, WorldErrc> DeferDestroy(
        EntityId id,
        bool destroyChildren = true) noexcept;

    [[nodiscard]] Result<void, WorldErrc> DeferSetParent(
        EntityId child,
        EntityId parent,
        bool keepWorldTransform = false) noexcept;

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferEmplace(EntityId id, const T& comp) noexcept {
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if ((m_registeredPoolsMask & (1ULL << tid)) == 0ULL) {
            if (auto r = RegisterComponentPool<T>(); !r) return Err(r.error());
        }
        return EnqueueRawCommand(CmdKind::Emplace, id, kNullEntity, tid, &comp, sizeof(T));
    }

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferRemove(EntityId id) noexcept {
        return EnqueueRawCommand(
            CmdKind::Remove, id, kNullEntity, static_cast<u8>(T::kTypeId), nullptr, 0U);
    }

    [[nodiscard]] Result<void, WorldErrc> FlushCommands() noexcept;

    [[nodiscard]] LocalTransform2D GetLocalTransform(EntityId id) const noexcept;
    [[nodiscard]] LocalTransform2D& ModifyLocalTransformByIndex(u16 idx) noexcept {
        m_transforms.MarkSubtreeDirty(idx, m_maxEntities);
        return m_transforms.Local(idx);
    }
    void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept;
    void Teleport(EntityId id, const LocalTransform2D& local) noexcept;

    [[nodiscard]] WorldTransform2D GetWorldTransform(EntityId id) const noexcept;
    [[nodiscard]] const WorldTransform2D& GetWorldByIndex(u16 idx) const noexcept {
        return m_transforms.World(idx);
    }
    [[nodiscard]] const PrevWorldSnapshot2D& GetPrevWorldByIndex(u16 idx) const noexcept {
        return m_transforms.Prev(idx);
    }
    [[nodiscard]] constexpr u8 GetTimeChannelByIndex(u16 idx) const noexcept {
        return m_timeChannels[idx];
    }
    constexpr void SetTimeChannel(EntityId id, TimeChannel ch) noexcept {
        if (IsAlive(id)) m_timeChannels[id.index] = static_cast<u8>(ch);
    }
    [[nodiscard]] constexpr u64 GetComponentMaskByIndex(u16 idx) const noexcept {
        return m_componentMasks[idx];
    }
    [[nodiscard]] constexpr EntityId EntityAtSlot(u16 idx) const noexcept {
        return EntityId{idx, m_generations[idx]};
    }

    [[nodiscard]] Result<void, WorldErrc> SetParent(
        EntityId child,
        EntityId parent,
        bool keepWorldTransform = false) noexcept;

    [[nodiscard]] EntityId GetParent(EntityId child) const noexcept;
    void PropagateTransforms() noexcept {
        m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
    }

    [[nodiscard]] std::span<const WorldTransform2D> WorldTransforms() const noexcept {
        return m_transforms.WorldSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }
    [[nodiscard]] std::span<const PrevWorldSnapshot2D> PrevWorldTransforms() const noexcept {
        return m_transforms.PrevSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }
    [[nodiscard]] constexpr std::span<const u8> TimeChannels() const noexcept {
        return {m_timeChannels, static_cast<usize>(m_maxAllocatedIndex + 1U)};
    }
    [[nodiscard]] std::span<const u64> MovedWords() const noexcept {
        return m_transforms.MovedWords(m_aliveWordCount);
    }
    [[nodiscard]] constexpr std::span<const u64> AliveWords() const noexcept {
        return {m_aliveWords, m_aliveWordCount};
    }
    [[nodiscard]] constexpr std::span<const u64> TeleportedWords() const noexcept {
        return {m_teleportedWords, m_aliveWordCount};
    }

    void BeginTick(const SimStepView& step) noexcept;
    void StepAmbientVelocities(const SimStepView& step) noexcept;
    void StepSpriteAnimationsOnly(const SimStepView& step) noexcept;
    void StepLifetimes(const SimStepView& step) noexcept;
    void StepPointLights(const SimStepView& step) noexcept;
    void StepAnimations(const SimStepView& step) noexcept;
    void StepAnimationsAndLifetimes(const SimStepView& step) noexcept;
    void EndTick(const SimStepView& step = {}) noexcept;

    [[nodiscard]] Result<EntityId, WorldErrc> InstantiatePrefab(
        const AssetBank& bank,
        BlobHandle prefabBlob,
        const LocalTransform2D& rootTransform = {},
        std::span<EntityId> outSpawnedEntities = {}) noexcept;

    [[nodiscard]] Result<usize, WorldErrc> SerializePrefabTemplate(
        EntityId rootEntity,
        std::span<u8> outBuffer) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    [[nodiscard]] bool EmplaceRawByTypeId(EntityId id, u8 typeId, const void* rawData) noexcept;

private:
    enum class CmdKind : u8 {
        Destroy = 0,
        Emplace = 1,
        Remove = 2,
        SetParent = 3,
    };

    void DestroySingleSlot(EntityId id) noexcept;

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename WorldT, typename Fn>
    static void DispatchEach(WorldT& self, Fn&& fn, Exclude<ExclCs...>) noexcept {
        constexpr u64 reqMask = MakeComponentMask<FirstC, RestCs...>();
        constexpr u64 exclMask = MakeComponentMask<ExclCs...>();
        if ((self.m_registeredPoolsMask & reqMask) != reqMask) return;

        std::span<const EntityId> driverEntities = self.template GetPool<FirstC>().Entities();
        if constexpr (sizeof...(RestCs) > 0U) {
            const auto pickSmallest = [&]<Component C>() noexcept {
                const auto span = self.template GetPool<C>().Entities();
                if (span.size() < driverEntities.size()) driverEntities = span;
            };
            (pickSmallest.template operator()<RestCs>(), ...);
        }

        for (const EntityId id : driverEntities) {
            const u64 mask = self.m_componentMasks[id.index];
            if ((mask & reqMask) != reqMask || (mask & exclMask) != 0ULL) continue;
            fn(id,
               self.template GetPool<FirstC>().DirectAtEntityIndex(id.index),
               self.template GetPool<RestCs>().DirectAtEntityIndex(id.index)...);
        }
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename WorldT>
    [[nodiscard]] static constexpr auto MakeView(WorldT& self, Exclude<ExclCs...>) noexcept {
        constexpr u64 reqMask = MakeComponentMask<FirstC, RestCs...>();
        constexpr u64 exclMask = MakeComponentMask<ExclCs...>();
        if ((self.m_registeredPoolsMask & reqMask) != reqMask) {
            return ViewRange<WorldT*, FirstC, RestCs...>{&self, {}, reqMask, exclMask};
        }
        std::span<const EntityId> driverEntities = self.template GetPool<FirstC>().Entities();
        if constexpr (sizeof...(RestCs) > 0U) {
            const auto pickSmallest = [&]<Component C>() noexcept {
                const auto span = self.template GetPool<C>().Entities();
                if (span.size() < driverEntities.size()) driverEntities = span;
            };
            (pickSmallest.template operator()<RestCs>(), ...);
        }
        return ViewRange<WorldT*, FirstC, RestCs...>{&self, driverEntities, reqMask, exclMask};
    }

    [[nodiscard]] Result<void, WorldErrc> EnqueueRawCommand(
        CmdKind kind,
        EntityId id,
        EntityId auxId,
        u8 typeId,
        const void* payload,
        u16 payloadBytes) noexcept;

    // Cache Line 0 (offset 0..63, 64B exact, 0B padding): Hot 120 Hz / 240 Hz pointers & counters
    u16* m_generations{nullptr};
    u64* m_componentMasks{nullptr};
    u8* m_timeChannels{nullptr};
    u64* m_aliveWords{nullptr};
    RawComponentPool* m_pools{nullptr};
    u8* m_cmdTape{nullptr};
    u32 m_aliveCount{0};
    u16 m_maxEntities{0};
    u16 m_maxAllocatedIndex{0};
    u16 m_nextFreshSlot{1};
    u16 m_freeHead{kInvalidDenseIndex};
    u16 m_freeTail{kInvalidDenseIndex};
    u16 m_defaultPoolCap{0};

    // Cache Line 1 (offset 64..127, 64B exact, 0B padding): Non-recursive LCRS TransformSolver
    TransformSolver m_transforms{};

    // Cache Line 2 (offset 128..191, 64B exact, 0B padding): Arena, pool mask, command tape & telemetry
    AlignedArena m_arena{};
    u64 m_registeredPoolsMask{0};
    u64* m_teleportedWords{nullptr};
    u32 m_cmdCapacity{0};
    u32 m_cmdWriteOffset{0};
    u32 m_tickCount{0};
    u16 m_aliveWordCount{0};
    u16 m_pendingCmdCount{0};
    u64 m_spawnedTotalCount{0};
    u64 m_destroyedTotalCount{0};
};
static_assert(sizeof(World) == 192);

} // namespace en

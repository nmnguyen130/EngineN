#pragma once

#include "world/SparseSet.h"
#include "world/TransformSolver.h"

#include <tuple>

namespace en {

class AssetBank;

// Sparse-set ECS registry, LCRS transform hierarchy, and deferred command buffer.
// Move-disabled owner of an Arena; slot 0 is reserved as kNullEntity.
class World {
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
        if (!m_pools) return Err(WorldErrc::InvalidConfig);
        if ((m_registeredPoolsMask & (1ULL << tid)) != 0ULL) return {};
        const u16 cap = (capacity > 0U) ? capacity : m_defaultPoolCap;
        if (!static_cast<SparseSet<T>&>(m_pools[tid]).Init(m_arena, m_maxEntities, cap)) {
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

    [[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept {
        return id.index > 0U && id.index < m_maxEntities &&
               m_generations[id.index] == id.generation;
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
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if (!IsAlive(id) || (m_registeredPoolsMask & (1ULL << tid)) == 0ULL ||
            !GetPool<T>().Remove(id)) {
            return false;
        }
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
    [[nodiscard]] constexpr SparseSet<T>& GetPool() noexcept {
        return static_cast<SparseSet<T>&>(m_pools[static_cast<u8>(T::kTypeId)]);
    }

    template <Component T>
    [[nodiscard]] constexpr const SparseSet<T>& GetPool() const noexcept {
        return static_cast<const SparseSet<T>&>(m_pools[static_cast<u8>(T::kTypeId)]);
    }

    template <typename WorldPtr, Component FirstC, Component... RestCs>
    class ViewRange {
    public:
        struct Sentinel {};

        struct Iterator {
            WorldPtr        world{nullptr};
            const EntityId* cur{nullptr};
            const EntityId* end{nullptr};
            u64             reqMask{0};
            u64             exclMask{0};

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

            [[nodiscard]] constexpr bool operator==(Sentinel) const noexcept {
                return cur == end;
            }

            [[nodiscard]] constexpr auto operator*() const noexcept {
                const u16 idx = cur->index;
                return std::tie(
                    *cur,
                    world->template GetPool<FirstC>().DirectAtEntityIndex(idx),
                    world->template GetPool<RestCs>().DirectAtEntityIndex(idx)...);
            }
        };

        [[nodiscard]] constexpr Iterator begin() const noexcept {
            Iterator it{world, driver.data(), driver.data() + driver.size(), reqMask, exclMask};
            it.AdvanceToValid();
            return it;
        }
        [[nodiscard]] constexpr Sentinel end() const noexcept { return {}; }

        WorldPtr                  world{nullptr};
        std::span<const EntityId> driver{};
        u64                       reqMask{0};
        u64                       exclMask{0};
    };

    template <Component FirstC, Component... RestCs, Component... ExclCs>
    [[nodiscard]] constexpr auto View(Exclude<ExclCs...> excl = {}) noexcept {
        return MakeView<FirstC, RestCs...>(*this, excl);
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs>
    [[nodiscard]] constexpr auto View(Exclude<ExclCs...> excl = {}) const noexcept {
        return MakeView<FirstC, RestCs...>(*this, excl);
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> excl = {}) noexcept {
        for (auto&& tuple : MakeView<FirstC, RestCs...>(*this, excl)) {
            std::apply(fn, tuple);
        }
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> excl = {}) const noexcept {
        for (auto&& tuple : MakeView<FirstC, RestCs...>(*this, excl)) {
            std::apply(fn, tuple);
        }
    }

    [[nodiscard]] Result<void, WorldErrc> DeferDestroy(
        EntityId id,
        bool destroyChildren = true) noexcept {
        return EnqueueRawCommand(
            CmdKind::Destroy, id, kNullEntity, destroyChildren ? 1U : 0U, nullptr, 0U);
    }

    [[nodiscard]] Result<void, WorldErrc> DeferSetParent(
        EntityId child,
        EntityId parent,
        bool keepWorldTransform = false) noexcept {
        return EnqueueRawCommand(
            CmdKind::SetParent, child, parent, keepWorldTransform ? 1U : 0U, nullptr, 0U);
    }

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

    [[nodiscard]] constexpr LocalTransform2D GetLocalTransform(EntityId id) const noexcept {
        return IsAlive(id) ? m_transforms.Local(id.index) : LocalTransform2D{};
    }

    [[nodiscard]] LocalTransform2D& ModifyLocalTransformByIndex(u16 idx) noexcept {
        m_transforms.MarkSubtreeDirty(idx, m_maxEntities);
        return m_transforms.Local(idx);
    }

    void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept {
        if (IsAlive(id)) {
            m_transforms.Local(id.index) = local;
            m_transforms.MarkSubtreeDirty(id.index, m_maxEntities);
        }
    }

    void Teleport(EntityId id, const LocalTransform2D& local) noexcept {
        if (IsAlive(id)) {
            m_transforms.Local(id.index) = local;
            m_transforms.TeleportSubtree(id.index, m_maxEntities);
        }
    }

    [[nodiscard]] constexpr WorldTransform2D GetWorldTransform(EntityId id) const noexcept {
        return IsAlive(id) ? m_transforms.World(id.index) : WorldTransform2D{};
    }

    [[nodiscard]] constexpr const WorldTransform2D& GetWorldByIndex(u16 idx) const noexcept {
        return m_transforms.World(idx);
    }

    [[nodiscard]] constexpr const PrevWorldSnapshot2D& GetPrevWorldByIndex(u16 idx) const noexcept {
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
        return {idx, m_generations[idx]};
    }

    [[nodiscard]] Result<void, WorldErrc> SetParent(
        EntityId child,
        EntityId parent,
        bool keepWorldTransform = false) noexcept {
        if (!IsAlive(child) || (parent.IsValid() && !IsAlive(parent))) {
            return Err(WorldErrc::DeadEntity);
        }
        return m_transforms.SetParent(
            child.index,
            parent.IsValid() ? parent.index : kInvalidDenseIndex,
            m_maxEntities,
            keepWorldTransform);
    }

    [[nodiscard]] constexpr EntityId GetParent(EntityId id) const noexcept {
        if (!IsAlive(id)) return kNullEntity;
        const u16 pIdx = m_transforms.ParentOf(id.index);
        return (pIdx != kInvalidDenseIndex && pIdx < m_maxEntities)
                   ? EntityId{pIdx, m_generations[pIdx]}
                   : kNullEntity;
    }

    void PropagateTransforms() noexcept {
        m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
    }

    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldTransforms() const noexcept {
        return m_transforms.WorldSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }

    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevWorldTransforms() const noexcept {
        return m_transforms.PrevSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }

    [[nodiscard]] constexpr std::span<const u8> TimeChannels() const noexcept {
        return {m_timeChannels, static_cast<usize>(m_maxAllocatedIndex + 1U)};
    }

    [[nodiscard]] constexpr std::span<const u64> MovedWords() const noexcept {
        return m_transforms.MovedWords(m_aliveWordCount);
    }

    [[nodiscard]] constexpr std::span<const u64> AliveWords() const noexcept {
        return {m_aliveWords, m_aliveWordCount};
    }

    void BeginTick(const TickStep& step) noexcept {
        m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
        m_transforms.SnapshotPreviousTransforms(
            AliveWords(), m_timeChannels, step.frozenMask, m_maxAllocatedIndex);
        ++m_tickCount;
    }

    void EndTick(const TickStep& /*step*/ = {}) noexcept {
        (void)FlushCommands();
        m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
    }

    [[nodiscard]] Result<EntityId, WorldErrc> InstantiatePrefab(
        const AssetBank& bank,
        BlobHandle prefabBlob,
        const LocalTransform2D& rootTransform = {},
        std::span<EntityId> outSpawnedEntities = {}) noexcept;

    [[nodiscard]] Result<usize, WorldErrc> SerializePrefabTemplate(
        EntityId rootEntity,
        std::span<u8> outBuffer) const noexcept;

    [[nodiscard]] usize RequiredSnapshotBytes() const noexcept;

    [[nodiscard]] Result<usize, WorldErrc> SerializeWorldSnapshot(
        std::span<u8> outBuffer) const noexcept;

    [[nodiscard]] Result<void, WorldErrc> DeserializeWorldSnapshot(
        std::span<const u8> inBuffer) noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

    bool EmplaceRawByTypeId(
        EntityId id,
        u8 typeId,
        const void* rawData,
        u16 expectedElemBytes = 0U) noexcept;

private:
    enum class CmdKind : u8 {
        Destroy   = 0,
        Emplace   = 1,
        Remove    = 2,
        SetParent = 3,
    };

    [[nodiscard]] constexpr usize ActiveWordCount() const noexcept {
        return std::min<usize>(
            m_aliveWordCount, (static_cast<usize>(m_maxAllocatedIndex) + 64U) >> 6U);
    }

    template <typename Fn>
    constexpr void ForEachAliveSlot(Fn&& fn) const noexcept {
        const usize wordCount = ActiveWordCount();
        for (usize w = 0U; w < wordCount; ++w) {
            for (u64 alive = m_aliveWords[w]; alive != 0ULL; alive &= (alive - 1ULL)) {
                fn(static_cast<u16>((w << 6U) + std::countr_zero(alive)));
            }
        }
    }

    void PushFreeSlot(u16 idx, u16 nextGen) noexcept;
    void DestroySingleSlot(EntityId id) noexcept;

    [[nodiscard]] usize CollectSubtree(
        u16 rootIdx,
        bool includeChildren,
        std::span<u16> outNodes,
        std::span<i16> outParentMap) const noexcept;

    [[nodiscard]] bool WriteEntityComponents(class BinaryWriter& writer, u16 idx) const noexcept;

    template <Component FirstC, Component... RestCs>
    [[nodiscard]] constexpr std::span<const EntityId> SmallestDriverSpan() const noexcept {
        std::span<const EntityId> driver = GetPool<FirstC>().Entities();
        ((GetPool<RestCs>().Size() < driver.size()
              ? (driver = GetPool<RestCs>().Entities(), 0)
              : 0),
         ...);
        return driver;
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename WorldT>
    [[nodiscard]] static constexpr auto MakeView(WorldT& self, Exclude<ExclCs...>) noexcept {
        constexpr u64 reqMask = MakeComponentMask<FirstC, RestCs...>();
        constexpr u64 exclMask = MakeComponentMask<ExclCs...>();
        if ((self.m_registeredPoolsMask & reqMask) != reqMask) {
            return ViewRange<WorldT*, FirstC, RestCs...>{&self, {}, reqMask, exclMask};
        }
        return ViewRange<WorldT*, FirstC, RestCs...>{
            &self, self.template SmallestDriverSpan<FirstC, RestCs...>(), reqMask, exclMask};
    }

    [[nodiscard]] Result<void, WorldErrc> EnqueueRawCommand(
        CmdKind kind,
        EntityId id,
        EntityId auxId,
        u8 typeId,
        const void* payload,
        u16 payloadBytes) noexcept;

    u16*            m_generations{nullptr};
    u64*            m_componentMasks{nullptr};
    u8*             m_timeChannels{nullptr};
    u64*            m_aliveWords{nullptr};
    RawSparseSet*   m_pools{nullptr};
    u64             m_registeredPoolsMask{0};
    u32             m_aliveCount{0};
    u16             m_maxEntities{0};
    u16             m_maxAllocatedIndex{0};
    u16             m_nextFreshSlot{1};
    u16             m_freeHead{kInvalidDenseIndex};
    u16             m_freeTail{kInvalidDenseIndex};
    u16             m_defaultPoolCap{0};

    TransformSolver m_transforms{};

    Arena           m_arena{};
    u8*             m_cmdTape{nullptr};
    u32             m_cmdCapacity{0};
    u32             m_cmdWriteOffset{0};
    u32             m_tickCount{0};
    u16             m_aliveWordCount{0};
    u16             m_pendingCmdCount{0};
    u64             m_spawnedTotalCount{0};
    u64             m_destroyedTotalCount{0};
};
static_assert(sizeof(World) == 184);

} // namespace en

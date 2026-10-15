#pragma once

#include "world/WorldTypes.h"
#include "world/ComponentPool.h"
#include "world/TransformSolver.h"
#include "core/time/TimeViews.h"

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <tuple>

namespace en::assets { class AssetBank; }

namespace en::world {

enum class WorldCmdOp : u8 {
    DestroyEntity = 0,
    SetParent,
    SetTimeChannel,
    EmplaceComponent,
    RemoveComponent,
};

struct alignas(8) WorldCmdHeader {
    WorldCmdOp op{WorldCmdOp::DestroyEntity};
    u8 componentTypeId{0};
    u16 payloadBytes{0};
    EntityId target{kNullEntity};
};
static_assert(sizeof(WorldCmdHeader) == 8);

// Bitmask-accelerated multi-component view selecting the smallest dense pool as lead iterator.
template <typename WorldPtr, Component... IncludeTs, Component... ExcludeTs>
class WorldView final {
    static_assert(sizeof...(IncludeTs) >= 1, "WorldView requires at least one Include component.");
    static constexpr ComponentMask kIncludeMask = MakeComponentMask<IncludeTs...>();
    static constexpr ComponentMask kExcludeMask = MakeComponentMask<ExcludeTs...>();

    using PoolTuple = std::tuple<decltype(&std::declval<WorldPtr>()->template GetPool<IncludeTs>())...>;

public:
    class Iterator final {
    public:
        constexpr Iterator(
            WorldPtr world,
            PoolTuple pools,
            std::span<const EntityId> leadEntities,
            usize idx) noexcept
            : m_world(world), m_pools(pools), m_lead(leadEntities), m_idx(idx) {
            AdvanceToValid();
        }

        constexpr Iterator& operator++() noexcept {
            ++m_idx;
            AdvanceToValid();
            return *this;
        }

        [[nodiscard]] constexpr bool operator==(const Iterator& other) const noexcept {
            return m_idx == other.m_idx;
        }

        [[nodiscard]] constexpr auto operator*() const noexcept {
            const EntityId id = m_lead[m_idx];
            return std::tuple<EntityId, decltype(std::get<IncludeTs*>(m_pools)->DirectAtEntityIndex(id.index))...>(
                id,
                std::get<decltype(&m_world->template GetPool<IncludeTs>())>(m_pools)->DirectAtEntityIndex(id.index)...
            );
        }

    private:
        constexpr void AdvanceToValid() noexcept {
            while (m_idx < m_lead.size()) {
                const u16 entityIdx = m_lead[m_idx].index;
                const ComponentMask mask = m_world->GetComponentMaskByIndex(entityIdx);
                if (((mask & kIncludeMask) == kIncludeMask) && ((mask & kExcludeMask) == 0)) [[likely]] {
                    break;
                }
                ++m_idx;
            }
        }

        WorldPtr m_world;
        PoolTuple m_pools;
        std::span<const EntityId> m_lead;
        usize m_idx{0};
    };

    constexpr WorldView(WorldPtr world, PoolTuple pools, std::span<const EntityId> lead) noexcept
        : m_world(world), m_pools(pools), m_lead(lead) {}

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator(m_world, m_pools, m_lead, 0); }
    [[nodiscard]] constexpr Iterator end() const noexcept { return Iterator(m_world, m_pools, m_lead, m_lead.size()); }

private:
    WorldPtr m_world;
    PoolTuple m_pools;
    std::span<const EntityId> m_lead;
};

// Authoritative simulation container owning entity lifetimes, SoA spatial spine, and component pools.
// Move-only RAII owner of a single pre-allocated memory slab; zero heap allocations in hot paths.
// Invariant: Slot 0 is permanently reserved as the null/root identity sentinel.
class World final {
public:
    World() = default;
    ~World() noexcept { Shutdown(); }

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;

    [[nodiscard]] Result<void, WorldErrc> Init(const WorldConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;
    void Clear() noexcept;

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> RegisterComponentPool(u16 capacity = 0) noexcept {
        constexpr u8 tid = T::kTypeId;
        if (m_poolOps[tid].poolInstance != nullptr) {
            return Ok;
        }
        const u16 cap = (capacity == 0) ? m_cfg.defaultComponentPoolCapacity : capacity;
        const u32 wordCount = static_cast<u32>(m_cfg.maxEntities) >> 6;

        auto* poolMem      = CarveAligned<ComponentPool<T>>(1);
        auto* sparseMem    = CarveAligned<u16>(m_cfg.maxEntities);
        auto* presenceMem  = CarveAligned<u64>(wordCount);
        auto* denseEntMem  = CarveAligned<EntityId>(cap);
        auto* denseCompMem = CarveAligned<T>(cap, std::max<usize>(alignof(T), 64));

        if (!poolMem || !sparseMem || !presenceMem || !denseEntMem || !denseCompMem) [[unlikely]] {
            return Err(WorldErrc::OutOfMemory);
        }

        auto* pool = std::construct_at(poolMem);
        pool->BindArena(
            {sparseMem, m_cfg.maxEntities},
            {presenceMem, wordCount},
            {denseEntMem, cap},
            {denseCompMem, cap}
        );

        m_poolOps[tid] = PoolOps{
            .poolInstance = pool,
            .removeUncheckedFn = [](void* p, EntityId id) noexcept {
                static_cast<ComponentPool<T>*>(p)->Remove(id);
            },
            .emplaceRawFn = [](void* p, EntityId id, const void* raw) noexcept -> WorldErrc {
                auto res = static_cast<ComponentPool<T>*>(p)->Emplace(id, *static_cast<const T*>(raw));
                return res ? WorldErrc::InvalidConfig : res.error();
            },
            .clearFn = [](void* p) noexcept {
                static_cast<ComponentPool<T>*>(p)->Clear();
            },
            .hashCanonicalFn = [](const void* p, u16 maxIdx) noexcept -> u64 {
                const auto* cp = static_cast<const ComponentPool<T>*>(p);
                u64 h = 14695981039346656037ULL ^ T::kTypeId;
                const auto comps = cp->Components();
                for (u16 idx = 1; idx <= maxIdx; ++idx) {
                    const u16 dense = cp->DenseIndexOf(idx);
                    if (dense < cp->Size()) {
                        const auto* bytes = reinterpret_cast<const u8*>(&comps[dense]);
                        for (usize b = 0; b < sizeof(T); ++b) {
                            h = (h ^ bytes[b]) * 1099511628211ULL;
                        }
                    }
                }
                return h;
            }
        };
        return {};
    }

    [[nodiscard]] Result<EntityId, WorldErrc> Spawn(const EntitySpawnDesc& desc = {}) noexcept;
    bool Destroy(EntityId id) noexcept;

    [[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept {
        return id.index > 0 && id.index < m_cfg.maxEntities &&
               m_generations[id.index] == id.generation &&
               HasFlag(m_flags[id.index], EntityFlags::Alive);
    }

    template <Component T, typename... Args>
    [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (!IsAlive(id)) [[unlikely]] {
            return Err(WorldErrc::InvalidEntity);
        }
        constexpr u8 tid = T::kTypeId;
        if (m_poolOps[tid].poolInstance == nullptr) [[unlikely]] {
            return Err(WorldErrc::ComponentNotRegistered);
        }
        auto& pool = GetPool<T>();
        auto res = pool.Emplace(id, std::forward<Args>(args)...);
        if (!res) [[unlikely]] {
            return res;
        }
        const ComponentMask prevMask = m_componentMasks[id.index];
        const ComponentMask newMask  = prevMask | (ComponentMask{1} << tid);
        m_componentMasks[id.index]   = newMask;

        if constexpr (tid == Sprite2D::kTypeId || tid == SpriteAnimator2D::kTypeId) {
            constexpr ComponentMask kAnimGroupMask = MakeComponentMask<Sprite2D, SpriteAnimator2D>();
            if ((prevMask & kAnimGroupMask) != kAnimGroupMask && (newMask & kAnimGroupMask) == kAnimGroupMask) {
                SyncLockstepAnimationGroupOnAdd(id);
            }
        }
        return res;
    }

    template <Component T>
    bool Remove(EntityId id) noexcept {
        if (!IsAlive(id)) [[unlikely]] {
            return false;
        }
        constexpr u8 tid = T::kTypeId;
        const ComponentMask bit = ComponentMask{1} << tid;
        if ((m_componentMasks[id.index] & bit) == 0) {
            return false;
        }
        if constexpr (tid == Sprite2D::kTypeId || tid == SpriteAnimator2D::kTypeId) {
            constexpr ComponentMask kAnimGroupMask = MakeComponentMask<Sprite2D, SpriteAnimator2D>();
            if ((m_componentMasks[id.index] & kAnimGroupMask) == kAnimGroupMask) {
                SyncLockstepAnimationGroupOnRemove(id);
            }
        }
        m_componentMasks[id.index] &= ~bit;
        return GetPool<T>().Remove(id);
    }

    template <Component T, typename Self>
    [[nodiscard]] constexpr auto* TryGet(this Self&& self, EntityId id) noexcept {
        if (!self.IsAlive(id)) [[unlikely]] {
            return static_cast<decltype(self.template GetPool<T>().TryGet(id))>(nullptr);
        }
        return self.template GetPool<T>().TryGet(id);
    }

    template <Component T>
    [[nodiscard]] constexpr bool Has(EntityId id) const noexcept {
        return IsAlive(id) && ((m_componentMasks[id.index] & (ComponentMask{1} << T::kTypeId)) != 0);
    }

    template <Component T, typename Self>
    [[nodiscard]] constexpr auto& GetPool(this Self&& self) noexcept {
        using PoolPtr = std::conditional_t<
            std::is_const_v<std::remove_reference_t<Self>>,
            const ComponentPool<T>*,
            ComponentPool<T>*>;
        return *static_cast<PoolPtr>(self.m_poolOps[T::kTypeId].poolInstance);
    }

    // Deferred Command Tape API
    [[nodiscard]] Result<void, WorldErrc> DeferDestroy(EntityId id) noexcept;
    [[nodiscard]] Result<void, WorldErrc> DeferSetParent(EntityId child, EntityId parent) noexcept;

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferEmplace(EntityId id, const T& comp) noexcept {
        return PushRawCommand(WorldCmdOp::EmplaceComponent, T::kTypeId, id, &comp, sizeof(T));
    }

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferRemove(EntityId id) noexcept {
        return PushRawCommand(WorldCmdOp::RemoveComponent, T::kTypeId, id, nullptr, 0);
    }

    [[nodiscard]] Result<void, WorldErrc> FlushCommands() noexcept;

    // Direct O(1) Spatial & Metadata Accessors
    [[nodiscard]] constexpr const LocalTransform2D& GetLocalTransform(EntityId id) const noexcept {
        return m_transforms.GetLocal(id.index);
    }
    void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept {
        if (IsAlive(id)) {
            m_transforms.SetLocal(id.index, local);
        }
    }
    void Teleport(EntityId id, const LocalTransform2D& local) noexcept;

    [[nodiscard]] constexpr const WorldTransform2D& GetWorldTransform(EntityId id) const noexcept {
        return m_transforms.GetWorld(id.index);
    }
    [[nodiscard]] constexpr const WorldTransform2D& GetWorldByIndex(u16 idx) const noexcept {
        return m_transforms.GetWorld(idx);
    }
    [[nodiscard]] constexpr const PrevWorldSnapshot2D& GetPrevWorldByIndex(u16 idx) const noexcept {
        return m_transforms.GetPrevWorld(idx);
    }
    [[nodiscard]] constexpr time::TimeChannel GetTimeChannelByIndex(u16 idx) const noexcept {
        return m_timeChannels[idx];
    }
    [[nodiscard]] constexpr ComponentMask GetComponentMaskByIndex(u16 idx) const noexcept {
        return m_componentMasks[idx];
    }
    [[nodiscard]] Result<void, WorldErrc> SetParent(EntityId child, EntityId parent) noexcept;
    void PropagateTransforms() noexcept { m_transforms.Propagate(m_maxAllocatedIndex); }

    template <Component... IncludeTs, Component... ExcludeTs, typename Self>
    [[nodiscard]] constexpr auto View(this Self&& self, Exclude<ExcludeTs...> = {}) noexcept {
        auto pools = std::make_tuple(&self.template GetPool<IncludeTs>()...);
        std::span<const EntityId> smallestLead{};
        usize minSize = ~usize{0};
        auto pickSmallest = [&](const auto* poolPtr) noexcept {
            if (poolPtr->Size() < minSize) {
                minSize = poolPtr->Size();
                smallestLead = poolPtr->Entities();
            }
        };
        (pickSmallest(std::get<decltype(&self.template GetPool<IncludeTs>())>(pools)), ...);
        return WorldView<decltype(&self), IncludeTs..., ExcludeTs...>(&self, pools, smallestLead);
    }

    // 120 Hz Simulation Tick Pipeline
    void BeginTick(time::SimStepView step) noexcept;
    void StepKinematicsAndAnimations(time::SimStepView step, const assets::AssetBank& assetBank) noexcept;
    void EndTick(time::SimStepView step) noexcept;
    void Tick(time::SimStepView step, const assets::AssetBank& assetBank) noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    template <typename T>
    [[nodiscard]] T* CarveAligned(usize count, usize alignment = alignof(T)) noexcept {
        const usize mask = alignment - 1u;
        const usize alignedOffset = (m_arenaOffset + mask) & ~mask;
        const usize bytes = count * sizeof(T);
        if (alignedOffset + bytes > m_arenaCapacity) [[unlikely]] {
            return nullptr;
        }
        m_arenaOffset = alignedOffset + bytes;
        return reinterpret_cast<T*>(m_arenaSlab.get() + alignedOffset);
    }

    [[nodiscard]] Result<void, WorldErrc> PushRawCommand(
        WorldCmdOp op, u8 componentTypeId, EntityId target, const void* payload, u16 payloadBytes) noexcept;

    void SyncLockstepAnimationGroupOnAdd(EntityId id) noexcept;
    void SyncLockstepAnimationGroupOnRemove(EntityId id) noexcept;

    WorldConfig m_cfg{};
    std::unique_ptr<u8[]> m_arenaSlab{};
    usize m_arenaCapacity{0};
    usize m_arenaOffset{0};

    // Split SoA Entity Metadata Planes
    std::span<u16> m_generations{};
    std::span<EntityFlags> m_flags{};
    std::span<time::TimeChannel> m_timeChannels{};
    std::span<ComponentMask> m_componentMasks{};
    std::span<u64> m_aliveWords{};
    std::span<u64> m_teleportedWords{};

    // FIFO Recycled Slot Ring Buffer
    std::span<u16> m_freeRing{};
    u32 m_freeHead{0};
    u32 m_freeTail{0};
    u32 m_freeCount{0};
    u16 m_nextFreshSlot{1};
    u16 m_maxAllocatedIndex{0};
    u32 m_aliveCount{0};

    // Deferred Command Tape
    std::span<u8> m_cmdTape{};
    u32 m_cmdWriteOffset{0};

    TransformSolver m_transforms{};
    std::array<PoolOps, kMaxComponentTypes> m_poolOps{};
};

} // namespace en::world

#pragma once

#include "world/WorldTypes.h"
#include "world/ComponentPool.h"
#include "world/TransformSolver.h"
#include "core/time/TimePipeline.h"

#include <cstring>
#include <memory>
#include <span>
#include <tuple>

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

struct alignas(64) AlignedArenaLine {
    u8 bytes[64];
};
static_assert(sizeof(AlignedArenaLine) == 64 && alignof(AlignedArenaLine) == 64);

// Bitmask-accelerated multi-component view selecting the smallest dense pool as lead iterator.
// Single-component unfiltered views bypass mask and sparse lookups at compile time.
template <typename WorldPtr, typename ExcludeTag, Component... IncludeTs>
class WorldView;

template <typename WorldPtr, Component... ExcludeTs, Component... IncludeTs>
class WorldView<WorldPtr, Exclude<ExcludeTs...>, IncludeTs...> final {
    static_assert(sizeof...(IncludeTs) >= 1, "WorldView requires at least one Include component.");
    static constexpr ComponentMask kIncludeMask = MakeComponentMask<IncludeTs...>();
    static constexpr ComponentMask kExcludeMask = MakeComponentMask<ExcludeTs...>();
    static constexpr bool kSingleUnfiltered = (sizeof...(IncludeTs) == 1) && (sizeof...(ExcludeTs) == 0);

    using PoolTuple = std::tuple<decltype(&std::declval<WorldPtr>()->template GetPool<IncludeTs>())...>;

public:
    class Iterator final {
    public:
        constexpr Iterator(
            const ComponentMask* masks,
            PoolTuple pools,
            std::span<const EntityId> leadEntities,
            usize idx) noexcept
            : m_masks(masks), m_pools(pools), m_lead(leadEntities), m_idx(idx) {
            if constexpr (!kSingleUnfiltered) {
                AdvanceToValid();
            }
        }

        constexpr Iterator& operator++() noexcept {
            ++m_idx;
            if constexpr (!kSingleUnfiltered) {
                AdvanceToValid();
            }
            return *this;
        }

        [[nodiscard]] constexpr bool operator==(const Iterator& other) const noexcept {
            return m_idx == other.m_idx;
        }

        [[nodiscard]] constexpr auto operator*() const noexcept {
            const EntityId id = m_lead[m_idx];
            if constexpr (kSingleUnfiltered) {
                return std::tuple<EntityId, decltype(std::get<0>(m_pools)->ComponentAtDense(m_idx))>(
                    id, std::get<0>(m_pools)->ComponentAtDense(m_idx)
                );
            } else {
                return std::apply(
                    [id](auto*... poolPtrs) noexcept {
                        return std::tuple<EntityId, decltype(poolPtrs->DirectAtEntityIndex(id.index))...>(
                            id, poolPtrs->DirectAtEntityIndex(id.index)...
                        );
                    },
                    m_pools
                );
            }
        }

    private:
        constexpr void AdvanceToValid() noexcept {
            while (m_idx < m_lead.size()) {
                const ComponentMask mask = m_masks[m_lead[m_idx].index];
                if (((mask & kIncludeMask) == kIncludeMask) && ((mask & kExcludeMask) == 0)) [[likely]] {
                    break;
                }
                ++m_idx;
            }
        }

        const ComponentMask* m_masks{nullptr};
        PoolTuple m_pools;
        std::span<const EntityId> m_lead;
        usize m_idx{0};
    };

    constexpr WorldView(const ComponentMask* masks, PoolTuple pools, std::span<const EntityId> lead) noexcept
        : m_masks(masks), m_pools(pools), m_lead(lead) {}

    [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator(m_masks, m_pools, m_lead, 0); }
    [[nodiscard]] constexpr Iterator end() const noexcept { return Iterator(m_masks, m_pools, m_lead, m_lead.size()); }

private:
    const ComponentMask* m_masks{nullptr};
    PoolTuple m_pools;
    std::span<const EntityId> m_lead;
};

// Authoritative simulation container owning entity lifetimes, SoA spatial spine, and component pools.
// Move-only 192B (3-cache-line) RAII owner of a single 64B-aligned memory slab; zero heap allocations in hot paths.
// Invariant: Slot 0 is reserved as kNullEntity (m_generations[0] == 0); dead slots thread an intrusive FIFO freelist in m_componentMasks.
class alignas(64) World final {
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
            return {};
        }
        const u16 cap = (capacity == 0) ? m_defaultPoolCap : capacity;

        auto* poolMem      = CarveAligned<ComponentPool<T>>(1, alignof(ComponentPool<T>));
        auto* sparseMem    = CarveAligned<u16>(m_maxEntities, 64);
        auto* denseEntMem  = CarveAligned<EntityId>(cap, 64);
        auto* denseCompMem = CarveAligned<T>(cap, std::max<usize>(alignof(T), 64));

        if (!poolMem || !sparseMem || !denseEntMem || !denseCompMem) [[unlikely]] {
            return Err(WorldErrc::OutOfMemory);
        }

        auto* pool = std::construct_at(poolMem);
        pool->BindArena(
            {sparseMem, m_maxEntities},
            {denseEntMem, cap},
            {denseCompMem, cap}
        );

        m_poolOps[tid] = PoolOps{
            .poolInstance = pool,
            .removeOrClearFn = [](void* p, EntityId id) noexcept {
                auto* cp = static_cast<ComponentPool<T>*>(p);
                if (id.index != 0) [[likely]] {
                    cp->RemoveUnchecked(id);
                } else {
                    cp->Clear();
                }
            },
            .emplaceRawFn = [](void* p, EntityId id, const void* raw) noexcept -> Result<void, WorldErrc> {
                return static_cast<ComponentPool<T>*>(p)->EmplaceRaw(id, raw);
            },
            .hashCanonicalFn = [](const void* p, u16 maxIdx) noexcept -> u64 {
                const auto* cp = static_cast<const ComponentPool<T>*>(p);
                if (cp->Size() == 0) {
                    return 0ULL;
                }
                u64 h = 14695981039346656037ULL ^ T::kTypeId;
                const auto comps = cp->Components();
                for (u16 idx = 1; idx <= maxIdx; ++idx) {
                    const u16 dense = cp->DenseIndexOf(idx);
                    if (dense < cp->Size()) {
                        h = (h ^ idx) * 1099511628211ULL;
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
        return id.index < m_maxEntities &&
               m_generations[id.index] == id.generation &&
               id.generation != 0;
    }

    [[nodiscard]] constexpr u32 AliveCount() const noexcept { return m_aliveCount; }
    [[nodiscard]] constexpr u16 MaxAllocatedIndex() const noexcept { return m_maxAllocatedIndex; }

    template <Component T, typename... Args>
    [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (!IsAlive(id)) [[unlikely]] {
            return Err(WorldErrc::InvalidEntity);
        }
        constexpr u8 tid = T::kTypeId;
        if (m_poolOps[tid].poolInstance == nullptr) [[unlikely]] {
            return Err(WorldErrc::ComponentNotRegistered);
        }
        auto res = GetPool<T>().Emplace(id, std::forward<Args>(args)...);
        if (res) [[likely]] {
            m_componentMasks[id.index] |= (ComponentMask{1} << tid);
        }
        return res;
    }

    template <Component T>
    bool Remove(EntityId id) noexcept {
        if (!IsAlive(id)) [[unlikely]] {
            return false;
        }
        constexpr ComponentMask bit = ComponentMask{1} << T::kTypeId;
        if ((m_componentMasks[id.index] & bit) == 0) {
            return false;
        }
        m_componentMasks[id.index] &= ~bit;
        GetPool<T>().RemoveUnchecked(id);
        return true;
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

    [[nodiscard]] constexpr const LocalTransform2D& GetLocalTransform(EntityId id) const noexcept {
        return m_transforms.GetLocal(id.index);
    }
    [[nodiscard]] constexpr LocalTransform2D& ModifyLocalTransformByIndex(u16 idx) noexcept {
        return m_transforms.ModifyLocal(idx);
    }
    constexpr void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept {
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
    [[nodiscard]] constexpr std::span<const WorldTransform2D> WorldTransforms() const noexcept {
        return m_transforms.WorldSpan();
    }
    [[nodiscard]] constexpr std::span<const PrevWorldSnapshot2D> PrevWorldTransforms() const noexcept {
        return m_transforms.PrevWorldSpan();
    }
    [[nodiscard]] constexpr std::span<const time::TimeChannel> TimeChannels() const noexcept {
        return {m_timeChannels, m_maxEntities};
    }
    [[nodiscard]] constexpr std::span<const u64> MovedWords() const noexcept {
        return m_transforms.MovedWordsSpan();
    }
    [[nodiscard]] constexpr ComponentMask GetComponentMaskByIndex(u16 idx) const noexcept {
        return (m_generations[idx] != 0) ? m_componentMasks[idx] : 0ULL;
    }
    [[nodiscard]] Result<void, WorldErrc> SetParent(EntityId child, EntityId parent) noexcept;
    void PropagateTransforms() noexcept;

    template <Component... IncludeTs, Component... ExcludeTs, typename Self>
    [[nodiscard]] constexpr auto View(this Self&& self, Exclude<ExcludeTs...> = {}) noexcept {
        static_assert(sizeof...(IncludeTs) >= 1, "View requires at least one Include component.");
        auto pools = std::make_tuple(&self.template GetPool<IncludeTs>()...);
        std::span<const EntityId> smallestLead{};
        usize minSize = ~usize{0};

        std::apply(
            [&](const auto*... poolPtrs) noexcept {
                auto pickSmallest = [&](const auto* poolPtr) noexcept {
                    if (poolPtr->Size() < minSize) {
                        minSize = poolPtr->Size();
                        smallestLead = poolPtr->Entities();
                    }
                };
                (pickSmallest(poolPtrs), ...);
            },
            pools
        );

        return WorldView<decltype(&self), Exclude<ExcludeTs...>, IncludeTs...>(
            self.m_componentMasks, pools, smallestLead
        );
    }

    // Pure Layer 1 120 Hz Spatial, Ambient Velocity & Atlas Animation Lifecycle
    void BeginTick(time::SimStepView step) noexcept;
    void StepAmbientVelocities(time::SimStepView step) noexcept;
    void StepAnimations(time::SimStepView step) noexcept;
    void EndTick(time::SimStepView step = {}) noexcept;

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
        auto* rawBase = reinterpret_cast<u8*>(m_arenaSlab.get());
        return reinterpret_cast<T*>(rawBase + alignedOffset);
    }

    [[nodiscard]] Result<void, WorldErrc> PushRawCommand(
        WorldCmdOp op, u8 componentTypeId, EntityId target, const void* payload, u16 payloadBytes) noexcept;

    void SyncTeleportedSnapshots() noexcept;

    // Cache Line 0 (offset 0..63): Hot 120 Hz / 240 Hz entity & component state
    u16* m_generations{nullptr};
    ComponentMask* m_componentMasks{nullptr};
    time::TimeChannel* m_timeChannels{nullptr};
    u64* m_aliveWords{nullptr};
    u64* m_teleportedWords{nullptr};
    PoolOps* m_poolOps{nullptr};
    u16 m_maxEntities{0};
    u16 m_maxAllocatedIndex{0};
    u16 m_nextFreshSlot{1};
    u16 m_freeHead{0};
    u16 m_freeTail{0};
    u16 m_defaultPoolCap{0};
    u32 m_aliveCount{0};

    // Cache Line 1 (offset 64..127): 64B L1-aligned spatial & hierarchy solver
    TransformSolver m_transforms{};

    // Cache Line 2 (offset 128..191): Command tape & 64B-aligned arena slab ownership
    u8* m_cmdTape{nullptr};
    u32 m_cmdWriteOffset{0};
    u32 m_cmdCapacity{0};
    std::unique_ptr<AlignedArenaLine[]> m_arenaSlab{};
    usize m_arenaCapacity{0};
    usize m_arenaOffset{0};
    u8 m_reserved[24]{};
};
static_assert(sizeof(World) == 192);

} // namespace en::world

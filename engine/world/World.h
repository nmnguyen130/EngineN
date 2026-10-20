#pragma once

#include "world/ComponentPool.h"
#include "world/TransformSolver.h"

namespace en {

class AssetBank;

// Authoritative 64-component Sparse-Set ECS simulation container.
// Move-only RAII owner of a single 64B-aligned arena slab; zero runtime heap allocations after Init.
// Invariant: EntityId index 0 is reserved for kNullEntity and structural deferred commands flush at EndTick.
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
    [[nodiscard]] Result<void, WorldErrc> RegisterComponentPool(u16 capacity = 0) noexcept {
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if (m_poolOps.empty()) return Err(WorldErrc::InvalidConfig);
        if (m_poolOps[tid].poolPtr != nullptr) return {};
        const u16 cap = (capacity > 0) ? capacity : m_defaultPoolCap;
        auto poolSpan = m_arena.Carve<ComponentPool<T>>(1);
        if (poolSpan.empty() || !poolSpan[0].Init(m_arena, m_maxEntities, cap)) {
            return Err(WorldErrc::OutOfMemory);
        }
        m_poolOps[tid] = PoolOps{
            &poolSpan[0],
            [](void* p, EntityId id) noexcept { static_cast<ComponentPool<T>*>(p)->RemoveUnchecked(id); },
            [](void* p, EntityId id, const void* raw) noexcept {
                return static_cast<ComponentPool<T>*>(p)->EmplaceRaw(id, raw);
            },
            [](void* p) noexcept { static_cast<ComponentPool<T>*>(p)->Clear(); },
        };
        return {};
    }

    [[nodiscard]] Result<EntityId, WorldErrc> Spawn(const EntitySpawnDesc& desc = {}) noexcept;
    bool Destroy(EntityId id) noexcept;
    [[nodiscard]] Result<EntityId, WorldErrc> CloneEntity(EntityId source, const LocalTransform2D& overrideLocal) noexcept;
    [[nodiscard]] EntityId FindByTag(StringId tagName) const noexcept;

    [[nodiscard]] constexpr bool IsAlive(EntityId id) const noexcept {
        return id.IsValid() && id.index < m_maxEntities && m_generations[id.index] == id.generation;
    }
    [[nodiscard]] constexpr u32 AliveCount() const noexcept { return m_aliveCount; }
    [[nodiscard]] constexpr u16 MaxAllocatedIndex() const noexcept { return m_maxAllocatedIndex; }

    template <Component T, typename... Args>
    [[nodiscard]] Result<T*, WorldErrc> Emplace(EntityId id, Args&&... args) noexcept {
        if (!IsAlive(id)) return Err(WorldErrc::DeadEntity);
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if (m_poolOps[tid].poolPtr == nullptr) {
            if (auto r = RegisterComponentPool<T>(); !r) return Err(r.error());
        }
        auto* pool = static_cast<ComponentPool<T>*>(m_poolOps[tid].poolPtr);
        auto res = pool->Emplace(id, std::forward<Args>(args)...);
        if (res) m_componentMasks[id.index] |= (1ULL << tid);
        return res;
    }

    template <Component T>
    bool Remove(EntityId id) noexcept {
        if (!IsAlive(id)) return false;
        constexpr u8 tid = static_cast<u8>(T::kTypeId);
        if (m_poolOps[tid].poolPtr == nullptr) return false;
        auto* pool = static_cast<ComponentPool<T>*>(m_poolOps[tid].poolPtr);
        if (!pool->Remove(id)) return false;
        m_componentMasks[id.index] &= ~(1ULL << tid);
        return true;
    }

    template <Component T>
    [[nodiscard]] constexpr bool Has(EntityId id) const noexcept {
        return IsAlive(id) && ((m_componentMasks[id.index] & (1ULL << static_cast<u8>(T::kTypeId))) != 0ULL);
    }

    template <Component T>
    [[nodiscard]] constexpr T* TryGet(EntityId id) noexcept {
        if (!Has<T>(id)) return nullptr;
        auto* pool = static_cast<ComponentPool<T>*>(m_poolOps[static_cast<u8>(T::kTypeId)].poolPtr);
        return &pool->DirectAtEntityIndex(id.index);
    }
    template <Component T>
    [[nodiscard]] constexpr const T* TryGet(EntityId id) const noexcept {
        if (!Has<T>(id)) return nullptr;
        const auto* pool = static_cast<const ComponentPool<T>*>(m_poolOps[static_cast<u8>(T::kTypeId)].poolPtr);
        return &pool->DirectAtEntityIndex(id.index);
    }

    template <Component T>
    [[nodiscard]] constexpr ComponentPool<T>& GetPool() noexcept {
        return *static_cast<ComponentPool<T>*>(m_poolOps[static_cast<u8>(T::kTypeId)].poolPtr);
    }
    template <Component T>
    [[nodiscard]] constexpr const ComponentPool<T>& GetPool() const noexcept {
        return *static_cast<const ComponentPool<T>*>(m_poolOps[static_cast<u8>(T::kTypeId)].poolPtr);
    }

    // Variadic smallest-pool-driven query iterator with optional Exclude<...> filter.
    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> = {}) noexcept {
        if (!AllPoolsRegistered<FirstC, RestCs...>()) return;
        constexpr ComponentMask reqMask = MakeComponentMask<FirstC, RestCs...>();
        constexpr ComponentMask exclMask = MakeComponentMask<ExclCs...>();

        std::span<const EntityId> driverEntities = GetPool<FirstC>().Entities();
        if constexpr (sizeof...(RestCs) > 0) {
            const auto pickSmallest = [&]<Component C>() noexcept {
                const auto span = GetPool<C>().Entities();
                if (span.size() < driverEntities.size()) driverEntities = span;
            };
            (pickSmallest.template operator()<RestCs>(), ...);
        }

        for (const EntityId id : driverEntities) {
            const ComponentMask mask = m_componentMasks[id.index];
            if ((mask & reqMask) != reqMask || (mask & exclMask) != 0ULL) continue;
            fn(id, GetPool<FirstC>().DirectAtEntityIndex(id.index),
               GetPool<RestCs>().DirectAtEntityIndex(id.index)...);
        }
    }

    template <Component FirstC, Component... RestCs, Component... ExclCs, typename Fn>
    void Each(Fn&& fn, Exclude<ExclCs...> = {}) const noexcept {
        if (!AllPoolsRegistered<FirstC, RestCs...>()) return;
        constexpr ComponentMask reqMask = MakeComponentMask<FirstC, RestCs...>();
        constexpr ComponentMask exclMask = MakeComponentMask<ExclCs...>();

        std::span<const EntityId> driverEntities = GetPool<FirstC>().Entities();
        if constexpr (sizeof...(RestCs) > 0) {
            const auto pickSmallest = [&]<Component C>() noexcept {
                const auto span = GetPool<C>().Entities();
                if (span.size() < driverEntities.size()) driverEntities = span;
            };
            (pickSmallest.template operator()<RestCs>(), ...);
        }

        for (const EntityId id : driverEntities) {
            const ComponentMask mask = m_componentMasks[id.index];
            if ((mask & reqMask) != reqMask || (mask & exclMask) != 0ULL) continue;
            fn(id, GetPool<FirstC>().DirectAtEntityIndex(id.index),
               GetPool<RestCs>().DirectAtEntityIndex(id.index)...);
        }
    }

    [[nodiscard]] Result<void, WorldErrc> DeferDestroy(EntityId id) noexcept;
    [[nodiscard]] Result<void, WorldErrc> DeferSetParent(EntityId child, EntityId parent) noexcept;

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferEmplace(EntityId id, const T& comp) noexcept {
        return EnqueueRawCommand(CmdKind::Emplace, id, kNullEntity, static_cast<u8>(T::kTypeId), &comp, sizeof(T));
    }

    template <Component T>
    [[nodiscard]] Result<void, WorldErrc> DeferRemove(EntityId id) noexcept {
        return EnqueueRawCommand(CmdKind::Remove, id, kNullEntity, static_cast<u8>(T::kTypeId), nullptr, 0);
    }

    [[nodiscard]] Result<void, WorldErrc> FlushCommands() noexcept;

    [[nodiscard]] LocalTransform2D GetLocalTransform(EntityId id) const noexcept;
    [[nodiscard]] LocalTransform2D& ModifyLocalTransformByIndex(u16 idx) noexcept {
        m_transforms.MarkSubtreeDirty(idx);
        return m_transforms.Local(idx);
    }
    void SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept;
    void Teleport(EntityId id, const LocalTransform2D& local) noexcept;

    [[nodiscard]] WorldTransform2D GetWorldTransform(EntityId id) const noexcept;
    [[nodiscard]] const WorldTransform2D& GetWorldByIndex(u16 idx) const noexcept { return m_transforms.World(idx); }
    [[nodiscard]] const PrevWorldSnapshot2D& GetPrevWorldByIndex(u16 idx) const noexcept { return m_transforms.Prev(idx); }
    [[nodiscard]] constexpr u8 GetTimeChannelByIndex(u16 idx) const noexcept { return m_timeChannels[idx]; }
    constexpr void SetTimeChannel(EntityId id, TimeChannel ch) noexcept {
        if (IsAlive(id)) m_timeChannels[id.index] = static_cast<u8>(ch);
    }
    [[nodiscard]] constexpr ComponentMask GetComponentMaskByIndex(u16 idx) const noexcept { return m_componentMasks[idx]; }
    [[nodiscard]] constexpr EntityId EntityAtSlot(u16 idx) const noexcept { return EntityId{idx, m_generations[idx]}; }

    [[nodiscard]] Result<void, WorldErrc> SetParent(EntityId child, EntityId parent) noexcept;
    void PropagateTransforms() noexcept { m_transforms.PropagateHierarchy(m_aliveWords, m_maxAllocatedIndex); }

    [[nodiscard]] std::span<const WorldTransform2D> WorldTransforms() const noexcept {
        return m_transforms.WorldSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }
    [[nodiscard]] std::span<const PrevWorldSnapshot2D> PrevWorldTransforms() const noexcept {
        return m_transforms.PrevSpan(static_cast<u16>(m_maxAllocatedIndex + 1U));
    }
    [[nodiscard]] std::span<const u64> MovedWords() const noexcept { return m_transforms.MovedWords(); }
    [[nodiscard]] std::span<const u64> AliveWords() const noexcept { return m_aliveWords; }

    void BeginTick(const SimStepView& step) noexcept;
    void StepAmbientVelocities(const SimStepView& step) noexcept;
    void StepAnimationsAndLifetimes(const SimStepView& step) noexcept;
    void EndTick(const SimStepView& step = {}) noexcept;

    [[nodiscard]] Result<EntityId, WorldErrc> InstantiatePrefab(
        const AssetBank& bank, BlobHandle prefabBlob, const LocalTransform2D& rootTransform = {}) noexcept;

    [[nodiscard]] Result<usize, WorldErrc> SerializePrefabTemplate(
        EntityId rootEntity, std::span<u8> outBuffer) const noexcept;

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    enum class CmdKind : u8 { Destroy = 0, Emplace = 1, Remove = 2, SetParent = 3 };

    template <Component... Cs>
    [[nodiscard]] constexpr bool AllPoolsRegistered() const noexcept {
        return (!m_poolOps.empty()) &&
               ((m_poolOps[static_cast<u8>(Cs::kTypeId)].poolPtr != nullptr) && ...);
    }

    [[nodiscard]] Result<void, WorldErrc> EnqueueRawCommand(
        CmdKind kind, EntityId id, EntityId auxId, u8 typeId, const void* payload, u16 payloadBytes) noexcept;

    std::span<u16> m_generations{};
    std::span<ComponentMask> m_componentMasks{};
    std::span<u8> m_timeChannels{};
    std::span<u64> m_aliveWords{};
    std::span<PoolOps> m_poolOps{};
    std::span<u8> m_cmdTape{};

    u16 m_maxEntities{0};
    u16 m_maxAllocatedIndex{0};
    u16 m_nextFreshSlot{1};
    u16 m_freeHead{kInvalidDenseIndex};
    u16 m_defaultPoolCap{0};
    u32 m_aliveCount{0};
    u32 m_cmdWriteOffset{0};

    TransformSolver m_transforms{};
    AlignedArena m_arena{};
};

} // namespace en

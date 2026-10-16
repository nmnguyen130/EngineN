#include "world/World.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace en::world {

namespace {
[[nodiscard]] constexpr u8 BuildActiveChannelMask(time::SimStepView step) noexcept {
    u8 mask = 0;
    for (u8 c = 0; c < 8; ++c) {
        if (!step.IsFrozen(static_cast<time::TimeChannel>(c))) {
            mask |= static_cast<u8>(1u << c);
        }
    }
    return mask;
}
} // namespace

Result<void, WorldErrc> World::Init(const WorldConfig& cfg) noexcept {
    if (cfg.maxEntities < 64 || (cfg.maxEntities & 63u) != 0 || cfg.maxEntities > 65534) [[unlikely]] {
        return Err(WorldErrc::InvalidConfig);
    }
    Shutdown();
    m_cfg = cfg;

    m_arenaCapacity = 16u * 1024u * 1024u;
    m_arenaSlab = std::make_unique_for_overwrite<u8[]>(m_arenaCapacity);
    if (!m_arenaSlab) [[unlikely]] {
        return Err(WorldErrc::OutOfMemory);
    }
    m_arenaOffset = 0;

    const u16 maxEnt = m_cfg.maxEntities;
    const u32 wordCount = static_cast<u32>(maxEnt) >> 6;

    m_generations     = {CarveAligned<u16>(maxEnt, 64), maxEnt};
    m_flags           = {CarveAligned<EntityFlags>(maxEnt, 64), maxEnt};
    m_timeChannels    = {CarveAligned<time::TimeChannel>(maxEnt, 64), maxEnt};
    m_componentMasks  = {CarveAligned<ComponentMask>(maxEnt, 64), maxEnt};
    m_aliveWords      = {CarveAligned<u64>(wordCount, 64), wordCount};
    m_teleportedWords = {CarveAligned<u64>(wordCount, 64), wordCount};
    m_freeRing        = {CarveAligned<u16>(maxEnt, 64), maxEnt};
    m_cmdTape         = {CarveAligned<u8>(m_cfg.commandBufferBytes, 64), m_cfg.commandBufferBytes};

    auto* localSpan   = CarveAligned<LocalTransform2D>(maxEnt, 64);
    auto* worldSpan   = CarveAligned<WorldTransform2D>(maxEnt, 64);
    auto* prevSpan    = CarveAligned<PrevWorldSnapshot2D>(maxEnt, 64);
    auto* treeSpan    = CarveAligned<HierarchyNode>(maxEnt, 64);
    auto* depthSpan   = CarveAligned<u8>(maxEnt, 64);
    auto* orderSpan   = CarveAligned<HierarchyStep>(maxEnt, 64);
    auto* sAliveWords = CarveAligned<u64>(wordCount, 64);
    auto* sDynWords   = CarveAligned<u64>(wordCount, 64);
    auto* rDirtyWords = CarveAligned<u64>(wordCount, 64);
    auto* cDirtyWords = CarveAligned<u64>(wordCount, 64);
    auto* subDirtyW   = CarveAligned<u64>(wordCount, 64);

    if (!subDirtyW) [[unlikely]] {
        return Err(WorldErrc::OutOfMemory);
    }

    m_transforms.BindArena(
        {localSpan, maxEnt},
        {worldSpan, maxEnt},
        {prevSpan, maxEnt},
        {treeSpan, maxEnt},
        {depthSpan, maxEnt},
        {orderSpan, maxEnt},
        {sAliveWords, wordCount},
        {sDynWords, wordCount},
        {rDirtyWords, wordCount},
        {cDirtyWords, wordCount},
        {subDirtyW, wordCount}
    );

    m_poolOps.fill(PoolOps{});
    Clear();

    if (auto r = RegisterComponentPool<Velocity2D>(maxEnt); !r) return r;
    if (auto r = RegisterComponentPool<Sprite2D>(maxEnt); !r) return r;
    if (auto r = RegisterComponentPool<SpriteAnimator2D>(maxEnt); !r) return r;
    if (auto r = RegisterComponentPool<CameraRig2D>(64); !r) return r;

    return {};
}

void World::Shutdown() noexcept {
    m_poolOps.fill(PoolOps{});
    m_arenaSlab.reset();
    m_arenaCapacity = 0;
    m_arenaOffset   = 0;
}

void World::Clear() noexcept {
    std::fill(m_generations.begin(), m_generations.end(), u16{1});
    m_generations[0] = 0;
    std::fill(m_flags.begin(), m_flags.end(), EntityFlags::None);
    std::fill(m_timeChannels.begin(), m_timeChannels.end(), time::TimeChannel::World);
    std::fill(m_componentMasks.begin(), m_componentMasks.end(), ComponentMask{0});
    std::fill(m_aliveWords.begin(), m_aliveWords.end(), 0ULL);
    std::fill(m_teleportedWords.begin(), m_teleportedWords.end(), 0ULL);

    m_freeHead          = 0;
    m_freeTail          = 0;
    m_freeCount         = 0;
    m_nextFreshSlot     = 1;
    m_maxAllocatedIndex = 0;
    m_aliveCount        = 0;
    m_cmdWriteOffset    = 0;

    m_transforms.Clear();
    for (auto& ops : m_poolOps) {
        if (ops.poolInstance && ops.clearFn) {
            ops.clearFn(ops.poolInstance);
        }
    }
}

Result<EntityId, WorldErrc> World::Spawn(const EntitySpawnDesc& desc) noexcept {
    u16 idx = 0;
    if (m_nextFreshSlot < m_cfg.maxEntities) {
        idx = m_nextFreshSlot++;
    } else if (m_freeCount > 0) {
        idx = m_freeRing[m_freeHead];
        m_freeHead = (m_freeHead + 1u) % m_cfg.maxEntities;
        --m_freeCount;
    } else [[unlikely]] {
        return Err(WorldErrc::EntityPoolExhausted);
    }

    m_maxAllocatedIndex = std::max(m_maxAllocatedIndex, idx);
    ++m_aliveCount;

    const u16 gen = m_generations[idx];
    EntityFlags flags = EntityFlags::Alive | EntityFlags::Enabled | EntityFlags::Teleported;
    if (desc.hasSpatial) {
        flags = flags | EntityFlags::Spatial;
        if (desc.isStaticSpatial) {
            flags = flags | EntityFlags::StaticSpatial;
        }
        m_transforms.InitSlot(idx, desc.localTransform, desc.isStaticSpatial);
        if (desc.parent.IsValid() && IsAlive(desc.parent)) {
            (void)m_transforms.SetParent(idx, desc.parent.index);
        }
    }

    m_flags[idx]          = flags;
    m_timeChannels[idx]   = desc.timeChannel;
    m_componentMasks[idx] = 0;

    const u32 w = idx >> 6;
    const u64 b = 1ULL << (idx & 63u);
    m_aliveWords[w]      |= b;
    m_teleportedWords[w] |= b;

    return EntityId{idx, gen};
}

bool World::Destroy(EntityId id) noexcept {
    if (!IsAlive(id)) [[unlikely]] {
        return false;
    }
    const u16 idx = id.index;
    ComponentMask mask = m_componentMasks[idx];

    constexpr ComponentMask kAnimGroupMask = MakeComponentMask<Sprite2D, SpriteAnimator2D>();
    if ((mask & kAnimGroupMask) == kAnimGroupMask) {
        SyncLockstepAnimationGroupOnRemove(idx);
    }

    while (mask != 0) {
        const u32 tid = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1u);
        m_poolOps[tid].removeUncheckedFn(m_poolOps[tid].poolInstance, id);
    }
    m_componentMasks[idx] = 0;

    if (HasFlag(m_flags[idx], EntityFlags::Spatial)) {
        m_transforms.DisableSlot(idx);
    }

    m_flags[idx] = EntityFlags::None;
    const u32 w = idx >> 6;
    const u64 notB = ~(1ULL << (idx & 63u));
    m_aliveWords[w]      &= notB;
    m_teleportedWords[w] &= notB;

    u16 nextGen = static_cast<u16>(m_generations[idx] + 1u);
    if (nextGen == 0u) [[unlikely]] {
        nextGen = 1u;
    }
    m_generations[idx] = nextGen;

    m_freeRing[m_freeTail] = idx;
    m_freeTail = (m_freeTail + 1u) % m_cfg.maxEntities;
    ++m_freeCount;
    --m_aliveCount;
    return true;
}

void World::SyncLockstepAnimationGroupOnAdd(u16 entityIndex) noexcept {
    auto& spritePool = GetPool<Sprite2D>();
    auto& animPool   = GetPool<SpriteAnimator2D>();

    const u16 spriteSlot = spritePool.PromoteToGroup(entityIndex);
    const u16 animSlot   = animPool.PromoteToGroup(entityIndex);
    if (spriteSlot != animSlot) {
        animPool.SwapDenseSlots(animSlot, spriteSlot);
    }
}

void World::SyncLockstepAnimationGroupOnRemove(u16 entityIndex) noexcept {
    GetPool<Sprite2D>().DemoteFromGroup(entityIndex);
    GetPool<SpriteAnimator2D>().DemoteFromGroup(entityIndex);
}

void World::Teleport(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) [[unlikely]] {
        return;
    }
    m_transforms.SetLocal(id.index, local);
    m_flags[id.index] = m_flags[id.index] | EntityFlags::Teleported;
    m_teleportedWords[id.index >> 6] |= (1ULL << (id.index & 63u));
}

Result<void, WorldErrc> World::SetParent(EntityId child, EntityId parent) noexcept {
    if (!IsAlive(child)) [[unlikely]] {
        return Err(WorldErrc::InvalidEntity);
    }
    const u16 parentIdx = (parent.IsValid() && IsAlive(parent)) ? parent.index : 0u;
    return m_transforms.SetParent(child.index, parentIdx);
}

Result<void, WorldErrc> World::PushRawCommand(
    WorldCmdOp op, u8 componentTypeId, EntityId target, const void* payload, u16 payloadBytes) noexcept {
    const u32 alignedPayload = (static_cast<u32>(payloadBytes) + 7u) & ~7u;
    const u32 totalBytes = sizeof(WorldCmdHeader) + alignedPayload;
    if (m_cmdWriteOffset + totalBytes > m_cfg.commandBufferBytes) [[unlikely]] {
        return Err(WorldErrc::CommandBufferOverflow);
    }

    auto* hdr = reinterpret_cast<WorldCmdHeader*>(m_cmdTape.data() + m_cmdWriteOffset);
    *hdr = WorldCmdHeader{
        .op              = op,
        .componentTypeId = componentTypeId,
        .payloadBytes    = payloadBytes,
        .target          = target,
    };
    if (payloadBytes > 0 && payload != nullptr) {
        std::memcpy(m_cmdTape.data() + m_cmdWriteOffset + sizeof(WorldCmdHeader), payload, payloadBytes);
    }
    m_cmdWriteOffset += totalBytes;
    return {};
}

Result<void, WorldErrc> World::DeferDestroy(EntityId id) noexcept {
    return PushRawCommand(WorldCmdOp::DestroyEntity, 0, id, nullptr, 0);
}

Result<void, WorldErrc> World::DeferSetParent(EntityId child, EntityId parent) noexcept {
    return PushRawCommand(WorldCmdOp::SetParent, 0, child, &parent, sizeof(EntityId));
}

Result<void, WorldErrc> World::FlushCommands() noexcept {
    constexpr ComponentMask kAnimGroupMask = MakeComponentMask<Sprite2D, SpriteAnimator2D>();
    u32 offset = 0;

    while (offset < m_cmdWriteOffset) {
        const auto* hdr = reinterpret_cast<const WorldCmdHeader*>(m_cmdTape.data() + offset);
        const void* payload = m_cmdTape.data() + offset + sizeof(WorldCmdHeader);

        switch (hdr->op) {
            case WorldCmdOp::DestroyEntity:
                (void)Destroy(hdr->target);
                break;
            case WorldCmdOp::SetParent: {
                EntityId parent{};
                std::memcpy(&parent, payload, sizeof(EntityId));
                (void)SetParent(hdr->target, parent);
                break;
            }
            case WorldCmdOp::SetTimeChannel:
                if (IsAlive(hdr->target)) {
                    m_timeChannels[hdr->target.index] = *static_cast<const time::TimeChannel*>(payload);
                }
                break;
            case WorldCmdOp::EmplaceComponent: {
                const u8 tid = hdr->componentTypeId;
                if (IsAlive(hdr->target) && m_poolOps[tid].poolInstance) {
                    if (m_poolOps[tid].emplaceRawFn(m_poolOps[tid].poolInstance, hdr->target, payload)) {
                        const u16 idx = hdr->target.index;
                        const ComponentMask prevMask = m_componentMasks[idx];
                        const ComponentMask newMask  = prevMask | (ComponentMask{1} << tid);
                        m_componentMasks[idx] = newMask;
                        if ((prevMask & kAnimGroupMask) != kAnimGroupMask &&
                            (newMask & kAnimGroupMask) == kAnimGroupMask) {
                            SyncLockstepAnimationGroupOnAdd(idx);
                        }
                    }
                }
                break;
            }
            case WorldCmdOp::RemoveComponent: {
                const u8 tid = hdr->componentTypeId;
                const ComponentMask bit = ComponentMask{1} << tid;
                const u16 idx = hdr->target.index;
                if (IsAlive(hdr->target) && (m_componentMasks[idx] & bit) != 0) {
                    if ((m_componentMasks[idx] & kAnimGroupMask) == kAnimGroupMask &&
                        (tid == Sprite2D::kTypeId || tid == SpriteAnimator2D::kTypeId)) {
                        SyncLockstepAnimationGroupOnRemove(idx);
                    }
                    m_componentMasks[idx] &= ~bit;
                    m_poolOps[tid].removeUncheckedFn(m_poolOps[tid].poolInstance, hdr->target);
                }
                break;
            }
        }
        const u32 alignedPayload = (static_cast<u32>(hdr->payloadBytes) + 7u) & ~7u;
        offset += sizeof(WorldCmdHeader) + alignedPayload;
    }
    m_cmdWriteOffset = 0;
    return {};
}

void World::BeginTick(time::SimStepView step) noexcept {
    m_transforms.SnapshotPreviousTransforms(m_maxAllocatedIndex, BuildActiveChannelMask(step), m_timeChannels);
}

void World::StepAmbientVelocities(time::SimStepView step) noexcept {
    const u8 activeChannelMask = BuildActiveChannelMask(step);
    if (activeChannelMask == 0) {
        return;
    }

    alignas(32) std::array<f32, 8> chDt{};
    for (u8 c = 0; c < 8; ++c) {
        if ((activeChannelMask & (1u << c)) != 0) {
            chDt[c] = step.Dt(static_cast<time::TimeChannel>(c));
        }
    }

    const auto* __restrict channels = m_timeChannels.data();
    auto& velPool          = GetPool<Velocity2D>();
    const auto velEntities = velPool.Entities();
    auto velocities        = velPool.Components();

    for (usize i = 0; i < velEntities.size(); ++i) {
        const u16 idx = velEntities[i].index;
        const u8 ch   = static_cast<u8>(channels[idx]);
        if ((activeChannelMask & (1u << ch)) == 0) {
            continue;
        }
        const f32 dt          = chDt[ch];
        Velocity2D& v         = velocities[i];
        LocalTransform2D& loc = m_transforms.ModifyLocal(idx);

        loc.x += v.vx * dt;
        loc.y += v.vy * dt;
        loc.rotationRad = NormalizeAngleRad(loc.rotationRad + v.angularVelRad * dt);
        if (v.linearDamping > 0.0f) {
            const f32 damp = std::max(0.0f, 1.0f - v.linearDamping * dt);
            v.vx *= damp;
            v.vy *= damp;
        }
    }
}

void World::EndTick(time::SimStepView /*step*/) noexcept {
    (void)FlushCommands();
    m_transforms.Propagate(m_maxAllocatedIndex);

    const u32 wordCount = (static_cast<u32>(m_maxAllocatedIndex) + 63u) >> 6;
    for (u32 w = 0; w < wordCount; ++w) {
        u64 tele = m_teleportedWords[w] & m_aliveWords[w];
        if (tele == 0) {
            continue;
        }
        m_teleportedWords[w] = 0;
        while (tele != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(tele));
            tele &= (tele - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);
            m_transforms.SyncPrevToCurrent(idx);
            m_flags[idx] = m_flags[idx] & ~EntityFlags::Teleported;
        }
    }
}

u64 World::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    auto mixBytes = [&](const void* data, usize len) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0; i < len; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
        }
    };

    const u32 wordCount = (static_cast<u32>(m_maxAllocatedIndex) + 63u) >> 6;
    const auto worldTransforms = m_transforms.WorldSpan();
    for (u32 w = 0; w < wordCount; ++w) {
        u64 alive = m_aliveWords[w];
        while (alive != 0) {
            const u32 bit = static_cast<u32>(std::countr_zero(alive));
            alive &= (alive - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);

            mixBytes(&idx, sizeof(idx));
            mixBytes(&m_generations[idx], sizeof(u16));
            mixBytes(&m_timeChannels[idx], sizeof(u8));
            mixBytes(&m_componentMasks[idx], sizeof(ComponentMask));
            mixBytes(&worldTransforms[idx], sizeof(WorldTransform2D));
        }
    }

    for (const auto& ops : m_poolOps) {
        if (ops.poolInstance && ops.hashCanonicalFn) {
            const u64 poolHash = ops.hashCanonicalFn(ops.poolInstance, m_maxAllocatedIndex);
            mixBytes(&poolHash, sizeof(u64));
        }
    }
    return hash;
}

} // namespace en::world

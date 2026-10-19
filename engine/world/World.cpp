#include "world/World.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace en::world {

Result<void, WorldErrc> World::Init(const WorldConfig& cfg) noexcept {
    if (cfg.maxEntities < 64 || (cfg.maxEntities & 63u) != 0 || cfg.maxEntities > 65534 ||
        cfg.defaultComponentPoolCapacity == 0) [[unlikely]] {
        return Err(WorldErrc::InvalidConfig);
    }
    Shutdown();

    m_maxEntities    = cfg.maxEntities;
    m_defaultPoolCap = std::min<u16>(cfg.defaultComponentPoolCapacity, cfg.maxEntities);
    m_cmdCapacity    = cfg.commandBufferBytes;

    // Right-size arena to fit entity spine, built-in pools, command tape, and custom gameplay pools (~4 MB default).
    const usize minArena = (static_cast<usize>(m_maxEntities) * 256u) + m_cmdCapacity + (512u * 1024u);
    m_arenaCapacity = ((std::max<usize>(cfg.arenaBytes, minArena) + 63u) & ~usize{63u});
    m_arenaSlab = std::make_unique_for_overwrite<AlignedArenaLine[]>(m_arenaCapacity / sizeof(AlignedArenaLine));
    if (!m_arenaSlab) [[unlikely]] {
        return Err(WorldErrc::OutOfMemory);
    }
    m_arenaOffset = 0;

    const u16 maxEnt = m_maxEntities;
    const u32 wordCount = static_cast<u32>(maxEnt) >> 6;

    m_poolOps         = CarveAligned<PoolOps>(kMaxComponentTypes, 64);
    m_generations     = CarveAligned<u16>(maxEnt, 64);
    m_componentMasks  = CarveAligned<ComponentMask>(maxEnt, 64);
    m_timeChannels    = CarveAligned<time::TimeChannel>(maxEnt, 64);
    m_aliveWords      = CarveAligned<u64>(wordCount, 64);
    m_teleportedWords = CarveAligned<u64>(wordCount, 64);
    m_cmdTape         = CarveAligned<u8>(m_cmdCapacity, 64);

    auto* localSpan         = CarveAligned<LocalTransform2D>(maxEnt, 64);
    auto* worldSpan         = CarveAligned<WorldTransform2D>(maxEnt, 64);
    auto* prevSpan          = CarveAligned<PrevWorldSnapshot2D>(maxEnt, 64);
    auto* treeSpan          = CarveAligned<HierarchyNode>(maxEnt, 64);
    auto* orderSpan         = CarveAligned<HierarchyStep>(maxEnt, 64);
    auto* dirtyAndAliveWord = CarveAligned<u64>(wordCount * 2u, 64);
    auto* movedWords        = CarveAligned<u64>(wordCount, 64);

    if (!movedWords) [[unlikely]] {
        return Err(WorldErrc::OutOfMemory);
    }

    m_transforms.BindArena(
        {localSpan, maxEnt},
        {worldSpan, maxEnt},
        {prevSpan, maxEnt},
        {treeSpan, maxEnt},
        {orderSpan, maxEnt},
        {dirtyAndAliveWord, wordCount * 2u},
        {movedWords, wordCount}
    );

    std::fill_n(m_poolOps, kMaxComponentTypes, PoolOps{});
    m_maxAllocatedIndex = m_maxEntities - 1u;
    Clear();

    if (auto r = RegisterComponentPool<Velocity2D>(m_defaultPoolCap); !r) return r;
    if (auto r = RegisterComponentPool<Sprite2D>(maxEnt); !r) return r;
    if (auto r = RegisterComponentPool<SpriteAnimator2D>(m_defaultPoolCap); !r) return r;
    if (auto r = RegisterComponentPool<CameraRig2D>(64); !r) return r;

    return {};
}

void World::Shutdown() noexcept {
    m_poolOps           = nullptr;
    m_generations       = nullptr;
    m_componentMasks    = nullptr;
    m_timeChannels      = nullptr;
    m_aliveWords        = nullptr;
    m_teleportedWords   = nullptr;
    m_cmdTape           = nullptr;
    m_arenaSlab.reset();
    m_arenaCapacity     = 0;
    m_arenaOffset       = 0;
    m_maxEntities       = 0;
    m_maxAllocatedIndex = 0;
    m_aliveCount        = 0;
}

void World::Clear() noexcept {
    if (m_maxEntities == 0) {
        return;
    }
    const u32 clearSlots = std::min<u32>(static_cast<u32>(m_maxAllocatedIndex) + 1u, m_maxEntities);
    const u32 clearWords = std::min<u32>((static_cast<u32>(m_maxAllocatedIndex) + 64u) >> 6, m_maxEntities >> 6);

    std::memset(m_generations, 0, clearSlots * sizeof(u16));
    std::memset(m_componentMasks, 0, clearSlots * sizeof(ComponentMask));
    std::memset(m_timeChannels, 0, clearSlots * sizeof(time::TimeChannel));
    std::memset(m_aliveWords, 0, clearWords * sizeof(u64));
    std::memset(m_teleportedWords, 0, clearWords * sizeof(u64));

    m_transforms.Clear(m_maxAllocatedIndex);
    for (u8 tid = 0; tid < kMaxComponentTypes; ++tid) {
        const PoolOps& ops = m_poolOps[tid];
        if (ops.poolInstance && ops.removeOrClearFn) {
            ops.removeOrClearFn(ops.poolInstance, kNullEntity);
        }
    }

    m_freeHead          = 0;
    m_freeTail          = 0;
    m_nextFreshSlot     = 1;
    m_maxAllocatedIndex = 0;
    m_aliveCount        = 0;
    m_cmdWriteOffset    = 0;
}

Result<EntityId, WorldErrc> World::Spawn(const EntitySpawnDesc& desc) noexcept {
    u16 idx = 0;
    u16 gen = 1;
    if (m_nextFreshSlot < m_maxEntities) {
        idx = m_nextFreshSlot++;
    } else if (m_freeHead != 0) {
        idx = m_freeHead;
        const ComponentMask packed = m_componentMasks[idx];
        m_freeHead = static_cast<u16>(packed & 0xFFFFu);
        if (m_freeHead == 0) {
            m_freeTail = 0;
        }
        gen = static_cast<u16>((packed >> 16) & 0xFFFFu);
    } else [[unlikely]] {
        return Err(WorldErrc::EntityPoolExhausted);
    }

    m_maxAllocatedIndex   = std::max(m_maxAllocatedIndex, idx);
    ++m_aliveCount;
    m_generations[idx]    = gen;
    m_timeChannels[idx]   = desc.timeChannel;
    m_componentMasks[idx] = 0;

    if (desc.hasSpatial) {
        m_transforms.InitSlot(idx, desc.localTransform);
        if (desc.parent.IsValid() && IsAlive(desc.parent)) {
            (void)m_transforms.SetParent(idx, desc.parent.index);
        }
    }

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

    while (mask != 0) {
        const u32 tid = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1u);
        m_poolOps[tid].removeOrClearFn(m_poolOps[tid].poolInstance, id);
    }
    m_transforms.DisableSlot(idx);

    const u32 w = idx >> 6;
    const u64 notB = ~(1ULL << (idx & 63u));
    m_aliveWords[w]      &= notB;
    m_teleportedWords[w] &= notB;

    u16 nextGen = static_cast<u16>(id.generation + 1u);
    if (nextGen == 0u) [[unlikely]] {
        nextGen = 1u;
    }
    m_generations[idx] = 0;

    // Thread dead slot onto tail of intrusive FIFO freelist stored inside m_componentMasks[idx].
    m_componentMasks[idx] = static_cast<ComponentMask>(nextGen) << 16;
    if (m_freeHead == 0) {
        m_freeHead = idx;
    } else {
        m_componentMasks[m_freeTail] |= idx;
    }
    m_freeTail = idx;
    --m_aliveCount;
    return true;
}

void World::Teleport(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) [[unlikely]] {
        return;
    }
    m_transforms.SetLocal(id.index, local);
    m_teleportedWords[id.index >> 6] |= (1ULL << (id.index & 63u));
}

Result<void, WorldErrc> World::SetParent(EntityId child, EntityId parent) noexcept {
    if (!IsAlive(child)) [[unlikely]] {
        return Err(WorldErrc::InvalidEntity);
    }
    const u16 parentIdx = (parent.IsValid() && IsAlive(parent)) ? parent.index : 0u;
    return m_transforms.SetParent(child.index, parentIdx);
}

void World::SyncTeleportedSnapshots() noexcept {
    if (m_maxAllocatedIndex == 0) {
        return;
    }
    const u32 wordCount = (static_cast<u32>(m_maxAllocatedIndex) + 64u) >> 6;
    for (u32 w = 0; w < wordCount; ++w) {
        u64 tele = m_teleportedWords[w] & m_aliveWords[w];
        if (tele == 0) {
            continue;
        }
        m_teleportedWords[w] = 0;
        do {
            const u32 bit = static_cast<u32>(std::countr_zero(tele));
            tele &= (tele - 1u);
            const u16 idx = static_cast<u16>((w << 6) + bit);
            m_transforms.SyncPrevToCurrent(idx);
        } while (tele != 0);
    }
}

void World::PropagateTransforms() noexcept {
    m_transforms.Propagate(m_maxAllocatedIndex);
    SyncTeleportedSnapshots();
}

Result<void, WorldErrc> World::PushRawCommand(
    WorldCmdOp op, u8 componentTypeId, EntityId target, const void* payload, u16 payloadBytes) noexcept {
    const u32 alignedPayload = (static_cast<u32>(payloadBytes) + 7u) & ~7u;
    const u32 totalBytes = sizeof(WorldCmdHeader) + alignedPayload;
    if (m_cmdWriteOffset + totalBytes > m_cmdCapacity) [[unlikely]] {
        return Err(WorldErrc::CommandBufferOverflow);
    }

    auto* hdr = reinterpret_cast<WorldCmdHeader*>(m_cmdTape + m_cmdWriteOffset);
    *hdr = WorldCmdHeader{
        .op              = op,
        .componentTypeId = componentTypeId,
        .payloadBytes    = payloadBytes,
        .target          = target,
    };
    if (payloadBytes > 0 && payload != nullptr) {
        std::memcpy(m_cmdTape + m_cmdWriteOffset + sizeof(WorldCmdHeader), payload, payloadBytes);
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
    if (m_cmdWriteOffset == 0) {
        return {};
    }
    u32 offset = 0;

    while (offset < m_cmdWriteOffset) {
        const auto* hdr = reinterpret_cast<const WorldCmdHeader*>(m_cmdTape + offset);
        const void* payload = m_cmdTape + offset + sizeof(WorldCmdHeader);

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
                        m_componentMasks[hdr->target.index] |= (ComponentMask{1} << tid);
                    }
                }
                break;
            }
            case WorldCmdOp::RemoveComponent: {
                const u8 tid = hdr->componentTypeId;
                const ComponentMask bit = ComponentMask{1} << tid;
                const u16 idx = hdr->target.index;
                if (IsAlive(hdr->target) && (m_componentMasks[idx] & bit) != 0) {
                    m_componentMasks[idx] &= ~bit;
                    m_poolOps[tid].removeOrClearFn(m_poolOps[tid].poolInstance, hdr->target);
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
    const u8 activeChannelMask = static_cast<u8>(~step.FrozenMask());
    m_transforms.SnapshotPreviousTransforms(m_maxAllocatedIndex, activeChannelMask, m_timeChannels);
}

void World::StepAmbientVelocities(time::SimStepView step) noexcept {
    auto& velPool = GetPool<Velocity2D>();
    const usize count = velPool.Size();
    const u8 activeChannelMask = static_cast<u8>(~step.FrozenMask());
    if (count == 0 || activeChannelMask == 0) {
        return;
    }

    const auto* __restrict channels   = m_timeChannels;
    const auto* __restrict entities   = velPool.Entities().data();
    auto* __restrict velocities       = velPool.Components().data();

    for (usize i = 0; i < count; ++i) {
        Velocity2D& v = velocities[i];
        if (v.vx == 0.0f && v.vy == 0.0f && v.angularVelRad == 0.0f) {
            continue;
        }
        const u16 idx = entities[i].index;
        const time::TimeChannel ch = channels[idx];
        if ((activeChannelMask & (1u << static_cast<u8>(ch))) == 0) {
            continue;
        }

        const f32 dt = step.Dt(ch);
        LocalTransform2D& loc = m_transforms.ModifyLocal(idx);
        loc.x += v.vx * dt;
        loc.y += v.vy * dt;
        if (v.angularVelRad != 0.0f) {
            loc.rotationRad = NormalizeAngleRad(loc.rotationRad + v.angularVelRad * dt);
        }
        if (v.linearDamping > 0.0f) {
            const f32 damp = std::max(0.0f, 1.0f - v.linearDamping * dt);
            v.vx *= damp;
            v.vy *= damp;
        }
    }
}

void World::StepAnimations(time::SimStepView step) noexcept {
    auto& animPool = GetPool<SpriteAnimator2D>();
    const usize count = animPool.Size();
    const u8 activeChannelMask = static_cast<u8>(~step.FrozenMask());
    if (count == 0 || activeChannelMask == 0) {
        return;
    }

    const auto* __restrict channels = m_timeChannels;
    const auto* __restrict entities = animPool.Entities().data();
    auto* __restrict anims          = animPool.Components().data();

    for (usize i = 0; i < count; ++i) {
        SpriteAnimator2D& anim = anims[i];
        if (anim.playing == 0 || anim.frameCount <= 1) {
            continue;
        }
        const u16 idx = entities[i].index;
        const time::TimeChannel ch = channels[idx];
        if ((activeChannelMask & (1u << static_cast<u8>(ch))) == 0) {
            continue;
        }

        const u16 deltaSubTicks = static_cast<u16>(
            step.EffectiveScale(ch) * static_cast<f32>(SpriteAnimator2D::kSubTicksPerTick) + 0.5f
        );
        if (deltaSubTicks == 0) {
            continue;
        }

        u32 accum = static_cast<u32>(anim.subTickAccum) + deltaSubTicks;
        const u32 threshold = static_cast<u32>(std::max<u16>(1u, anim.ticksPerFrame))
                              * SpriteAnimator2D::kSubTicksPerTick;

        while (accum >= threshold) {
            accum -= threshold;
            if (anim.currentFrame + 1u < anim.frameCount) {
                ++anim.currentFrame;
            } else if (anim.loop != 0) {
                anim.currentFrame = 0;
            } else {
                anim.playing = 0;
                accum = 0;
                break;
            }
        }
        anim.subTickAccum = static_cast<u16>(accum);
    }
}

void World::EndTick(time::SimStepView /*step*/) noexcept {
    (void)FlushCommands();
    PropagateTransforms();
}

u64 World::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    auto mixBytes = [&](const void* data, usize len) noexcept {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0; i < len; ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ULL;
        }
    };

    const u32 wordCount = (static_cast<u32>(m_maxAllocatedIndex) + 64u) >> 6;
    const auto* worldTransforms = m_transforms.WorldSpan().data();
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

    for (u8 tid = 0; tid < kMaxComponentTypes; ++tid) {
        const PoolOps& ops = m_poolOps[tid];
        if (ops.poolInstance && ops.hashCanonicalFn) {
            const u64 poolHash = ops.hashCanonicalFn(ops.poolInstance, m_maxAllocatedIndex);
            if (poolHash != 0ULL) {
                mixBytes(&poolHash, sizeof(u64));
            }
        }
    }
    return hash;
}

} // namespace en::world

#include "world/World.h"
#include "assets/AssetBank.h"
#include "core/Containers.h"
#include "core/Serialization.h"

namespace en {

Result<void, WorldErrc> World::Init(const WorldConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxEntities < 4 || cfg.arenaBytes < 65536) return Err(WorldErrc::InvalidConfig);
    if (!m_arena.Init(cfg.arenaBytes)) return Err(WorldErrc::OutOfMemory);

    const usize wordCount = (static_cast<usize>(cfg.maxEntities) + 63U) / 64U;
    m_generations = m_arena.Carve<u16>(cfg.maxEntities);
    m_componentMasks = m_arena.Carve<ComponentMask>(cfg.maxEntities);
    m_timeChannels = m_arena.Carve<u8>(cfg.maxEntities);
    m_aliveWords = m_arena.Carve<u64>(wordCount);
    m_poolOps = m_arena.Carve<PoolOps>(kMaxComponentTypes);
    m_cmdTape = m_arena.Carve<u8>(cfg.commandBufferBytes);

    if (m_generations.empty() || m_componentMasks.empty() || m_timeChannels.empty() ||
        m_aliveWords.empty() || m_poolOps.empty() || m_cmdTape.empty() ||
        !m_transforms.Init(m_arena, cfg.maxEntities)) {
        Shutdown();
        return Err(WorldErrc::OutOfMemory);
    }

    m_maxEntities = cfg.maxEntities;
    m_defaultPoolCap = cfg.defaultPoolCapacity;
    m_maxAllocatedIndex = 0;
    m_nextFreshSlot = 1;
    m_freeHead = kInvalidDenseIndex;
    m_aliveCount = 0;
    m_cmdWriteOffset = 0;

    (void)RegisterComponentPool<Velocity2D>();
    (void)RegisterComponentPool<Sprite2D>();
    (void)RegisterComponentPool<SpriteAnimator2D>();
    (void)RegisterComponentPool<CameraRig2D>(64);
    (void)RegisterComponentPool<Lifetime2D>();
    (void)RegisterComponentPool<PointLight2D>(256);
    (void)RegisterComponentPool<Tag2D>();
    return {};
}

void World::Shutdown() noexcept {
    m_arena.Shutdown();
    m_maxEntities = 0;
    m_aliveCount = 0;
    m_cmdWriteOffset = 0;
}

void World::Clear() noexcept {
    if (m_maxEntities == 0) return;
    for (PoolOps& op : m_poolOps) {
        if (op.poolPtr && op.clearFn) op.clearFn(op.poolPtr);
    }
    const usize limit = static_cast<usize>(m_maxAllocatedIndex) + 1U;
    for (usize i = 0; i < limit; ++i) {
        m_generations[i] = 0;
        m_componentMasks[i] = 0;
        m_timeChannels[i] = 0;
    }
    for (u64& w : m_aliveWords) w = 0;
    m_transforms.Clear(m_maxAllocatedIndex);
    m_maxAllocatedIndex = 0;
    m_nextFreshSlot = 1;
    m_freeHead = kInvalidDenseIndex;
    m_aliveCount = 0;
    m_cmdWriteOffset = 0;
}

Result<EntityId, WorldErrc> World::Spawn(const EntitySpawnDesc& desc) noexcept {
    if (m_maxEntities == 0) return Err(WorldErrc::InvalidConfig);
    u16 idx = kInvalidDenseIndex;
    u16 gen = 1;
    if (m_freeHead != kInvalidDenseIndex) {
        idx = m_freeHead;
        const u64 packedFree = m_componentMasks[idx];
        m_freeHead = static_cast<u16>(packedFree & 0xFFFFU);
        gen = static_cast<u16>((packedFree >> 16U) & 0xFFFFU);
        if (gen == 0) gen = 1;
    } else {
        if (m_nextFreshSlot >= m_maxEntities) return Err(WorldErrc::EntityCapacityReached);
        idx = m_nextFreshSlot++;
        gen = 1;
    }

    if (idx > m_maxAllocatedIndex) m_maxAllocatedIndex = idx;
    m_generations[idx] = gen;
    m_componentMasks[idx] = 0;
    m_timeChannels[idx] = static_cast<u8>(desc.timeChannel);
    m_aliveWords[idx >> 6U] |= (1ULL << (idx & 63U));
    ++m_aliveCount;

    m_transforms.InitializeSlot(idx, desc.local);
    const EntityId id{idx, gen};
    if (desc.parent.IsValid() && IsAlive(desc.parent)) {
        (void)m_transforms.SetParent(idx, desc.parent.index);
        m_transforms.PropagateHierarchy(m_aliveWords, m_maxAllocatedIndex);
    }
    return id;
}

bool World::Destroy(EntityId id) noexcept {
    if (!IsAlive(id)) return false;
    const u16 idx = id.index;
    ComponentMask mask = m_componentMasks[idx];
    while (mask != 0ULL) {
        const u8 tid = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        if (m_poolOps[tid].poolPtr && m_poolOps[tid].removeUncheckedFn) {
            m_poolOps[tid].removeUncheckedFn(m_poolOps[tid].poolPtr, id);
        }
    }
    m_transforms.UnlinkSlot(idx);
    m_aliveWords[idx >> 6U] &= ~(1ULL << (idx & 63U));
    const u16 nextGen = static_cast<u16>(id.generation + 1U == 0 ? 1U : id.generation + 1U);
    m_generations[idx] = 0;
    m_componentMasks[idx] = static_cast<u64>(m_freeHead) | (static_cast<u64>(nextGen) << 16U);
    m_freeHead = idx;
    --m_aliveCount;
    return true;
}

Result<EntityId, WorldErrc> World::CloneEntity(EntityId source, const LocalTransform2D& overrideLocal) noexcept {
    if (!IsAlive(source)) return Err(WorldErrc::DeadEntity);
    EntitySpawnDesc desc{};
    desc.local = overrideLocal;
    desc.timeChannel = static_cast<TimeChannel>(m_timeChannels[source.index]);
    return Spawn(desc);
}

EntityId World::FindByTag(StringId tagName) const noexcept {
    EntityId found = kNullEntity;
    if (!tagName.IsValid()) return found;
    Each<Tag2D>([&](EntityId id, const Tag2D& t) noexcept {
        if (t.name == tagName && !found.IsValid()) found = id;
    });
    return found;
}

LocalTransform2D World::GetLocalTransform(EntityId id) const noexcept {
    return IsAlive(id) ? m_transforms.Local(id.index) : LocalTransform2D{};
}

void World::SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) return;
    m_transforms.Local(id.index) = local;
    m_transforms.MarkSubtreeDirty(id.index);
}

void World::Teleport(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) return;
    m_transforms.InitializeSlot(id.index, local);
    m_transforms.PropagateHierarchy(m_aliveWords, m_maxAllocatedIndex);
    m_transforms.Prev(id.index) = m_transforms.World(id.index).ToSnapshot();
}

WorldTransform2D World::GetWorldTransform(EntityId id) const noexcept {
    return IsAlive(id) ? m_transforms.World(id.index) : WorldTransform2D{};
}

Result<void, WorldErrc> World::SetParent(EntityId child, EntityId parent) noexcept {
    if (!IsAlive(child)) return Err(WorldErrc::DeadEntity);
    const u16 pIdx = (parent.IsValid() && IsAlive(parent)) ? parent.index : kInvalidDenseIndex;
    return m_transforms.SetParent(child.index, pIdx);
}

Result<void, WorldErrc> World::EnqueueRawCommand(
    CmdKind kind, EntityId id, EntityId auxId, u8 typeId, const void* payload, u16 payloadBytes) noexcept {
    const usize totalBytes = 12U + payloadBytes;
    if (m_cmdWriteOffset + totalBytes > m_cmdTape.size()) {
        return Err(WorldErrc::CommandBufferOverflow);
    }
    u8* dst = m_cmdTape.data() + m_cmdWriteOffset;
    dst[0] = static_cast<u8>(kind);
    dst[1] = typeId;
    std::memcpy(dst + 2, &payloadBytes, 2);
    std::memcpy(dst + 4, &id, 4);
    std::memcpy(dst + 8, &auxId, 4);
    if (payloadBytes > 0 && payload) {
        std::memcpy(dst + 12, payload, payloadBytes);
    }
    m_cmdWriteOffset += static_cast<u32>(totalBytes);
    return {};
}

Result<void, WorldErrc> World::DeferDestroy(EntityId id) noexcept {
    return EnqueueRawCommand(CmdKind::Destroy, id, kNullEntity, 0, nullptr, 0);
}

Result<void, WorldErrc> World::DeferSetParent(EntityId child, EntityId parent) noexcept {
    return EnqueueRawCommand(CmdKind::SetParent, child, parent, 0, nullptr, 0);
}

Result<void, WorldErrc> World::FlushCommands() noexcept {
    usize offset = 0;
    while (offset + 12U <= m_cmdWriteOffset) {
        const u8* src = m_cmdTape.data() + offset;
        const auto kind = static_cast<CmdKind>(src[0]);
        const u8 tid = src[1];
        u16 payloadBytes = 0;
        EntityId id{};
        EntityId auxId{};
        std::memcpy(&payloadBytes, src + 2, 2);
        std::memcpy(&id, src + 4, 4);
        std::memcpy(&auxId, src + 8, 4);
        const void* payload = src + 12;
        offset += 12U + payloadBytes;

        switch (kind) {
            case CmdKind::Destroy:
                (void)Destroy(id);
                break;
            case CmdKind::Emplace:
                if (IsAlive(id) && tid < kMaxComponentTypes && m_poolOps[tid].poolPtr) {
                    if (m_poolOps[tid].emplaceRawFn(m_poolOps[tid].poolPtr, id, payload)) {
                        m_componentMasks[id.index] |= (1ULL << tid);
                    }
                }
                break;
            case CmdKind::Remove:
                if (IsAlive(id) && tid < kMaxComponentTypes && m_poolOps[tid].poolPtr) {
                    m_poolOps[tid].removeUncheckedFn(m_poolOps[tid].poolPtr, id);
                    m_componentMasks[id.index] &= ~(1ULL << tid);
                }
                break;
            case CmdKind::SetParent:
                (void)SetParent(id, auxId);
                break;
        }
    }
    m_cmdWriteOffset = 0;
    return {};
}

void World::BeginTick(const SimStepView& step) noexcept {
    m_transforms.PropagateHierarchy(m_aliveWords, m_maxAllocatedIndex);
    m_transforms.SnapshotPreviousTransforms(m_aliveWords, m_timeChannels, step.frozenMask, m_maxAllocatedIndex);
}

void World::StepAmbientVelocities(const SimStepView& step) noexcept {
    Each<Velocity2D>([&](EntityId id, Velocity2D& vel) noexcept {
        const auto ch = static_cast<TimeChannel>(m_timeChannels[id.index]);
        if (step.IsFrozen(ch)) return;
        const f32 dt = step.Dt(ch);
        if (vel.linearDamping > 0.0f) {
            const f32 damp = std::max(0.0f, 1.0f - vel.linearDamping * dt);
            vel.vx *= damp;
            vel.vy *= damp;
        }
        LocalTransform2D& loc = ModifyLocalTransformByIndex(id.index);
        loc.x += vel.vx * dt;
        loc.y += vel.vy * dt;
        loc.rotationRad += vel.angularVelRad * dt;
    });
}

void World::StepAnimationsAndLifetimes(const SimStepView& step) noexcept {
    Each<SpriteAnimator2D>([&](EntityId id, SpriteAnimator2D& anim) noexcept {
        if (!anim.playing || anim.frameCount <= 1) return;
        const auto ch = static_cast<TimeChannel>(m_timeChannels[id.index]);
        if (step.IsFrozen(ch)) return;
        const u16 threshold = std::max<u16>(1, anim.ticksPerFrame);
        if (++anim.tickAccum >= threshold) {
            anim.tickAccum = 0;
            if (anim.currentFrame + 1U < anim.frameCount) {
                ++anim.currentFrame;
            } else if (anim.loop) {
                anim.currentFrame = 0;
            } else {
                anim.playing = false;
            }
        }
    });

    Each<Lifetime2D>([&](EntityId id, Lifetime2D& life) noexcept {
        if (!life.active) return;
        const auto ch = static_cast<TimeChannel>(m_timeChannels[id.index]);
        if (step.IsFrozen(ch)) return;
        if (life.remainingTicks > 0) --life.remainingTicks;
        if (life.fadeSpriteAlpha && life.initialTicks > 0) {
            life.alphaScale = static_cast<f32>(life.remainingTicks) / static_cast<f32>(life.initialTicks);
            if (Sprite2D* sp = TryGet<Sprite2D>(id)) {
                sp->tint.a = life.alphaScale;
            }
        }
        if (life.remainingTicks == 0) {
            (void)DeferDestroy(id);
        }
    });

    Each<PointLight2D>([&](EntityId id, PointLight2D& light) noexcept {
        const auto ch = static_cast<TimeChannel>(m_timeChannels[id.index]);
        if (!step.IsFrozen(ch) && light.pulseSpeed != 0.0f) {
            light.pulsePhase += light.pulseSpeed * step.Dt(ch);
        }
    });
}

void World::EndTick(const SimStepView& /*step*/) noexcept {
    (void)FlushCommands();
    m_transforms.PropagateHierarchy(m_aliveWords, m_maxAllocatedIndex);
}

Result<EntityId, WorldErrc> World::InstantiatePrefab(
    const AssetBank& bank, BlobHandle prefabBlob, const LocalTransform2D& rootTransform) noexcept {
    const BlobRecord& blob = bank.GetBlob(prefabBlob);
    if (!blob.IsValid() || blob.size < sizeof(CookedPrefabHeader)) return Err(WorldErrc::CorruptedPrefab);

    BinaryReader reader(blob.Bytes());
    CookedPrefabHeader hdr{};
    if (!reader.Read(hdr) || hdr.magic != kCookedPrefabMagic || hdr.entityCount == 0) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    SmallVector<EntityId, 64> spawned{};
    for (u16 i = 0; i < hdr.entityCount; ++i) {
        CookedPrefabEntityRecord rec{};
        if (!reader.Read(rec)) return Err(WorldErrc::CorruptedPrefab);
        EntitySpawnDesc sdesc{};
        sdesc.local = (i == 0) ? rootTransform : rec.local;
        sdesc.timeChannel = static_cast<TimeChannel>(rec.timeChannel);
        if (rec.parentEntityIndex >= 0 && static_cast<usize>(rec.parentEntityIndex) < spawned.Size()) {
            sdesc.parent = spawned[static_cast<usize>(rec.parentEntityIndex)];
        }
        auto entRes = Spawn(sdesc);
        if (!entRes) return entRes;
        (void)spawned.PushBack(*entRes);

        for (u8 c = 0; c < rec.componentCount; ++c) {
            u8 tid = 0;
            u16 compSize = 0;
            if (!reader.Read(tid) || !reader.Read(compSize)) return Err(WorldErrc::CorruptedPrefab);
            if (reader.Remaining() < compSize) return Err(WorldErrc::CorruptedPrefab);
            const void* rawPtr = blob.data + reader.Offset();
            std::array<u8, 128> skipBuf{};
            if (compSize <= skipBuf.size()) (void)reader.ReadBytes(std::span<u8>(skipBuf.data(), compSize));
            if (tid < kMaxComponentTypes && m_poolOps[tid].poolPtr && m_poolOps[tid].emplaceRawFn) {
                if (m_poolOps[tid].emplaceRawFn(m_poolOps[tid].poolPtr, *entRes, rawPtr)) {
                    m_componentMasks[entRes->index] |= (1ULL << tid);
                }
            }
        }
    }
    PropagateTransforms();
    return spawned[0];
}

Result<usize, WorldErrc> World::SerializePrefabTemplate(
    EntityId rootEntity, std::span<u8> outBuffer) const noexcept {
    if (!IsAlive(rootEntity)) return Err(WorldErrc::DeadEntity);

    SmallVector<u16, 32> nodes{};
    SmallVector<i16, 32> parentMap{};
    (void)nodes.PushBack(rootEntity.index);
    (void)parentMap.PushBack(-1);

    for (u16 i = 1; i <= m_maxAllocatedIndex; ++i) {
        if (m_generations[i] == 0) continue;
        if (m_transforms.ParentOf(i) == rootEntity.index && nodes.Size() < 32) {
            (void)nodes.PushBack(i);
            (void)parentMap.PushBack(0);
        }
    }

    BinaryWriter writer(outBuffer);
    CookedPrefabHeader hdr{};
    hdr.magic = kCookedPrefabMagic;
    hdr.version = 1;
    hdr.entityCount = static_cast<u16>(nodes.Size());
    hdr.componentRecordCount = 0;
    hdr.schemaCrc32 = 0;
    if (!writer.Write(hdr)) return Err(WorldErrc::OutOfMemory);

    for (usize n = 0; n < nodes.Size(); ++n) {
        const u16 idx = nodes[n];
        const EntityId eid{idx, m_generations[idx]};
        u8 compCount = 0;
        if (Has<Sprite2D>(eid)) ++compCount;
        if (Has<Tag2D>(eid)) ++compCount;
        if (Has<PointLight2D>(eid)) ++compCount;
        if (Has<Lifetime2D>(eid)) ++compCount;

        CookedPrefabEntityRecord rec{};
        rec.local = m_transforms.Local(idx);
        rec.parentEntityIndex = parentMap[n];
        rec.timeChannel = m_timeChannels[idx];
        rec.componentCount = compCount;
        rec.entityFlags = 0;
        if (!writer.Write(rec)) return Err(WorldErrc::OutOfMemory);

        auto writeComp = [&]<Component C>() -> bool {
            if (const C* c = TryGet<C>(eid)) {
                const u8 tid = C::kTypeId;
                const u16 csize = static_cast<u16>(sizeof(C));
                return writer.Write(tid) && writer.Write(csize) && writer.Write(*c);
            }
            return true;
        };
        if (!writeComp.template operator()<Sprite2D>() ||
            !writeComp.template operator()<Tag2D>() ||
            !writeComp.template operator()<PointLight2D>() ||
            !writeComp.template operator()<Lifetime2D>()) {
            return Err(WorldErrc::OutOfMemory);
        }
    }
    return writer.Offset();
}

u64 World::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_aliveCount) | (static_cast<u64>(m_maxAllocatedIndex) << 32U));
    for (u16 i = 1; i <= m_maxAllocatedIndex; ++i) {
        if (m_generations[i] == 0) continue;
        h = HashCombine64(h, m_componentMasks[i] ^ (static_cast<u64>(m_generations[i]) << 48U));
        const WorldTransform2D& wt = m_transforms.World(i);
        h = HashCombine64(h, static_cast<u64>(std::bit_cast<u32>(wt.x)) |
                             (static_cast<u64>(std::bit_cast<u32>(wt.y)) << 32U));
    }
    return h;
}

} // namespace en

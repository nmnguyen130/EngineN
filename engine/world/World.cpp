#include "world/World.h"
#include "assets/AssetBank.h"

namespace en {

Result<void, WorldErrc> World::Init(const WorldConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxEntities < 4U || cfg.defaultPoolCapacity == 0U ||
        cfg.commandBufferBytes < 1024U || cfg.arenaBytes < 65536U) {
        return Err(WorldErrc::InvalidConfig);
    }
    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(WorldErrc::OutOfMemory);
    }

    const usize wordCount = (static_cast<usize>(cfg.maxEntities) + 63U) >> 6U;
    auto genSpan = m_arena.Carve<u16>(cfg.maxEntities);
    auto maskSpan = m_arena.Carve<u64>(cfg.maxEntities);
    auto chSpan = m_arena.Carve<u8>(cfg.maxEntities);
    auto aliveSpan = m_arena.Carve<u64>(wordCount);
    auto teleSpan = m_arena.Carve<u64>(wordCount);
    auto poolsSpan = m_arena.Carve<RawComponentPool>(kMaxComponentTypes);
    auto cmdSpan = m_arena.Carve<u8>(cfg.commandBufferBytes);

    if (genSpan.empty() || maskSpan.empty() || chSpan.empty() ||
        aliveSpan.empty() || teleSpan.empty() || poolsSpan.empty() ||
        cmdSpan.empty() || !m_transforms.Init(m_arena, cfg.maxEntities)) {
        Shutdown();
        return Err(WorldErrc::OutOfMemory);
    }

    m_generations = genSpan.data();
    m_componentMasks = maskSpan.data();
    m_timeChannels = chSpan.data();
    m_aliveWords = aliveSpan.data();
    m_teleportedWords = teleSpan.data();
    m_pools = poolsSpan.data();
    m_cmdTape = cmdSpan.data();

    m_maxEntities = cfg.maxEntities;
    m_defaultPoolCap = cfg.defaultPoolCapacity;
    m_maxAllocatedIndex = 0U;
    m_nextFreshSlot = 1U;
    m_freeHead = kInvalidDenseIndex;
    m_freeTail = kInvalidDenseIndex;
    m_aliveCount = 0U;
    m_registeredPoolsMask = 0ULL;
    m_cmdCapacity = cfg.commandBufferBytes;
    m_cmdWriteOffset = 0U;
    m_tickCount = 0U;
    m_aliveWordCount = static_cast<u16>(wordCount);
    m_pendingCmdCount = 0U;
    m_spawnedTotalCount = 0ULL;
    m_destroyedTotalCount = 0ULL;

    if (!RegisterComponentPool<Velocity2D>().has_value() ||
        !RegisterComponentPool<Sprite2D>().has_value() ||
        !RegisterComponentPool<SpriteAnimator2D>().has_value() ||
        !RegisterComponentPool<CameraRig2D>(64U).has_value() ||
        !RegisterComponentPool<Lifetime2D>().has_value() ||
        !RegisterComponentPool<PointLight2D>(256U).has_value() ||
        !RegisterComponentPool<Tag2D>().has_value()) {
        Shutdown();
        return Err(WorldErrc::OutOfMemory);
    }
    return {};
}

void World::Shutdown() noexcept {
    m_arena.Shutdown();
    m_generations = nullptr;
    m_componentMasks = nullptr;
    m_timeChannels = nullptr;
    m_aliveWords = nullptr;
    m_teleportedWords = nullptr;
    m_pools = nullptr;
    m_cmdTape = nullptr;
    m_maxEntities = 0U;
    m_maxAllocatedIndex = 0U;
    m_nextFreshSlot = 1U;
    m_freeHead = kInvalidDenseIndex;
    m_freeTail = kInvalidDenseIndex;
    m_defaultPoolCap = 0U;
    m_aliveCount = 0U;
    m_registeredPoolsMask = 0ULL;
    m_cmdCapacity = 0U;
    m_cmdWriteOffset = 0U;
    m_tickCount = 0U;
    m_aliveWordCount = 0U;
    m_pendingCmdCount = 0U;
    m_spawnedTotalCount = 0ULL;
    m_destroyedTotalCount = 0ULL;
}

void World::Clear() noexcept {
    if (m_maxEntities == 0U) return;
    u64 regMask = m_registeredPoolsMask;
    while (regMask != 0ULL) {
        const u8 tid = static_cast<u8>(std::countr_zero(regMask));
        regMask &= (regMask - 1ULL);
        m_pools[tid].Clear();
    }

    const usize limit = static_cast<usize>(m_maxAllocatedIndex) + 1U;
    for (usize i = 0U; i < limit; ++i) {
        m_generations[i] = 0U;
        m_componentMasks[i] = 0ULL;
        m_timeChannels[i] = 0U;
    }
    const usize activeWords = (limit + 63U) >> 6U;
    for (usize w = 0U; w < activeWords; ++w) {
        m_aliveWords[w] = 0ULL;
        m_teleportedWords[w] = 0ULL;
    }
    m_transforms.Clear(m_maxAllocatedIndex);
    m_maxAllocatedIndex = 0U;
    m_nextFreshSlot = 1U;
    m_freeHead = kInvalidDenseIndex;
    m_freeTail = kInvalidDenseIndex;
    m_aliveCount = 0U;
    m_cmdWriteOffset = 0U;
    m_pendingCmdCount = 0U;
}

Result<EntityId, WorldErrc> World::Spawn(const EntitySpawnDesc& desc) noexcept {
    if (m_maxEntities == 0U) return Err(WorldErrc::InvalidConfig);
    u16 idx = kInvalidDenseIndex;
    u16 gen = 1U;

    if (m_freeHead != kInvalidDenseIndex) {
        idx = m_freeHead;
        const u64 packedFree = m_componentMasks[idx];
        m_freeHead = static_cast<u16>(packedFree & 0xFFFFU);
        if (m_freeHead == kInvalidDenseIndex) {
            m_freeTail = kInvalidDenseIndex;
        }
        gen = static_cast<u16>((packedFree >> 16U) & 0xFFFFU);
        if (gen == 0U) gen = 1U;
    } else {
        if (m_nextFreshSlot >= m_maxEntities) return Err(WorldErrc::EntityCapacityReached);
        idx = m_nextFreshSlot++;
        gen = 1U;
    }

    if (idx > m_maxAllocatedIndex) m_maxAllocatedIndex = idx;
    m_generations[idx] = gen;
    m_componentMasks[idx] = 0ULL;
    m_timeChannels[idx] = static_cast<u8>(desc.timeChannel);
    const u64 bit = (1ULL << (idx & 63U));
    m_aliveWords[idx >> 6U] |= bit;
    m_teleportedWords[idx >> 6U] &= ~bit;
    ++m_aliveCount;
    ++m_spawnedTotalCount;

    const u16 parentIdx = (desc.parent.IsValid() && IsAlive(desc.parent))
                              ? desc.parent.index
                              : kInvalidDenseIndex;
    m_transforms.InitializeSlot(idx, desc.local, parentIdx, desc.keepWorldTransform);
    return EntityId{idx, gen};
}

void World::DestroySingleSlot(EntityId id) noexcept {
    const u16 idx = id.index;
    u64 mask = m_componentMasks[idx];
    while (mask != 0ULL) {
        const u8 tid = static_cast<u8>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        m_pools[tid].RemoveUnchecked(id);
    }
    m_transforms.UnlinkSlot(idx, m_maxEntities);
    const u64 bit = (1ULL << (idx & 63U));
    m_aliveWords[idx >> 6U] &= ~bit;
    m_teleportedWords[idx >> 6U] &= ~bit;

    const u16 nextGen = static_cast<u16>((id.generation + 1U == 0U) ? 1U : (id.generation + 1U));
    m_generations[idx] = 0U;

    // Thread destroyed slot onto the tail of the intrusive FIFO freelist inside m_componentMasks
    m_componentMasks[idx] = static_cast<u64>(kInvalidDenseIndex) | (static_cast<u64>(nextGen) << 16U);
    if (m_freeTail != kInvalidDenseIndex) {
        const u64 tailGenBits = m_componentMasks[m_freeTail] & ~0xFFFFULL;
        m_componentMasks[m_freeTail] = tailGenBits | static_cast<u64>(idx);
        m_freeTail = idx;
    } else {
        m_freeHead = idx;
        m_freeTail = idx;
    }

    --m_aliveCount;
    ++m_destroyedTotalCount;
}

bool World::Destroy(EntityId id, bool destroyChildren) noexcept {
    if (!IsAlive(id)) return false;
    if (!destroyChildren || m_transforms.FirstChildOf(id.index) == kInvalidDenseIndex) {
        DestroySingleSlot(id);
        return true;
    }

    // Gather subtree nodes in pre-order, then destroy bottom-up (reverse pre-order)
    std::array<EntityId, 256> toDestroy{};
    usize count = 0U;
    m_transforms.TraverseSubtree(id.index, m_maxEntities, [&](u16 cur) noexcept {
        if (count < toDestroy.size() && m_generations[cur] != 0U) {
            toDestroy[count++] = EntityId{cur, m_generations[cur]};
        }
    });

    for (usize i = count; i > 0U; --i) {
        const EntityId target = toDestroy[i - 1U];
        if (IsAlive(target)) {
            DestroySingleSlot(target);
        }
    }
    return true;
}

Result<EntityId, WorldErrc> World::CloneEntity(
    EntityId source,
    const LocalTransform2D& overrideLocal,
    bool cloneChildren) noexcept {
    if (!IsAlive(source)) return Err(WorldErrc::DeadEntity);

    if (!cloneChildren || m_transforms.FirstChildOf(source.index) == kInvalidDenseIndex) {
        EntitySpawnDesc desc{};
        desc.local = overrideLocal;
        desc.parent = GetParent(source);
        desc.timeChannel = static_cast<TimeChannel>(m_timeChannels[source.index]);
        auto cloneRes = Spawn(desc);
        if (!cloneRes) return cloneRes;

        const EntityId cloneId = *cloneRes;
        u64 mask = m_componentMasks[source.index];
        while (mask != 0ULL) {
            const u8 tid = static_cast<u8>(std::countr_zero(mask));
            mask &= (mask - 1ULL);
            if (m_pools[tid].CloneFromEntityIndex(cloneId, source.index)) {
                m_componentMasks[cloneId.index] |= (1ULL << tid);
            }
        }
        return cloneId;
    }

    // Clone full subtree hierarchy in pre-order
    std::array<u16, 128> srcNodes{};
    std::array<EntityId, 128> clonedNodes{};
    usize nodeCount = 0U;

    const u16 rootIdx = source.index;
    m_transforms.TraverseSubtree(rootIdx, m_maxEntities, [&](u16 cur) noexcept {
        if (nodeCount < srcNodes.size() && m_generations[cur] != 0U) {
            srcNodes[nodeCount++] = cur;
        }
    });

    for (usize n = 0U; n < nodeCount; ++n) {
        const u16 srcIdx = srcNodes[n];
        EntitySpawnDesc desc{};
        desc.local = (n == 0U) ? overrideLocal : m_transforms.Local(srcIdx);
        desc.timeChannel = static_cast<TimeChannel>(m_timeChannels[srcIdx]);
        if (n == 0U) {
            desc.parent = GetParent(source);
        } else {
            const u16 srcParent = m_transforms.ParentOf(srcIdx);
            for (usize p = 0U; p < n; ++p) {
                if (srcNodes[p] == srcParent) {
                    desc.parent = clonedNodes[p];
                    break;
                }
            }
        }

        auto cloneRes = Spawn(desc);
        if (!cloneRes) return cloneRes;
        clonedNodes[n] = *cloneRes;

        u64 mask = m_componentMasks[srcIdx];
        while (mask != 0ULL) {
            const u8 tid = static_cast<u8>(std::countr_zero(mask));
            mask &= (mask - 1ULL);
            if (m_pools[tid].CloneFromEntityIndex(*cloneRes, srcIdx)) {
                m_componentMasks[cloneRes->index] |= (1ULL << tid);
            }
        }
    }
    return clonedNodes[0];
}

EntityId World::FindByTag(StringId tagName) const noexcept {
    if (!tagName.IsValid() || (m_registeredPoolsMask & (1ULL << Tag2D::kTypeId)) == 0ULL) {
        return kNullEntity;
    }
    const auto& tagPool = GetPool<Tag2D>();
    const auto entities = tagPool.Entities();
    const auto tags = tagPool.Components();
    for (usize i = 0U; i < entities.size(); ++i) {
        if (tags[i].name == tagName) {
            return entities[i];
        }
    }
    return kNullEntity;
}

u32 World::FindByCategory(u32 categoryMask, std::span<EntityId> outEntities) const noexcept {
    if (categoryMask == 0U || (m_registeredPoolsMask & (1ULL << Tag2D::kTypeId)) == 0ULL) {
        return 0U;
    }
    const auto& tagPool = GetPool<Tag2D>();
    const auto entities = tagPool.Entities();
    const auto tags = tagPool.Components();
    u32 matched = 0U;
    for (usize i = 0U; i < entities.size(); ++i) {
        if ((tags[i].categoryMask & categoryMask) != 0U) {
            if (matched < outEntities.size()) {
                outEntities[matched] = entities[i];
            }
            ++matched;
        }
    }
    return matched;
}

LocalTransform2D World::GetLocalTransform(EntityId id) const noexcept {
    return IsAlive(id) ? m_transforms.Local(id.index) : LocalTransform2D{};
}

void World::SetLocalTransform(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) return;
    m_transforms.Local(id.index) = local;
    m_transforms.MarkSubtreeDirty(id.index, m_maxEntities);
}

void World::Teleport(EntityId id, const LocalTransform2D& local) noexcept {
    if (!IsAlive(id)) return;
    m_transforms.Local(id.index) = local;
    m_transforms.TeleportSubtree(id.index, m_maxEntities);
    m_teleportedWords[id.index >> 6U] |= (1ULL << (id.index & 63U));
}

WorldTransform2D World::GetWorldTransform(EntityId id) const noexcept {
    return IsAlive(id) ? m_transforms.World(id.index) : WorldTransform2D{};
}

Result<void, WorldErrc> World::SetParent(
    EntityId child,
    EntityId parent,
    bool keepWorldTransform) noexcept {
    if (!IsAlive(child)) return Err(WorldErrc::DeadEntity);
    if (parent.IsValid() && !IsAlive(parent)) return Err(WorldErrc::DeadEntity);
    const u16 pIdx = parent.IsValid() ? parent.index : kInvalidDenseIndex;
    return m_transforms.SetParent(child.index, pIdx, m_maxEntities, keepWorldTransform);
}

EntityId World::GetParent(EntityId child) const noexcept {
    if (!IsAlive(child)) return kNullEntity;
    const u16 pIdx = m_transforms.ParentOf(child.index);
    return (pIdx != kInvalidDenseIndex && pIdx < m_maxEntities)
               ? EntityId{pIdx, m_generations[pIdx]}
               : kNullEntity;
}

bool World::EmplaceRawByTypeId(EntityId id, u8 typeId, const void* rawData) noexcept {
    if (!IsAlive(id) || typeId >= kMaxComponentTypes || !rawData) return false;
    if ((m_registeredPoolsMask & (1ULL << typeId)) == 0ULL) return false;
    if (m_pools[typeId].EmplaceRaw(id, rawData)) {
        m_componentMasks[id.index] |= (1ULL << typeId);
        return true;
    }
    return false;
}

Result<void, WorldErrc> World::EnqueueRawCommand(
    CmdKind kind,
    EntityId id,
    EntityId auxId,
    u8 typeId,
    const void* payload,
    u16 payloadBytes) noexcept {
    const usize totalBytes = 12U + static_cast<usize>(payloadBytes);
    if (m_cmdWriteOffset + totalBytes > m_cmdCapacity) {
        return Err(WorldErrc::CommandBufferOverflow);
    }
    u8* dst = m_cmdTape + m_cmdWriteOffset;
    dst[0] = static_cast<u8>(kind);
    dst[1] = typeId;
    std::memcpy(dst + 2U, &payloadBytes, 2U);
    std::memcpy(dst + 4U, &id, 4U);
    std::memcpy(dst + 8U, &auxId, 4U);
    if (payloadBytes > 0U && payload != nullptr) {
        std::memcpy(dst + 12U, payload, payloadBytes);
    }
    m_cmdWriteOffset += static_cast<u32>(totalBytes);
    ++m_pendingCmdCount;
    return {};
}

Result<void, WorldErrc> World::DeferDestroy(EntityId id, bool destroyChildren) noexcept {
    return EnqueueRawCommand(
        CmdKind::Destroy, id, kNullEntity, destroyChildren ? 1U : 0U, nullptr, 0U);
}

Result<void, WorldErrc> World::DeferSetParent(
    EntityId child,
    EntityId parent,
    bool keepWorldTransform) noexcept {
    return EnqueueRawCommand(
        CmdKind::SetParent, child, parent, keepWorldTransform ? 1U : 0U, nullptr, 0U);
}

Result<void, WorldErrc> World::FlushCommands() noexcept {
    usize offset = 0U;
    while (offset + 12U <= m_cmdWriteOffset) {
        const u8* src = m_cmdTape + offset;
        const auto kind = static_cast<CmdKind>(src[0]);
        const u8 tidOrFlag = src[1];
        u16 payloadBytes = 0U;
        EntityId id{};
        EntityId auxId{};
        std::memcpy(&payloadBytes, src + 2U, 2U);
        std::memcpy(&id, src + 4U, 4U);
        std::memcpy(&auxId, src + 8U, 4U);
        const void* payload = src + 12U;
        offset += 12U + static_cast<usize>(payloadBytes);

        switch (kind) {
            case CmdKind::Destroy:
                (void)Destroy(id, tidOrFlag != 0U);
                break;
            case CmdKind::Emplace:
                (void)EmplaceRawByTypeId(id, tidOrFlag, payload);
                break;
            case CmdKind::Remove:
                if (IsAlive(id) && tidOrFlag < kMaxComponentTypes &&
                    (m_registeredPoolsMask & (1ULL << tidOrFlag)) != 0ULL) {
                    m_pools[tidOrFlag].RemoveUnchecked(id);
                    m_componentMasks[id.index] &= ~(1ULL << tidOrFlag);
                }
                break;
            case CmdKind::SetParent:
                (void)SetParent(id, auxId, tidOrFlag != 0U);
                break;
        }
    }
    m_cmdWriteOffset = 0U;
    m_pendingCmdCount = 0U;
    return {};
}

void World::BeginTick(const SimStepView& step) noexcept {
    const usize wordCount = std::min<usize>(
        m_aliveWordCount, (static_cast<usize>(m_maxAllocatedIndex) + 64U) >> 6U);
    for (usize w = 0U; w < wordCount; ++w) {
        m_teleportedWords[w] = 0ULL;
    }
    m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
    m_transforms.SnapshotPreviousTransforms(AliveWords(), m_timeChannels, step.frozenMask, m_maxAllocatedIndex);
    ++m_tickCount;
}

void World::StepAmbientVelocities(const SimStepView& step) noexcept {
    if ((m_registeredPoolsMask & (1ULL << Velocity2D::kTypeId)) == 0ULL) return;
    auto& velPool = GetPool<Velocity2D>();
    const auto entities = velPool.Entities();
    auto vels = velPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const u16 idx = entities[i].index;
        const auto ch = static_cast<TimeChannel>(m_timeChannels[idx]);
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        Velocity2D& vel = vels[i];
        if (vel.linearDamping > 0.0f) {
            const f32 damp = std::max(0.0f, 1.0f - vel.linearDamping * dt);
            vel.vx *= damp;
            vel.vy *= damp;
        }
        LocalTransform2D& loc = ModifyLocalTransformByIndex(idx);
        loc.x += vel.vx * dt;
        loc.y += vel.vy * dt;
        loc.rotationRad += vel.angularVelRad * dt;
    }
}

void World::StepAnimations(const SimStepView& step) noexcept {
    StepAnimationsAndLifetimes(step);
}

void World::StepAnimationsAndLifetimes(const SimStepView& step) noexcept {
    if ((m_registeredPoolsMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
        auto& animPool = GetPool<SpriteAnimator2D>();
        const auto entities = animPool.Entities();
        auto anims = animPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            SpriteAnimator2D& anim = anims[i];
            if (!anim.playing || anim.frameCount <= 1U) continue;
            const auto ch = static_cast<TimeChannel>(m_timeChannels[entities[i].index]);
            if (step.IsFrozen(ch)) continue;
            const u16 threshold = std::max<u16>(1U, anim.ticksPerFrame);
            if (++anim.tickAccum >= threshold) {
                anim.tickAccum = 0U;
                if (anim.currentFrame + 1U < anim.frameCount) {
                    ++anim.currentFrame;
                } else if (anim.loop) {
                    anim.currentFrame = 0U;
                } else {
                    anim.playing = false;
                }
            }
        }
    }

    if ((m_registeredPoolsMask & (1ULL << Lifetime2D::kTypeId)) != 0ULL) {
        auto& lifePool = GetPool<Lifetime2D>();
        auto& spritePool = GetPool<Sprite2D>();
        const auto entities = lifePool.Entities();
        auto lives = lifePool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            Lifetime2D& life = lives[i];
            if (!life.active) continue;
            const EntityId id = entities[i];
            const auto ch = static_cast<TimeChannel>(m_timeChannels[id.index]);
            if (step.IsFrozen(ch)) continue;
            if (life.remainingTicks > 0U) --life.remainingTicks;
            if (life.fadeSpriteAlpha && life.initialTicks > 0U) {
                life.alphaScale = static_cast<f32>(life.remainingTicks) / static_cast<f32>(life.initialTicks);
                if ((m_componentMasks[id.index] & (1ULL << Sprite2D::kTypeId)) != 0ULL) {
                    spritePool.DirectAtEntityIndex(id.index).tint.a = life.alphaScale;
                }
            }
            if (life.remainingTicks == 0U) {
                (void)DeferDestroy(id);
            }
        }
    }

    if ((m_registeredPoolsMask & (1ULL << PointLight2D::kTypeId)) != 0ULL) {
        auto& lightPool = GetPool<PointLight2D>();
        const auto entities = lightPool.Entities();
        auto lights = lightPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            PointLight2D& light = lights[i];
            const auto ch = static_cast<TimeChannel>(m_timeChannels[entities[i].index]);
            if (!step.IsFrozen(ch) && light.pulseSpeed != 0.0f) {
                light.pulsePhase += light.pulseSpeed * step.Dt(ch);
            }
        }
    }
}

void World::EndTick(const SimStepView& /*step*/) noexcept {
    (void)FlushCommands();
    m_transforms.PropagateHierarchy(AliveWords(), m_maxAllocatedIndex);
}

Result<EntityId, WorldErrc> World::InstantiatePrefab(
    const AssetBank& bank,
    BlobHandle prefabBlob,
    const LocalTransform2D& rootTransform,
    std::span<EntityId> outSpawnedEntities) noexcept {
    const BlobRecord& blob = bank.GetBlob(prefabBlob);
    if (!blob.IsValid() || blob.size < sizeof(CookedPrefabHeader)) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    BinaryReader reader(blob.Bytes());
    CookedPrefabHeader hdr{};
    if (!reader.Read(hdr) || hdr.magic != kCookedPrefabMagic ||
        hdr.entityCount == 0U || hdr.entityCount > 128U) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    std::array<EntityId, 128> spawned{};
    for (u16 i = 0U; i < hdr.entityCount; ++i) {
        CookedPrefabEntityRecord rec{};
        if (!reader.Read(rec)) return Err(WorldErrc::CorruptedPrefab);

        EntitySpawnDesc sdesc{};
        sdesc.local = (i == 0U) ? rootTransform : rec.local;
        sdesc.timeChannel = static_cast<TimeChannel>(rec.timeChannel);
        if (rec.parentEntityIndex >= 0 && static_cast<u16>(rec.parentEntityIndex) < i) {
            sdesc.parent = spawned[static_cast<usize>(rec.parentEntityIndex)];
        }

        auto entRes = Spawn(sdesc);
        if (!entRes) return entRes;
        spawned[i] = *entRes;
        if (i < outSpawnedEntities.size()) {
            outSpawnedEntities[i] = *entRes;
        }

        for (u8 c = 0U; c < rec.componentCount; ++c) {
            u8 tid = 0U;
            u16 compSize = 0U;
            if (!reader.Read(tid) || !reader.Read(compSize) || reader.Remaining() < compSize) {
                return Err(WorldErrc::CorruptedPrefab);
            }
            const void* rawPtr = blob.data + reader.Offset();
            // Advance reader directly without copying into a temporary stack buffer
            std::string_view skipped{};
            (void)skipped;
            for (u16 b = 0U; b < compSize; ++b) {
                u8 dummy = 0U;
                (void)reader.Read(dummy);
            }
            (void)EmplaceRawByTypeId(*entRes, tid, rawPtr);
        }
    }

    return spawned[0];
}

Result<usize, WorldErrc> World::SerializePrefabTemplate(
    EntityId rootEntity,
    std::span<u8> outBuffer) const noexcept {
    if (!IsAlive(rootEntity)) return Err(WorldErrc::DeadEntity);

    // Traverse the full hierarchy rooted at rootEntity using TransformSolver::TraverseSubtree
    std::array<u16, 128> nodes{};
    std::array<i16, 128> parentMap{};
    usize nodeCount = 0U;

    const u16 rootIdx = rootEntity.index;
    m_transforms.TraverseSubtree(rootIdx, m_maxEntities, [&](u16 cur) noexcept {
        if (nodeCount >= nodes.size() || m_generations[cur] == 0U) return;
        const u16 curParent = m_transforms.ParentOf(cur);
        i16 mappedParent = -1;
        if (cur != rootIdx && curParent != kInvalidDenseIndex) {
            for (usize p = 0U; p < nodeCount; ++p) {
                if (nodes[p] == curParent) {
                    mappedParent = static_cast<i16>(p);
                    break;
                }
            }
        }
        nodes[nodeCount] = cur;
        parentMap[nodeCount] = mappedParent;
        ++nodeCount;
    });

    BinaryWriter writer(outBuffer);
    u32 totalComponentRecords = 0U;
    for (usize n = 0U; n < nodeCount; ++n) {
        totalComponentRecords += static_cast<u32>(std::popcount(m_componentMasks[nodes[n]]));
    }

    CookedPrefabHeader hdr{};
    hdr.magic = kCookedPrefabMagic;
    hdr.version = 1U;
    hdr.entityCount = static_cast<u16>(nodeCount);
    hdr.componentRecordCount = totalComponentRecords;
    hdr.schemaCrc32 = 0U;
    if (!writer.Write(hdr)) return Err(WorldErrc::OutOfMemory);

    for (usize n = 0U; n < nodeCount; ++n) {
        const u16 idx = nodes[n];
        u64 mask = m_componentMasks[idx];

        CookedPrefabEntityRecord rec{};
        rec.local = m_transforms.Local(idx);
        rec.parentEntityIndex = parentMap[n];
        rec.timeChannel = m_timeChannels[idx];
        rec.componentCount = static_cast<u8>(std::popcount(mask));
        rec.entityFlags = 0U;
        if (!writer.Write(rec)) return Err(WorldErrc::OutOfMemory);

        while (mask != 0ULL) {
            const u8 tid = static_cast<u8>(std::countr_zero(mask));
            mask &= (mask - 1ULL);
            u16 elemBytes = 0U;
            const void* rawComp = m_pools[tid].GetRawByEntityIndex(idx, elemBytes);
            if (!writer.Write(tid) || !writer.Write(elemBytes) ||
                !writer.WriteBytes(std::span<const u8>(static_cast<const u8*>(rawComp), elemBytes))) {
                return Err(WorldErrc::OutOfMemory);
            }
        }
    }
    return writer.Offset();
}

u64 World::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_aliveCount) | (static_cast<u64>(m_maxAllocatedIndex) << 32U));
    const usize wordCount = std::min<usize>(
        m_aliveWordCount, (static_cast<usize>(m_maxAllocatedIndex) + 64U) >> 6U);
    for (usize w = 0U; w < wordCount; ++w) {
        u64 alive = m_aliveWords[w];
        while (alive != 0ULL) {
            const u16 bit = static_cast<u16>(std::countr_zero(alive));
            alive &= (alive - 1ULL);
            const u16 i = static_cast<u16>((w << 6U) + bit);
            h = HashCombine64(h, m_componentMasks[i] ^ (static_cast<u64>(m_generations[i]) << 48U));
            const WorldTransform2D& wt = m_transforms.World(i);
            h = HashCombine64(
                h,
                static_cast<u64>(std::bit_cast<u32>(wt.x)) |
                    (static_cast<u64>(std::bit_cast<u32>(wt.y)) << 32U));
        }
    }
    return h;
}

} // namespace en

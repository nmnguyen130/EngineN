#include "world/World.h"

#include "assets/AssetBank.h"
#include "core/Diagnostics.h"

namespace en {

namespace {

[[nodiscard]] bool VerifyPacketCrc32(std::span<const u8> bytes, u32 expectedCrc) noexcept {
    const u32 hdrCrc = ComputeCrc32(bytes.subspan(0U, 12U));
    return ComputeCrc32(bytes.subspan(16U), hdrCrc) == expectedCrc;
}

template <typename HeaderT>
usize StampPacketCrc32(
    std::span<u8> outBuffer,
    usize totalBytes,
    HeaderT& hdr,
    u32& crcField) noexcept {
    const u32 hdrCrc = ComputeCrc32({outBuffer.data(), 12U});
    crcField = ComputeCrc32(
        {outBuffer.data() + sizeof(HeaderT), totalBytes - sizeof(HeaderT)}, hdrCrc);
    std::memcpy(outBuffer.data(), &hdr, sizeof(HeaderT));
    return totalBytes;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

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
    auto poolsSpan = m_arena.Carve<RawComponentPool>(kMaxComponentTypes);
    auto maskSpan = m_arena.Carve<u64>(cfg.maxEntities);
    auto bitSpan = m_arena.Carve<u64>(wordCount * 2U);
    auto genSpan = m_arena.Carve<u16>(cfg.maxEntities);
    auto chSpan = m_arena.Carve<u8>(cfg.maxEntities);
    auto cmdSpan = m_arena.Carve<u8>(cfg.commandBufferBytes);

    if (poolsSpan.empty() || maskSpan.empty() || bitSpan.empty() || genSpan.empty() ||
        chSpan.empty() || cmdSpan.empty() || !m_transforms.Init(m_arena, cfg.maxEntities)) {
        Shutdown();
        return Err(WorldErrc::OutOfMemory);
    }

    m_pools = poolsSpan.data();
    m_componentMasks = maskSpan.data();
    m_aliveWords = bitSpan.data();
    m_teleportedWords = m_aliveWords + wordCount;
    m_generations = genSpan.data();
    m_timeChannels = chSpan.data();
    m_cmdTape = cmdSpan.data();

    m_maxEntities = cfg.maxEntities;
    m_defaultPoolCap = cfg.defaultPoolCapacity;
    m_cmdCapacity = cfg.commandBufferBytes;
    m_aliveWordCount = static_cast<u16>(wordCount);

    if (!RegisterComponentPool<Velocity2D>() || !RegisterComponentPool<Sprite2D>() ||
        !RegisterComponentPool<SpriteAnimator2D>() || !RegisterComponentPool<CameraRig2D>(64U) ||
        !RegisterComponentPool<Lifetime2D>() || !RegisterComponentPool<PointLight2D>(256U) ||
        !RegisterComponentPool<Tag2D>()) {
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
    m_pools = nullptr;
    m_registeredPoolsMask = 0ULL;
    m_aliveCount = 0U;
    m_maxEntities = 0U;
    m_maxAllocatedIndex = 0U;
    m_nextFreshSlot = 1U;
    m_freeHead = kInvalidDenseIndex;
    m_freeTail = kInvalidDenseIndex;
    m_defaultPoolCap = 0U;
    m_transforms = {};
    m_cmdTape = nullptr;
    m_teleportedWords = nullptr;
    m_cmdCapacity = 0U;
    m_cmdWriteOffset = 0U;
    m_tickCount = 0U;
    m_aliveWordCount = 0U;
    m_pendingCmdCount = 0U;
    m_spawnedTotalCount = 0ULL;
    m_destroyedTotalCount = 0ULL;
}

void World::Clear() noexcept {
    if (m_maxEntities == 0U) {
        return;
    }
    for (u64 mask = m_registeredPoolsMask; mask != 0ULL; mask &= (mask - 1ULL)) {
        m_pools[std::countr_zero(mask)].Clear();
    }

    const usize limit = static_cast<usize>(m_maxAllocatedIndex) + 1U;
    std::fill_n(m_generations, limit, u16{0});
    std::fill_n(m_componentMasks, limit, 0ULL);
    std::fill_n(m_timeChannels, limit, u8{0});

    const usize activeWords = ActiveWordCount();
    std::fill_n(m_aliveWords, activeWords, 0ULL);
    std::fill_n(m_teleportedWords, activeWords, 0ULL);

    m_transforms.Clear(m_maxAllocatedIndex);
    m_maxAllocatedIndex = 0U;
    m_nextFreshSlot = 1U;
    m_freeHead = kInvalidDenseIndex;
    m_freeTail = kInvalidDenseIndex;
    m_aliveCount = 0U;
    m_cmdWriteOffset = 0U;
    m_pendingCmdCount = 0U;
}

// ---------------------------------------------------------------------------
// Entity allocation / free list
// ---------------------------------------------------------------------------

// Free-list packed into m_componentMasks while dead: low16 = next free, mid16 = next generation.
void World::PushFreeSlot(u16 idx, u16 nextGen) noexcept {
    m_componentMasks[idx] =
        static_cast<u64>(kInvalidDenseIndex) | (static_cast<u64>(nextGen) << 16U);
    if (m_freeTail != kInvalidDenseIndex) {
        m_componentMasks[m_freeTail] =
            (m_componentMasks[m_freeTail] & ~0xFFFFULL) | static_cast<u64>(idx);
    } else {
        m_freeHead = idx;
    }
    m_freeTail = idx;
}

Result<EntityId, WorldErrc> World::Spawn(const EntitySpawnDesc& desc) noexcept {
    if (m_maxEntities == 0U) {
        return Err(WorldErrc::InvalidConfig);
    }

    u16 parentIdx = kInvalidDenseIndex;
    if (desc.parent.IsValid()) {
        if (!IsAlive(desc.parent)) {
            return Err(WorldErrc::DeadEntity);
        }
        parentIdx = desc.parent.index;
        usize depth = 1U;
        for (u16 anc = parentIdx; anc != kInvalidDenseIndex; anc = m_transforms.ParentOf(anc)) {
            if (++depth > kMaxHierarchyDepth) {
                return Err(WorldErrc::HierarchyDepthExceeded);
            }
        }
    }

    u16 idx = kInvalidDenseIndex;
    u16 gen = 1U;
    if (m_freeHead != kInvalidDenseIndex) {
        idx = m_freeHead;
        const u64 packed = m_componentMasks[idx];
        m_freeHead = static_cast<u16>(packed & 0xFFFFU);
        if (m_freeHead == kInvalidDenseIndex) {
            m_freeTail = kInvalidDenseIndex;
        }
        gen = static_cast<u16>((packed >> 16U) & 0x7FFFU);
        if (gen == 0U) {
            gen = 1U;
        }
    } else {
        if (m_nextFreshSlot >= m_maxEntities) {
            return Err(WorldErrc::EntityCapacityReached);
        }
        idx = m_nextFreshSlot++;
    }

    m_maxAllocatedIndex = std::max(m_maxAllocatedIndex, idx);
    m_generations[idx] = gen;
    m_componentMasks[idx] = 0ULL;
    m_timeChannels[idx] = static_cast<u8>(desc.timeChannel);
    const u64 bit = (1ULL << (idx & 63U));
    m_aliveWords[idx >> 6U] |= bit;
    m_teleportedWords[idx >> 6U] &= ~bit;
    ++m_aliveCount;
    ++m_spawnedTotalCount;

    m_transforms.InitializeSlot(idx, desc.local, parentIdx, desc.keepWorldTransform);
    return EntityId{idx, gen};
}

void World::DestroySingleSlot(EntityId id) noexcept {
    const u16 idx = id.index;
    for (u64 mask = m_componentMasks[idx]; mask != 0ULL; mask &= (mask - 1ULL)) {
        m_pools[std::countr_zero(mask)].RemoveUnchecked(id);
    }
    m_transforms.UnlinkSlot(idx, m_maxEntities);
    const u64 bit = (1ULL << (idx & 63U));
    m_aliveWords[idx >> 6U] &= ~bit;
    m_teleportedWords[idx >> 6U] &= ~bit;

    // Generation wraps 1..0x7FFF (bit 15 reserved for physics tile sentinels).
    const u16 rawNext = static_cast<u16>((id.generation + 1U) & 0x7FFFU);
    m_generations[idx] = 0U;
    PushFreeSlot(idx, (rawNext == 0U) ? 1U : rawNext);

    --m_aliveCount;
    ++m_destroyedTotalCount;
}

bool World::Destroy(EntityId id, bool destroyChildren) noexcept {
    if (!IsAlive(id)) {
        return false;
    }
    if (!destroyChildren || m_transforms.FirstChildOf(id.index) == kInvalidDenseIndex) {
        DestroySingleSlot(id);
        return true;
    }

    const u16 rootIdx = id.index;
    u16 cur = rootIdx;
    for (usize safety = static_cast<usize>(m_maxEntities) * 2U; safety > 0U; --safety) {
        const u16 firstChild = m_transforms.FirstChildOf(cur);
        if (firstChild != kInvalidDenseIndex) {
            cur = firstChild;
            continue;
        }
        const u16 nextSib =
            (cur == rootIdx) ? kInvalidDenseIndex : m_transforms.NextSiblingOf(cur);
        const u16 parent =
            (cur == rootIdx) ? kInvalidDenseIndex : m_transforms.ParentOf(cur);
        if (m_generations[cur] != 0U) {
            DestroySingleSlot({cur, m_generations[cur]});
        }
        if (cur == rootIdx) {
            break;
        }
        cur = (nextSib != kInvalidDenseIndex) ? nextSib : parent;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Clone / subtree
// ---------------------------------------------------------------------------

usize World::CollectSubtree(
    u16 rootIdx,
    bool includeChildren,
    std::span<u16> outNodes,
    std::span<i16> outParentMap) const noexcept {
    if (!includeChildren || m_transforms.FirstChildOf(rootIdx) == kInvalidDenseIndex) {
        outNodes[0] = rootIdx;
        outParentMap[0] = -1;
        return 1U;
    }

    usize count = 0U;
    bool overflow = false;
    m_transforms.TraverseSubtree(rootIdx, m_maxEntities, [&](u16 cur) noexcept {
        if (m_generations[cur] == 0U) {
            return;
        }
        if (count >= outNodes.size()) {
            overflow = true;
            return;
        }
        i16 mappedParent = -1;
        if (cur != rootIdx) {
            const u16 curParent = m_transforms.ParentOf(cur);
            for (usize p = 0U; p < count; ++p) {
                if (outNodes[p] == curParent) {
                    mappedParent = static_cast<i16>(p);
                    break;
                }
            }
        }
        outNodes[count] = cur;
        outParentMap[count] = mappedParent;
        ++count;
    });
    return overflow ? 0U : count;
}

Result<EntityId, WorldErrc> World::CloneEntity(
    EntityId source,
    const LocalTransform2D& overrideLocal,
    bool cloneChildren) noexcept {
    if (!IsAlive(source)) {
        return Err(WorldErrc::DeadEntity);
    }

    constexpr usize kMaxSubtree = 512U;
    std::array<u16, kMaxSubtree> srcNodes{};
    std::array<i16, kMaxSubtree> parentMap{};
    std::array<EntityId, kMaxSubtree> cloned{};
    const usize nodeCount = CollectSubtree(source.index, cloneChildren, srcNodes, parentMap);
    if (nodeCount == 0U) {
        return Err(WorldErrc::EntityCapacityReached);
    }

    for (usize n = 0U; n < nodeCount; ++n) {
        const u16 srcIdx = srcNodes[n];
        EntitySpawnDesc desc{
            .local = (n == 0U) ? overrideLocal : m_transforms.Local(srcIdx),
            .parent = (n == 0U)
                          ? GetParent(source)
                          : (parentMap[n] >= 0 ? cloned[static_cast<usize>(parentMap[n])]
                                               : kNullEntity),
            .timeChannel = static_cast<TimeChannel>(m_timeChannels[srcIdx]),
        };

        auto cloneRes = Spawn(desc);
        if (!cloneRes) {
            for (usize k = n; k > 0U; --k) {
                DestroySingleSlot(cloned[k - 1U]);
            }
            return cloneRes;
        }
        cloned[n] = *cloneRes;

        for (u64 mask = m_componentMasks[srcIdx]; mask != 0ULL; mask &= (mask - 1ULL)) {
            const u8 tid = static_cast<u8>(std::countr_zero(mask));
            if (!m_pools[tid].CloneFromEntityIndex(*cloneRes, srcIdx)) {
                for (usize k = n + 1U; k > 0U; --k) {
                    DestroySingleSlot(cloned[k - 1U]);
                }
                return Err(WorldErrc::PoolCapacityReached);
            }
            m_componentMasks[cloneRes->index] |= (1ULL << tid);
        }
    }
    return cloned[0];
}

// ---------------------------------------------------------------------------
// Component helpers / commands
// ---------------------------------------------------------------------------

bool World::WriteEntityComponents(BinaryWriter& writer, u16 idx) const noexcept {
    for (u64 m = m_componentMasks[idx]; m != 0ULL; m &= (m - 1ULL)) {
        const u8 tid = static_cast<u8>(std::countr_zero(m));
        const u16 elemBytes = m_pools[tid].ElemBytes();
        if (!writer.Write(tid) || !writer.Write(elemBytes) ||
            !writer.WriteBytes({m_pools[tid].RawAtEntityIndex(idx), elemBytes})) {
            return false;
        }
    }
    return true;
}

bool World::EmplaceRawByTypeId(
    EntityId id,
    u8 typeId,
    const void* rawData,
    u16 expectedElemBytes) noexcept {
    if (!IsAlive(id) || typeId >= kMaxComponentTypes || !rawData ||
        (m_registeredPoolsMask & (1ULL << typeId)) == 0ULL) {
        return false;
    }
    if (expectedElemBytes != 0U && expectedElemBytes != m_pools[typeId].ElemBytes()) {
        return false;
    }
    if (!m_pools[typeId].EmplaceRaw(id, rawData)) {
        return false;
    }
    m_componentMasks[id.index] |= (1ULL << typeId);
    return true;
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
    if (payloadBytes > 0U && payload) {
        std::memcpy(dst + 12U, payload, payloadBytes);
    }
    m_cmdWriteOffset += static_cast<u32>(totalBytes);
    ++m_pendingCmdCount;
    return {};
}

Result<void, WorldErrc> World::FlushCommands() noexcept {
    using namespace en::literals;
    const usize endOffset = m_cmdWriteOffset;
    m_cmdWriteOffset = 0U;
    m_pendingCmdCount = 0U;
    usize offset = 0U;
    WorldErrc firstErr = WorldErrc::None;

    while (offset + 12U <= endOffset) {
        const u8* src = m_cmdTape + offset;
        const auto kind = static_cast<CmdKind>(src[0]);
        const u8 tidOrFlag = src[1];
        u16 payloadBytes = 0U;
        EntityId id{};
        EntityId auxId{};
        std::memcpy(&payloadBytes, src + 2U, 2U);
        std::memcpy(&id, src + 4U, 4U);
        std::memcpy(&auxId, src + 8U, 4U);
        const void* payload = (payloadBytes > 0U) ? (src + 12U) : nullptr;
        offset += 12U + static_cast<usize>(payloadBytes);

        switch (kind) {
            case CmdKind::Destroy:
                if (!Destroy(id, tidOrFlag != 0U) && firstErr == WorldErrc::None) {
                    firstErr = WorldErrc::DeadEntity;
                }
                break;
            case CmdKind::Emplace:
                if (!EmplaceRawByTypeId(id, tidOrFlag, payload, payloadBytes) &&
                    firstErr == WorldErrc::None) {
                    firstErr = WorldErrc::PoolCapacityReached;
                    Logger::Log(
                        LogLevel::Warn, "world/cmd"_sid,
                        "Deferred Emplace failed during FlushCommands");
                }
                break;
            case CmdKind::Remove:
                if (IsAlive(id) && tidOrFlag < kMaxComponentTypes &&
                    (m_registeredPoolsMask & (1ULL << tidOrFlag)) != 0ULL) {
                    m_pools[tidOrFlag].RemoveUnchecked(id);
                    m_componentMasks[id.index] &= ~(1ULL << tidOrFlag);
                }
                break;
            case CmdKind::SetParent:
                if (auto r = SetParent(id, auxId, tidOrFlag != 0U);
                    !r && firstErr == WorldErrc::None) {
                    firstErr = r.error();
                    Logger::Log(
                        LogLevel::Warn, "world/cmd"_sid,
                        "Deferred SetParent failed during FlushCommands");
                }
                break;
        }
    }
    return (firstErr == WorldErrc::None) ? Result<void, WorldErrc>{} : Err(firstErr);
}

// ---------------------------------------------------------------------------
// Prefab
// ---------------------------------------------------------------------------

Result<EntityId, WorldErrc> World::InstantiatePrefab(
    const AssetBank& bank,
    BlobHandle prefabBlob,
    const LocalTransform2D& rootTransform,
    std::span<EntityId> outSpawnedEntities) noexcept {
    const BlobRecord& blob = bank.GetBlob(prefabBlob);
    if (!blob.data || blob.size < sizeof(CookedPrefabHeader)) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    const std::span<const u8> bytes = blob.Bytes();
    BinaryReader reader(bytes);
    CookedPrefabHeader hdr{};
    if (!reader.Read(hdr) || hdr.magic != kCookedPrefabMagic || hdr.version != 1U ||
        hdr.entityCount == 0U || hdr.entityCount > 512U ||
        !VerifyPacketCrc32(bytes, hdr.schemaCrc32)) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    std::array<EntityId, 512> spawned{};
    const auto rollback = [&](u16 count) noexcept {
        for (u16 k = count; k > 0U; --k) {
            DestroySingleSlot(spawned[k - 1U]);
        }
        std::fill_n(
            outSpawnedEntities.data(),
            std::min<usize>(count, outSpawnedEntities.size()), kNullEntity);
    };

    u32 parsedComponentRecords = 0U;
    for (u16 i = 0U; i < hdr.entityCount; ++i) {
        CookedPrefabEntityRecord rec{};
        if (!reader.Read(rec) || rec.parentEntityIndex >= static_cast<i16>(i) ||
            rec.parentEntityIndex < -1) {
            rollback(i);
            return Err(WorldErrc::CorruptedPrefab);
        }

        EntitySpawnDesc sdesc{
            .local = (i == 0U) ? rootTransform : rec.local,
            .parent = (rec.parentEntityIndex >= 0)
                          ? spawned[static_cast<usize>(rec.parentEntityIndex)]
                          : kNullEntity,
            .timeChannel = static_cast<TimeChannel>(rec.timeChannel),
        };

        auto entRes = Spawn(sdesc);
        if (!entRes) {
            rollback(i);
            return entRes;
        }
        spawned[i] = *entRes;
        if (i < outSpawnedEntities.size()) {
            outSpawnedEntities[i] = *entRes;
        }

        parsedComponentRecords += rec.componentCount;
        for (u8 c = 0U; c < rec.componentCount; ++c) {
            u8 tid = 0U;
            u16 compSize = 0U;
            if (!reader.Read(tid) || !reader.Read(compSize) || compSize == 0U) {
                rollback(static_cast<u16>(i + 1U));
                return Err(WorldErrc::CorruptedPrefab);
            }
            const void* rawPtr = blob.data + reader.Offset();
            if (!reader.Skip(compSize) || tid >= kMaxComponentTypes ||
                (m_registeredPoolsMask & (1ULL << tid)) == 0ULL ||
                compSize != m_pools[tid].ElemBytes()) {
                rollback(static_cast<u16>(i + 1U));
                return Err(WorldErrc::CorruptedPrefab);
            }
            if (!EmplaceRawByTypeId(*entRes, tid, rawPtr, compSize)) {
                rollback(static_cast<u16>(i + 1U));
                return Err(WorldErrc::PoolCapacityReached);
            }
        }
    }

    if (parsedComponentRecords != hdr.componentRecordCount || reader.Remaining() != 0U) {
        rollback(hdr.entityCount);
        return Err(WorldErrc::CorruptedPrefab);
    }
    return spawned[0];
}

Result<usize, WorldErrc> World::SerializePrefabTemplate(
    EntityId rootEntity,
    std::span<u8> outBuffer) const noexcept {
    if (!IsAlive(rootEntity)) {
        return Err(WorldErrc::DeadEntity);
    }

    constexpr usize kMaxPrefabNodes = 512U;
    std::array<u16, kMaxPrefabNodes> nodes{};
    std::array<i16, kMaxPrefabNodes> parentMap{};
    const usize nodeCount = CollectSubtree(rootEntity.index, true, nodes, parentMap);
    if (nodeCount == 0U) {
        return Err(WorldErrc::EntityCapacityReached);
    }

    BinaryWriter writer(outBuffer);
    u32 totalComponentRecords = 0U;
    for (usize n = 0U; n < nodeCount; ++n) {
        totalComponentRecords += static_cast<u32>(std::popcount(m_componentMasks[nodes[n]]));
    }

    CookedPrefabHeader hdr{
        kCookedPrefabMagic,
        1U,
        static_cast<u16>(nodeCount),
        totalComponentRecords,
        0U,
    };
    if (!writer.Write(hdr)) {
        return Err(WorldErrc::OutOfMemory);
    }

    for (usize n = 0U; n < nodeCount; ++n) {
        const u16 idx = nodes[n];
        const CookedPrefabEntityRecord rec{
            m_transforms.Local(idx),
            parentMap[n],
            m_timeChannels[idx],
            static_cast<u8>(std::popcount(m_componentMasks[idx])),
            0U,
        };
        if (!writer.Write(rec) || !WriteEntityComponents(writer, idx)) {
            return Err(WorldErrc::OutOfMemory);
        }
    }
    return StampPacketCrc32(outBuffer, writer.Offset(), hdr, hdr.schemaCrc32);
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

usize World::RequiredSnapshotBytes() const noexcept {
    if (m_maxEntities == 0U) {
        return sizeof(WorldSnapshotHeader);
    }
    usize total = sizeof(WorldSnapshotHeader);
    ForEachAliveSlot([&](u16 idx) noexcept {
        total += sizeof(EntityId) + sizeof(LocalTransform2D) + sizeof(u16) + sizeof(u8) +
                 sizeof(u8);
        for (u64 mask = m_componentMasks[idx]; mask != 0ULL; mask &= (mask - 1ULL)) {
            total += sizeof(u8) + sizeof(u16) + m_pools[std::countr_zero(mask)].ElemBytes();
        }
    });
    return total;
}

Result<usize, WorldErrc> World::SerializeWorldSnapshot(std::span<u8> outBuffer) const noexcept {
    if (m_maxEntities == 0U || m_aliveCount == 0U) {
        return Err(WorldErrc::InvalidConfig);
    }
    BinaryWriter writer(outBuffer);

    WorldSnapshotHeader hdr{
        kWorldSnapshotMagic,
        1U,
        static_cast<u16>(m_aliveCount),
        m_tickCount,
        0U,
    };
    if (!writer.Write(hdr)) {
        return Err(WorldErrc::OutOfMemory);
    }

    bool ok = true;
    ForEachAliveSlot([&](u16 idx) noexcept {
        if (!ok) {
            return;
        }
        const EntityId id{idx, m_generations[idx]};
        const u16 parentIdx = m_transforms.ParentOf(idx);
        const u8 ch = m_timeChannels[idx];
        const u8 compCount = static_cast<u8>(std::popcount(m_componentMasks[idx]));

        if (!writer.Write(id) || !writer.Write(m_transforms.Local(idx)) ||
            !writer.Write(parentIdx) || !writer.Write(ch) || !writer.Write(compCount) ||
            !WriteEntityComponents(writer, idx)) {
            ok = false;
        }
    });
    if (!ok) {
        return Err(WorldErrc::OutOfMemory);
    }
    return StampPacketCrc32(outBuffer, writer.Offset(), hdr, hdr.payloadCrc32);
}

Result<void, WorldErrc> World::DeserializeWorldSnapshot(std::span<const u8> inBuffer) noexcept {
    if (m_maxEntities == 0U) {
        return Err(WorldErrc::InvalidConfig);
    }
    if (inBuffer.size() < sizeof(WorldSnapshotHeader)) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    BinaryReader reader(inBuffer);
    WorldSnapshotHeader hdr{};
    if (!reader.Read(hdr) || hdr.magic != kWorldSnapshotMagic || hdr.version != 1U ||
        hdr.entityCount == 0U || !VerifyPacketCrc32(inBuffer, hdr.payloadCrc32)) {
        return Err(WorldErrc::CorruptedPrefab);
    }

    Clear();
    m_tickCount = hdr.tickCount;

    for (u16 e = 0U; e < hdr.entityCount; ++e) {
        EntityId id{};
        LocalTransform2D loc{};
        u16 parentIdx = kInvalidDenseIndex;
        u8 ch = 0U;
        u8 compCount = 0U;
        if (!reader.Read(id) || !reader.Read(loc) || !reader.Read(parentIdx) ||
            !reader.Read(ch) || !reader.Read(compCount)) {
            Clear();
            return Err(WorldErrc::CorruptedPrefab);
        }
        if (id.index == 0U || id.index >= m_maxEntities || id.generation == 0U ||
            (parentIdx != kInvalidDenseIndex && parentIdx >= m_maxEntities)) {
            Clear();
            return Err(WorldErrc::CorruptedPrefab);
        }

        m_generations[id.index] = id.generation;
        m_componentMasks[id.index] = 0ULL;
        m_timeChannels[id.index] = ch;
        m_aliveWords[id.index >> 6U] |= (1ULL << (id.index & 63U));
        ++m_aliveCount;
        m_maxAllocatedIndex = std::max(m_maxAllocatedIndex, id.index);
        m_nextFreshSlot = std::max(m_nextFreshSlot, static_cast<u16>(id.index + 1U));
        m_transforms.LinkLoadedSlot(id.index, loc, parentIdx);

        for (u8 c = 0U; c < compCount; ++c) {
            u8 tid = 0U;
            u16 elemBytes = 0U;
            if (!reader.Read(tid) || !reader.Read(elemBytes)) {
                Clear();
                return Err(WorldErrc::CorruptedPrefab);
            }
            const void* rawPtr = inBuffer.data() + reader.Offset();
            if (!reader.Skip(elemBytes) || tid >= kMaxComponentTypes ||
                (m_registeredPoolsMask & (1ULL << tid)) == 0ULL ||
                elemBytes != m_pools[tid].ElemBytes() ||
                (m_componentMasks[id.index] & (1ULL << tid)) != 0ULL) {
                Clear();
                return Err(WorldErrc::CorruptedPrefab);
            }
            if (!EmplaceRawByTypeId(id, tid, rawPtr, elemBytes)) {
                Clear();
                return Err(WorldErrc::PoolCapacityReached);
            }
        }
    }
    if (reader.Remaining() != 0U) {
        Clear();
        return Err(WorldErrc::CorruptedPrefab);
    }

    for (u16 s = 1U; s <= m_maxAllocatedIndex; ++s) {
        if (m_generations[s] == 0U) {
            PushFreeSlot(s, 1U);
            continue;
        }
        usize depth = 0U;
        for (u16 anc = m_transforms.ParentOf(s); anc != kInvalidDenseIndex;
             anc = m_transforms.ParentOf(anc)) {
            if (m_generations[anc] == 0U) {
                Clear();
                return Err(WorldErrc::CorruptedPrefab);
            }
            if (anc == s || ++depth > kMaxHierarchyDepth) {
                Clear();
                return Err(
                    anc == s ? WorldErrc::HierarchyCycle : WorldErrc::HierarchyDepthExceeded);
            }
        }
    }

    PropagateTransforms();
    m_transforms.SnapshotPreviousTransforms(
        AliveWords(), m_timeChannels, 0U, m_maxAllocatedIndex);
    return {};
}

u64 World::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_aliveCount) | (static_cast<u64>(m_maxAllocatedIndex) << 32U));
    ForEachAliveSlot([&](u16 i) noexcept {
        h = HashCombine64(
            h, m_componentMasks[i] ^ (static_cast<u64>(m_generations[i]) << 48U));
        const WorldTransform2D& wt = m_transforms.World(i);
        h = HashCombine64(
            h,
            static_cast<u64>(std::bit_cast<u32>(wt.x)) |
                (static_cast<u64>(std::bit_cast<u32>(wt.y)) << 32U));
    });
    return h;
}

} // namespace en

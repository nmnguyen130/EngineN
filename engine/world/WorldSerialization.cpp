#include "world/World.h"

#include "assets/AssetBank.h"
#include "core/Serialization.h"

namespace en {
namespace {

[[nodiscard]] bool VerifyPacketCrc32(std::span<const u8> bytes, u32 expectedCrc) noexcept {
    const u32 hdrCrc = Crc32(bytes.subspan(0U, 12U));
    return Crc32(bytes.subspan(16U), hdrCrc) == expectedCrc;
}

template <typename HeaderT>
usize StampPacketCrc32(
    std::span<u8> outBuffer,
    usize totalBytes,
    HeaderT& hdr,
    u32& crcField) noexcept {
    const u32 hdrCrc = Crc32({outBuffer.data(), 12U});
    crcField = Crc32(
        {outBuffer.data() + sizeof(HeaderT), totalBytes - sizeof(HeaderT)}, hdrCrc);
    std::memcpy(outBuffer.data(), &hdr, sizeof(HeaderT));
    return totalBytes;
}

} // namespace

bool World::WriteEntityComponents(BinaryWriter& writer, u16 idx) const noexcept {
    for (u64 m = m_componentMasks[idx]; m != 0ULL; m &= (m - 1ULL)) {
        const u8 tid        = static_cast<u8>(std::countr_zero(m));
        const u16 elemBytes = m_pools[tid].ElemBytes();
        if (!writer.Write(tid) || !writer.Write(elemBytes) ||
            !writer.WriteBytes({m_pools[tid].RawAtEntityIndex(idx), elemBytes})) {
            return false;
        }
    }
    return true;
}

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
        for (u16 k = count; k > 0U; --k) DestroySingleSlot(spawned[k - 1U]);
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
            .local  = (i == 0U) ? rootTransform : rec.local,
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
        if (i < outSpawnedEntities.size()) outSpawnedEntities[i] = *entRes;

        parsedComponentRecords += rec.componentCount;
        for (u8 c = 0U; c < rec.componentCount; ++c) {
            u8 tid       = 0U;
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
    if (!IsAlive(rootEntity)) return Err(WorldErrc::DeadEntity);

    constexpr usize kMaxPrefabNodes = 512U;
    std::array<u16, kMaxPrefabNodes> nodes{};
    std::array<i16, kMaxPrefabNodes> parentMap{};
    const usize nodeCount = CollectSubtree(rootEntity.index, true, nodes, parentMap);
    if (nodeCount == 0U) return Err(WorldErrc::EntityCapacityReached);

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
    if (!writer.Write(hdr)) return Err(WorldErrc::OutOfMemory);

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

usize World::RequiredSnapshotBytes() const noexcept {
    if (m_maxEntities == 0U) return sizeof(WorldSnapshotHeader);
    usize total = sizeof(WorldSnapshotHeader);
    ForEachAliveSlot([&](u16 idx) noexcept {
        total += sizeof(EntityId) + sizeof(LocalTransform2D) + sizeof(u16) + sizeof(u8) + sizeof(u8);
        for (u64 mask = m_componentMasks[idx]; mask != 0ULL; mask &= (mask - 1ULL)) {
            total += sizeof(u8) + sizeof(u16) + m_pools[std::countr_zero(mask)].ElemBytes();
        }
    });
    return total;
}

Result<usize, WorldErrc> World::SerializeWorldSnapshot(std::span<u8> outBuffer) const noexcept {
    if (m_maxEntities == 0U || m_aliveCount == 0U) return Err(WorldErrc::InvalidConfig);
    BinaryWriter writer(outBuffer);

    WorldSnapshotHeader hdr{
        kWorldSnapshotMagic,
        1U,
        static_cast<u16>(m_aliveCount),
        m_tickCount,
        0U,
    };
    if (!writer.Write(hdr)) return Err(WorldErrc::OutOfMemory);

    bool ok = true;
    ForEachAliveSlot([&](u16 idx) noexcept {
        if (!ok) return;
        const EntityId id{idx, m_generations[idx]};
        const u16 parentIdx = m_transforms.ParentOf(idx);
        const u8 ch         = m_timeChannels[idx];
        const u8 compCount  = static_cast<u8>(std::popcount(m_componentMasks[idx]));

        if (!writer.Write(id) || !writer.Write(m_transforms.Local(idx)) ||
            !writer.Write(parentIdx) || !writer.Write(ch) || !writer.Write(compCount) ||
            !WriteEntityComponents(writer, idx)) {
            ok = false;
        }
    });
    if (!ok) return Err(WorldErrc::OutOfMemory);
    return StampPacketCrc32(outBuffer, writer.Offset(), hdr, hdr.payloadCrc32);
}

Result<void, WorldErrc> World::DeserializeWorldSnapshot(std::span<const u8> inBuffer) noexcept {
    if (m_maxEntities == 0U) return Err(WorldErrc::InvalidConfig);
    if (inBuffer.size() < sizeof(WorldSnapshotHeader)) return Err(WorldErrc::CorruptedPrefab);

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
        u8 ch         = 0U;
        u8 compCount  = 0U;
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

        m_generations[id.index]    = id.generation;
        m_componentMasks[id.index] = 0ULL;
        m_timeChannels[id.index]   = ch;
        m_aliveWords[id.index >> 6U] |= (1ULL << (id.index & 63U));
        ++m_aliveCount;
        m_maxAllocatedIndex = std::max(m_maxAllocatedIndex, id.index);
        m_nextFreshSlot     = std::max(m_nextFreshSlot, static_cast<u16>(id.index + 1U));
        m_transforms.LinkLoadedSlot(id.index, loc, parentIdx);

        for (u8 c = 0U; c < compCount; ++c) {
            u8 tid        = 0U;
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
                return Err(anc == s ? WorldErrc::HierarchyCycle : WorldErrc::HierarchyDepthExceeded);
            }
        }
    }

    PropagateTransforms();
    m_transforms.SnapshotPreviousTransforms(
        AliveWords(), m_timeChannels, 0U, m_maxAllocatedIndex);
    return {};
}

} // namespace en

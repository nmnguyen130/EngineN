#include "tests/TestRunner.h"
#include "assets/AssetSystem.h"
#include "core/Serialization.h"
#include "world/World.h"
#include <vector>

namespace en::tests {

void RunSuiteAssetsAndWorld(SuiteStats& stats) noexcept {
    using namespace en::literals;

    // 1. AssetBank & AssetSystem (.enpak mount, scope pinning, blob hot-reload, eviction)
    AssetSystem assets{};
    EN_CHECK(stats, assets.Init().has_value());
    auto scopeRes = assets.BeginScope();
    EN_CHECK(stats, scopeRes.has_value());
    const ScopeId sc = *scopeRes;

    // Build an in-memory .enpak with 1 Blob entry
    const std::array<u8, 16> blobData{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::array<u8, 256> pakBuf{};
    BinaryWriter pw{pakBuf};
    EnPakHeader hdr{kEnPakMagic, 1, 1, 0};
    EnPakEntry ent{};
    ent.assetId = ("configs/test.blob"_aid).value;
    ent.offset = sizeof(EnPakHeader) + sizeof(EnPakEntry);
    ent.compressedSize = static_cast<u32>(blobData.size());
    ent.uncompressedSize = static_cast<u32>(blobData.size());
    ent.kind = AssetKind::Blob;
    (void)pw.Write(hdr);
    (void)pw.Write(ent);
    (void)pw.WriteBytes(blobData);

    EN_CHECK(stats, assets.MountPackMemory(pw.WrittenSpan(), 10).has_value());
    const BlobHandle bh = assets.PinBlob(sc, "configs/test.blob"_aid);
    EN_CHECK(stats, bh.IsValid());
    const BlobRecord& rec = assets.Bank().GetBlob(bh);
    EN_CHECK(stats, rec.IsValid() && rec.size == 16U && rec.Bytes()[3] == 4U);

    const std::array<u8, 8> updatedBlob{99, 98, 97, 96, 95, 94, 93, 92};
    EN_CHECK(stats, assets.Bank().HotReloadBlob("configs/test.blob"_aid, updatedBlob));
    EN_CHECK(stats, assets.Bank().GetBlob(bh).size == 8U && assets.Bank().GetBlob(bh).Bytes()[0] == 99U);

    assets.EndScope(sc);
    assets.Shutdown();

    // 2. World ECS: Spawn, Parent-Child Transform Hierarchy, ComponentPool, Snapshot Save/Restore, Deferred Destroy
    World world{};
    EN_CHECK(stats, world.Init().has_value());

    EntitySpawnDesc pDesc{};
    pDesc.local.x = 100.0f;
    pDesc.local.y = 50.0f;
    auto parentRes = world.Spawn(pDesc);
    EN_CHECK(stats, parentRes.has_value());
    const EntityId parent = *parentRes;
    (void)world.Emplace<Tag2D>(parent, Tag2D{"ParentNode"_sid, 1U});

    EntitySpawnDesc cDesc{};
    cDesc.local.x = 25.0f;
    cDesc.local.y = 15.0f;
    cDesc.parent = parent;
    auto childRes = world.Spawn(cDesc);
    EN_CHECK(stats, childRes.has_value());
    const EntityId child = *childRes;

    TimePipeline tp{};
    tp.BeginFrameManual(kFixedDtNs);
    const SimStepView step = tp.PollTick();
    world.BeginTick(step);
    world.EndTick(step);

    const WorldTransform2D& childWt = world.GetWorldByIndex(child.index);
    EN_CHECK(stats, std::abs(childWt.x - 125.0f) < 1e-4f && std::abs(childWt.y - 65.0f) < 1e-4f);
    EN_CHECK(stats, world.FindByTag("ParentNode"_sid) == parent);

    auto clonedRes = world.CloneEntity(parent, LocalTransform2D{200.0f, 80.0f});
    EN_CHECK(stats, clonedRes.has_value() && world.IsAlive(*clonedRes));
    world.SetLocalTransform(parent, LocalTransform2D{110.0f, 55.0f});
    EN_CHECK(stats, std::abs(world.GetLocalTransform(parent).x - 110.0f) < 1e-4f);

    // 3. Test 3-level dirty-tree parent -> child -> grandchild propagation
    EntitySpawnDesc gcDesc{};
    gcDesc.local.x = 5.0f;
    gcDesc.local.y = 5.0f;
    gcDesc.parent = child;
    const EntityId grandchild = *world.Spawn(gcDesc);
    world.PropagateTransforms();
    EN_CHECK(stats, std::abs(world.GetWorldByIndex(grandchild.index).x - 140.0f) < 1e-4f);

    // Move root parent; MarkSubtreeDirty cascades down to child and grandchild
    world.SetLocalTransform(parent, LocalTransform2D{200.0f, 100.0f});
    world.PropagateTransforms();
    EN_CHECK(stats, std::abs(world.GetWorldByIndex(grandchild.index).x - 230.0f) < 1e-4f);
    EN_CHECK(stats, std::abs(world.GetWorldByIndex(grandchild.index).y - 120.0f) < 1e-4f);

    // 4. PrefabTemplate serializer round-trip
    std::array<u8, 2048> prefabBytes{};
    auto pfLen = world.SerializePrefabTemplate(parent, prefabBytes);
    EN_CHECK(stats, pfLen.has_value() && *pfLen > sizeof(CookedPrefabHeader));

    // 5. XOR payload cipher round-trip
    std::array<u8, 8> secret{1, 2, 3, 4, 5, 6, 7, 8};
    AssetSystem::XorCipherPayload(secret, 12345ULL);
    EN_CHECK(stats, secret[0] != 1U);
    AssetSystem::XorCipherPayload(secret, 12345ULL);
    EN_CHECK(stats, secret[0] == 1U && secret[7] == 8U);

    (void)world.DeferDestroy(parent);
    world.EndTick(step);
    EN_CHECK(stats, !world.IsAlive(parent) && world.IsAlive(child));
    world.Shutdown();
}

} // namespace en::tests

#include "world/WorldExtractor.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace en::tests {

struct alignas(8) CustomStatsComponent {
    static constexpr u8 kTypeId = 24;
    i32 attackPower{10};
    i32 defensePower{5};
};
static_assert(sizeof(CustomStatsComponent) == 8);

[[nodiscard]] constexpr bool VerifyConstexprWorldLayer1() noexcept {
    // 1. Verify exact byte layouts and zero-padding invariants
    static_assert(sizeof(EntityId) == 4 && alignof(EntityId) == 4);
    static_assert(sizeof(WorldConfig) == 12 && alignof(WorldConfig) == 4);
    static_assert(sizeof(LocalTransform2D) == 24 && alignof(LocalTransform2D) == 4);
    static_assert(sizeof(PrevWorldSnapshot2D) == 24 && alignof(PrevWorldSnapshot2D) == 4);
    static_assert(sizeof(WorldTransform2D) == 32 && alignof(WorldTransform2D) == 32);
    static_assert(sizeof(EntitySpawnDesc) == 32 && alignof(EntitySpawnDesc) == 4);
    static_assert(sizeof(Velocity2D) == 16 && alignof(Velocity2D) == 16);
    static_assert(sizeof(Sprite2D) == 32 && alignof(Sprite2D) == 32);
    static_assert(sizeof(SpriteAnimator2D) == 16 && alignof(SpriteAnimator2D) == 16);
    static_assert(sizeof(CameraRig2D) == 32 && alignof(CameraRig2D) == 32);
    static_assert(sizeof(Lifetime2D) == 16 && alignof(Lifetime2D) == 16);
    static_assert(sizeof(PointLight2D) == 32 && alignof(PointLight2D) == 32);
    static_assert(sizeof(Tag2D) == 16 && alignof(Tag2D) == 16);
    static_assert(sizeof(CookedPrefabHeader) == 16 && alignof(CookedPrefabHeader) == 16);
    static_assert(sizeof(CookedPrefabEntityRecord) == 32 && alignof(CookedPrefabEntityRecord) == 8);
    static_assert(sizeof(RawComponentPool) == 32 && alignof(RawComponentPool) == 32);
    static_assert(sizeof(ComponentPool<Velocity2D>) == 32 && alignof(ComponentPool<Velocity2D>) == 32);
    static_assert(sizeof(TransformSolver) == 64 && alignof(TransformSolver) == 64);
    static_assert(sizeof(World) == 192 && alignof(World) == 64);

    // 2. Verify EntityId packing & MakeComponentMask
    constexpr EntityId e = EntityId::FromPacked(EntityId{42U, 7U}.Packed());
    static_assert(e.IsValid());
    static_assert(e.index == 42U && e.generation == 7U);
    static_assert(!kNullEntity.IsValid());

    constexpr u64 mask = MakeComponentMask<Velocity2D, Sprite2D, CustomStatsComponent>();
    static_assert(mask == ((1ULL << 1U) | (1ULL << 2U) | (1ULL << 24U)));

    // 3. Verify Sprite2D bit-packed flags
    Sprite2D sp{};
    sp.SetVisible(true);
    sp.SetFlipX(true);
    sp.SetFlipY(false);
    sp.SetYSort(true);
    sp.SetBlend(BlendMode::Additive);
    sp.SetViewSlot(2U);
    if (!sp.Visible() || !sp.FlipX() || sp.FlipY() || !sp.YSort() ||
        sp.Blend() != BlendMode::Additive || sp.ViewSlot() != 2U) {
        return false;
    }

    // 4. Verify FastSinCos accuracy
    f32 c = 0.0f;
    f32 s = 0.0f;
    FastSinCos(0.0f, c, s);
    if (c < 0.999f || s > 0.001f || s < -0.001f) return false;

    return true;
}

static_assert(VerifyConstexprWorldLayer1(), "Compile-time verification of Phase 7 World Layer 1 failed.");

struct TestSuite {
    u32 passed{0};
    u32 failed{0};

    void Check(bool condition, const char* expr, const char* file, int line) noexcept {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] %s:%d: %s\n", file, line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, cond) (suite).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define EXPECT_EQ(suite, a, b)   (suite).Check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define EXPECT_NEAR(suite, a, b, eps) (suite).Check(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

void TestEntityLifecycleFifoRecyclingAndClone(TestSuite& suite) noexcept {
    World world;
    WorldConfig cfg{};
    cfg.maxEntities = 128;
    cfg.defaultPoolCapacity = 64;
    cfg.commandBufferBytes = 16384;
    cfg.arenaBytes = 1024U * 1024U;
    EXPECT_TRUE(suite, world.Init(cfg).has_value());
    EXPECT_TRUE(suite, world.RegisterComponentPool<CustomStatsComponent>(32U).has_value());

    // 1. Spawn 3 entities, destroy e1 then e2, and verify FIFO freelist order (e1 recycled before e2)
    const EntityId e1 = world.Spawn(EntitySpawnDesc{.local = {10.0f, 20.0f}}).value_or(kNullEntity);
    const EntityId e2 = world.Spawn(EntitySpawnDesc{.local = {30.0f, 40.0f}}).value_or(kNullEntity);
    const EntityId e3 = world.Spawn(EntitySpawnDesc{.local = {50.0f, 60.0f}}).value_or(kNullEntity);
    EXPECT_TRUE(suite, e1.IsValid() && e2.IsValid() && e3.IsValid());
    EXPECT_EQ(suite, world.AliveCount(), 3U);

    EXPECT_TRUE(suite, world.Destroy(e1));
    EXPECT_TRUE(suite, world.Destroy(e2));
    EXPECT_EQ(suite, world.AliveCount(), 1U);
    EXPECT_TRUE(suite, !world.IsAlive(e1));
    EXPECT_TRUE(suite, !world.IsAlive(e2));

    const EntityId r1 = world.Spawn().value_or(kNullEntity);
    const EntityId r2 = world.Spawn().value_or(kNullEntity);
    EXPECT_EQ(suite, r1.index, e1.index);
    EXPECT_EQ(suite, r1.generation, static_cast<u16>(e1.generation + 1U));
    EXPECT_EQ(suite, r2.index, e2.index);
    EXPECT_EQ(suite, r2.generation, static_cast<u16>(e2.generation + 1U));

    // 2. Attach multiple built-in + custom components to e3 (+ child) and verify hierarchical CloneEntity & FindByCategory
    const EntityId e3Child = world.Spawn(EntitySpawnDesc{.local = {3.0f, 4.0f}, .parent = e3}).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.Emplace<Velocity2D>(e3, Velocity2D{4.0f, -2.0f, 0.5f, 0.1f}).has_value());
    EXPECT_TRUE(suite, world.Emplace<Tag2D>(e3, Tag2D{"hero"_sid, 0x05U, 99U}).has_value());
    EXPECT_TRUE(suite, world.Emplace<CustomStatsComponent>(e3, CustomStatsComponent{77, 33}).has_value());
    EXPECT_TRUE(suite, world.Emplace<CustomStatsComponent>(e3Child, CustomStatsComponent{15, 10}).has_value());

    EXPECT_EQ(suite, world.FindByTag("hero"_sid), e3);

    const EntityId cloned = world.CloneEntity(e3, LocalTransform2D{100.0f, 200.0f}, true).value_or(kNullEntity);
    EXPECT_TRUE(suite, cloned.IsValid());
    EXPECT_EQ(suite, world.AliveCount(), 6U); // r1, r2, e3, e3Child, cloned, clonedChild
    EXPECT_TRUE(suite, world.Has<Velocity2D>(cloned));
    EXPECT_TRUE(suite, world.Has<Tag2D>(cloned));
    EXPECT_TRUE(suite, world.Has<CustomStatsComponent>(cloned));

    std::array<EntityId, 4> catMatches{};
    EXPECT_EQ(suite, world.FindByCategory(0x01U, catMatches), 2U); // e3 and cloned both have categoryMask 0x05

    // 3. Verify View<...>(Exclude<...>) range-based iteration
    u32 viewMatches = 0U;
    for (auto [id, vel, stats] : world.View<Velocity2D, CustomStatsComponent>()) {
        (void)id;
        vel.vx += 1.0f;
        stats.attackPower += 3;
        ++viewMatches;
    }
    EXPECT_EQ(suite, viewMatches, 2U);
    EXPECT_EQ(suite, world.TryGet<CustomStatsComponent>(e3)->attackPower, 80);
    EXPECT_EQ(suite, world.TryGet<CustomStatsComponent>(cloned)->attackPower, 80);

    // 4. Verify cascading Destroy(e3, true) destroys both e3 and e3Child
    EXPECT_TRUE(suite, world.Destroy(e3, true));
    EXPECT_TRUE(suite, !world.IsAlive(e3));
    EXPECT_TRUE(suite, !world.IsAlive(e3Child));
    EXPECT_EQ(suite, world.AliveCount(), 4U);

    world.Shutdown();
}

void TestHierarchyPropagationKeepWorldTransformAndTeleport(TestSuite& suite) noexcept {
    World world;
    WorldConfig cfg{};
    cfg.maxEntities = 512;
    cfg.defaultPoolCapacity = 256;
    cfg.arenaBytes = 2U * 1024U * 1024U;
    EXPECT_TRUE(suite, world.Init(cfg).has_value());

    // 1. Spawn a root with 200 direct children (proving O(1)-space LCRS traversal never overflows a stack!)
    const EntityId root = world.Spawn(EntitySpawnDesc{.local = {10.0f, 5.0f}}).value_or(kNullEntity);
    EntityId lastChild = kNullEntity;
    for (u32 i = 0; i < 200U; ++i) {
        EntitySpawnDesc cdesc{};
        cdesc.local = {static_cast<f32>(i), 2.0f};
        cdesc.parent = root;
        lastChild = world.Spawn(cdesc).value_or(kNullEntity);
    }
    EXPECT_TRUE(suite, lastChild.IsValid());
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).x, 209.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).y, 7.0f, 1e-4f);

    // Move root by (+20, +10) and verify all 200 children update accurately
    world.SetLocalTransform(root, LocalTransform2D{30.0f, 15.0f});
    world.PropagateTransforms();
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).x, 229.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).y, 17.0f, 1e-4f);

    // 2. Verify SetParent(..., keepWorldTransform = true) preserves world position when detaching or reparenting!
    const EntityId newAnchor = world.Spawn(EntitySpawnDesc{.local = {100.0f, 100.0f}}).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.SetParent(lastChild, newAnchor, true).has_value());
    world.PropagateTransforms();
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).x, 229.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).y, 17.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetLocalTransform(lastChild).x, 129.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetLocalTransform(lastChild).y, -83.0f, 1e-4f);

    // Reparent back to root keeping world transform
    EXPECT_TRUE(suite, world.SetParent(lastChild, root, true).has_value());
    world.PropagateTransforms();
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).x, 229.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).y, 17.0f, 1e-4f);

    // 3. Cycle detection and hierarchy depth enforcement
    const EntityId grandChild = world.Spawn(EntitySpawnDesc{.local = {1.0f, 1.0f}, .parent = lastChild}).value_or(kNullEntity);
    EXPECT_TRUE(suite, !world.SetParent(root, grandChild).has_value()); // Rejects cycle!

    // 4. Verify Teleport preserves parent/child hierarchy links and snaps PrevWorldSnapshot2D (MovedWords bit == 0!)
    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 120.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;
    world.BeginTick(step); // Clears MovedWords from prior move

    world.Teleport(root, LocalTransform2D{100.0f, 50.0f});
    EXPECT_EQ(suite, world.GetParent(lastChild), root);
    EXPECT_EQ(suite, world.GetParent(grandChild), lastChild);
    EXPECT_NEAR(suite, world.GetWorldTransform(lastChild).x, 299.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetPrevWorldByIndex(lastChild.index).x, 299.0f, 1e-4f);
    const bool childMarkedMoved =
        (world.MovedWords()[lastChild.index >> 6U] & (1ULL << (lastChild.index & 63U))) != 0ULL;
    EXPECT_TRUE(suite, !childMarkedMoved);

    world.Shutdown();
}

void TestDeferredCommandsPrefabRoundTripAndWorldExtractor(TestSuite& suite) noexcept {
    World world;
    WorldConfig cfg{};
    cfg.maxEntities = 128;
    cfg.defaultPoolCapacity = 64;
    cfg.arenaBytes = 1024U * 1024U;
    EXPECT_TRUE(suite, world.Init(cfg).has_value());
    EXPECT_TRUE(suite, world.RegisterComponentPool<CustomStatsComponent>(32U).has_value());

    // 1. Test Deferred Command Tape: DeferSpawn returns EntityId immediately so DeferEmplace can target it in the same tick!
    const EntityId parentEnt = world.DeferSpawn(EntitySpawnDesc{.local = {5.0f, 5.0f}}).value_or(kNullEntity);
    const EntityId childEnt  = world.DeferSpawn(EntitySpawnDesc{.local = {2.0f, 3.0f}}).value_or(kNullEntity);
    const EntityId doomedEnt = world.DeferSpawn(EntitySpawnDesc{.local = {9.0f, 9.0f}}).value_or(kNullEntity);
    EXPECT_TRUE(suite, parentEnt.IsValid() && childEnt.IsValid() && doomedEnt.IsValid());

    EXPECT_TRUE(suite, world.DeferEmplace(parentEnt, Sprite2D{.width = 16.0f, .height = 24.0f}).has_value());
    EXPECT_TRUE(suite, world.DeferEmplace(childEnt, CustomStatsComponent{55, 22}).has_value());
    EXPECT_TRUE(suite, world.DeferSetParent(childEnt, parentEnt).has_value());
    EXPECT_TRUE(suite, world.DeferDestroy(doomedEnt).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 120.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;
    world.EndTick(step);

    EXPECT_TRUE(suite, !world.IsAlive(doomedEnt));
    EXPECT_EQ(suite, world.AliveCount(), 2U);
    EXPECT_TRUE(suite, world.Has<Sprite2D>(parentEnt));
    EXPECT_TRUE(suite, world.Has<CustomStatsComponent>(childEnt));
    EXPECT_EQ(suite, world.GetParent(childEnt), parentEnt);
    EXPECT_NEAR(suite, world.GetWorldTransform(childEnt).x, 7.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(childEnt).y, 8.0f, 1e-4f);

    // 2. Test full multi-level SerializePrefabTemplate & WorldExtractor::InstantiatePrefab round-trip
    std::array<u8, 1024> prefabBuf{};
    const auto bytesRes = world.SerializePrefabTemplate(parentEnt, prefabBuf);
    EXPECT_TRUE(suite, bytesRes.has_value());
    const usize prefabBytes = bytesRes.value_or(0U);
    EXPECT_TRUE(suite, prefabBytes > sizeof(CookedPrefabHeader));

    AssetBank bank;
    AssetBankConfig bankCfg{};
    bankCfg.arenaBytes = 256U * 1024U;
    EXPECT_TRUE(suite, bank.Init(bankCfg).has_value());
    const u8 scope = bank.BeginScope().value_or(kInvalidScope);
    bool needsLoad = false;
    const u16 blobSlot = bank.AcquireSlot("prefabs/hero_group"_aid, AssetKind::Blob, scope, AssetResidency::Sim, needsLoad);
    const BlobHandle hPrefab = bank.CommitBlob(blobSlot, std::span<const u8>(prefabBuf.data(), prefabBytes));
    EXPECT_TRUE(suite, hPrefab.IsValid());

    std::array<EntityId, 4> spawnedNodes{};
    const auto instRes = WorldExtractor::InstantiatePrefab(
        world, bank, hPrefab, LocalTransform2D{100.0f, 200.0f}, spawnedNodes);
    EXPECT_TRUE(suite, instRes.has_value());
    const EntityId instRoot = instRes.value_or(kNullEntity);
    const EntityId instChild = spawnedNodes[1];
    EXPECT_TRUE(suite, instRoot.IsValid() && instChild.IsValid());
    EXPECT_EQ(suite, world.GetParent(instChild), instRoot);
    EXPECT_TRUE(suite, world.Has<Sprite2D>(instRoot));
    EXPECT_TRUE(suite, world.Has<CustomStatsComponent>(instChild));
    EXPECT_EQ(suite, world.TryGet<CustomStatsComponent>(instChild)->attackPower, 55);
    EXPECT_NEAR(suite, world.GetWorldTransform(instChild).x, 102.0f, 1e-4f);
    EXPECT_NEAR(suite, world.GetWorldTransform(instChild).y, 203.0f, 1e-4f);

    // 3. Test WorldExtractor::Extract (CameraRig2D centered at (100, 200) + Sprite2D + PointLight2D) & ComputeAuditHash
    const EntityId camEnt = world.Spawn(EntitySpawnDesc{.local = {100.0f, 200.0f}}).value_or(kNullEntity);
    EXPECT_TRUE(suite, world.Emplace<CameraRig2D>(camEnt, CameraRig2D{.zoom = 1.0f, .viewSlot = 0U}).has_value());
    EXPECT_TRUE(suite, world.Emplace<PointLight2D>(instRoot, PointLight2D{.radius = 5.0f, .intensity = 0.8f}).has_value());

    auto renderList = std::make_unique<RenderList>();
    renderList->Reset({1280, 720});
    RenderTimeView rtv{};
    rtv.channelAlpha.fill(0.5f);
    WorldExtractor::Extract(world, bank, rtv, *renderList);
    renderList->Compile();
    EXPECT_EQ(suite, renderList->Stats().submittedQuads, 2U);
    EXPECT_EQ(suite, renderList->Stats().culledQuads, 1U);

    const u64 hashA = world.ComputeAuditHash();
    const u64 hashB = world.ComputeAuditHash();
    EXPECT_EQ(suite, hashA, hashB);

    bank.Shutdown();
    world.Shutdown();
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};

    TestEntityLifecycleFifoRecyclingAndClone(suite);
    TestHierarchyPropagationKeepWorldTransformAndTeleport(suite);
    TestDeferredCommandsPrefabRoundTripAndWorldExtractor(suite);

    std::printf("Phase 7 World v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return suite.failed == 0U ? 0 : 1;
}

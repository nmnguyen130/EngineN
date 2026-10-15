#include "assets/AssetBank.h"
#include "core/time/ChannelModulator.h"
#include "core/time/TimeViews.h"
#include "renderer/RendererList.h"
#include "world/ComponentPool.h"
#include "world/TransformSolver.h"
#include "world/World.h"
#include "world/WorldExtractor.h"
#include "world/WorldTypes.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <source_location>
#include <string_view>
#include <vector>

using namespace en;
using namespace en::world;

namespace {

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

void AssertTrue(bool condition, std::string_view expression, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line()
                  << " Assertion failed: " << expression << "\n";
        ++g_failedAssertions;
    } else {
        ++g_passedAssertions;
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond)

void TestCompileTimeConstantsAndLayout() {
    static_assert(sizeof(LocalTransform2D) == 24);
    static_assert(sizeof(WorldTransform2D) == 32);
    static_assert(alignof(WorldTransform2D) == 32);
    static_assert(sizeof(PrevWorldSnapshot2D) == 24);
    static_assert(sizeof(Velocity2D) == 16);
    static_assert(alignof(Velocity2D) == 16);
    static_assert(sizeof(Sprite2D) == 32);
    static_assert(alignof(Sprite2D) == 32);
    static_assert(sizeof(SpriteAnimator2D) == 16);
    static_assert(alignof(SpriteAnimator2D) == 16);
    static_assert(sizeof(CameraRig2D) == 40);
    static_assert(alignof(CameraRig2D) == 8);

    static_assert(sizeof(EntityId) == 4);
    static_assert(sizeof(HierarchyNode) == 8);
    static_assert(sizeof(HierarchyStep) == 4);
    static_assert(sizeof(WorldCmdHeader) == 8);
    static_assert(sizeof(CookedPrefabHeader) == 16);
    static_assert(sizeof(CookedPrefabEntityRecord) == 64);

    TEST_ASSERT(sizeof(LocalTransform2D) == 24);
    TEST_ASSERT(sizeof(WorldTransform2D) == 32);
    TEST_ASSERT(sizeof(Velocity2D) == 16);
    TEST_ASSERT(sizeof(Sprite2D) == 32);
    TEST_ASSERT(sizeof(SpriteAnimator2D) == 16);
    TEST_ASSERT(sizeof(CameraRig2D) == 40);

    constexpr EntityId nullEnt = kNullEntity;
    TEST_ASSERT(!nullEnt.IsValid());
    TEST_ASSERT(nullEnt.index == 0 && nullEnt.generation == 0);
}

void TestEntityLifecycleAndRecycling() {
    World world;
    auto initRes = world.Init({.maxEntities = 64});
    TEST_ASSERT(initRes.has_value());
    TEST_ASSERT(world.Capacity() == 64);
    TEST_ASSERT(world.AliveCount() == 0);

    // Slot 0 sentinel is never alive to external queries
    TEST_ASSERT(!world.IsAlive(kNullEntity));

    // Spawn entities
    auto e1Res = world.Spawn();
    TEST_ASSERT(e1Res.has_value());
    const EntityId e1 = *e1Res;
    TEST_ASSERT(e1.IsValid());
    TEST_ASSERT(e1.index == 1);
    TEST_ASSERT(e1.generation == 1);
    TEST_ASSERT(world.IsAlive(e1));
    TEST_ASSERT(world.AliveCount() == 1);

    auto e2Res = world.Spawn();
    TEST_ASSERT(e2Res.has_value());
    const EntityId e2 = *e2Res;
    TEST_ASSERT(e2.IsValid());
    TEST_ASSERT(e2.index == 2);
    TEST_ASSERT(e2.generation == 1);
    TEST_ASSERT(world.IsAlive(e2));
    TEST_ASSERT(world.AliveCount() == 2);

    // Destroy e1
    TEST_ASSERT(world.Destroy(e1));
    TEST_ASSERT(!world.IsAlive(e1));
    TEST_ASSERT(world.AliveCount() == 1);

    // Double destroy must return false
    TEST_ASSERT(!world.Destroy(e1));
    TEST_ASSERT(!world.Destroy(kNullEntity));

    // Next spawn allocates next fresh slot e3 = {index: 3, generation: 1}
    auto e3Res = world.Spawn();
    TEST_ASSERT(e3Res.has_value());
    const EntityId e3 = *e3Res;
    TEST_ASSERT(e3.index == 3);
    TEST_ASSERT(e3.generation == 1);
    TEST_ASSERT(world.IsAlive(e3));

    // Exhaust remaining fresh slots (4..63)
    std::vector<EntityId> remaining;
    for (int i = 4; i < 64; ++i) {
        remaining.push_back(*world.Spawn());
    }
    // 60 spawned + e2 + e3 = 62 alive
    TEST_ASSERT(world.AliveCount() == 62);

    // Fresh slots are exhausted, so next spawn recycles e1 from free ring!
    auto recycled1 = world.Spawn();
    TEST_ASSERT(recycled1.has_value());
    TEST_ASSERT(recycled1->index == e1.index);
    TEST_ASSERT(recycled1->generation == 2);
    TEST_ASSERT(world.IsAlive(*recycled1));
    TEST_ASSERT(!world.IsAlive(e1));
    TEST_ASSERT(world.AliveCount() == 63);

    // All slots are now alive and free ring is empty. Next spawn MUST fail!
    auto fullRes = world.Spawn();
    TEST_ASSERT(!fullRes.has_value());
    TEST_ASSERT(fullRes.error() == WorldErrc::EntityPoolExhausted);

    // Destroy e2 (index 2)
    TEST_ASSERT(world.Destroy(e2));
    TEST_ASSERT(world.AliveCount() == 62);

    // Next recycles e2
    auto recycled2 = world.Spawn();
    TEST_ASSERT(recycled2.has_value());
    TEST_ASSERT(recycled2->index == e2.index);
    TEST_ASSERT(recycled2->generation == 2);
    TEST_ASSERT(world.IsAlive(*recycled2));
    TEST_ASSERT(!world.IsAlive(e2));

    // Clear world
    world.Clear();
    TEST_ASSERT(world.AliveCount() == 0);
    TEST_ASSERT(!world.IsAlive(*recycled1));
    TEST_ASSERT(!world.IsAlive(*recycled2));
}

void TestComponentPoolSparseSet() {
    World world;
    (void)world.Init({.maxEntities = 64});

    auto regTrans = world.RegisterComponentPool<LocalTransform2D>(64);
    auto regVel   = world.RegisterComponentPool<Velocity2D>(64);
    auto regSpr   = world.RegisterComponentPool<Sprite2D>(64);
    TEST_ASSERT(regTrans.has_value());
    TEST_ASSERT(regVel.has_value());
    TEST_ASSERT(regSpr.has_value());

    // Re-registration is idempotent and succeeds as a safe no-op
    auto regDup = world.RegisterComponentPool<Velocity2D>(64);
    TEST_ASSERT(regDup.has_value());

    EntityId e1 = *world.Spawn();
    EntityId e2 = *world.Spawn();
    EntityId e3 = *world.Spawn();

    // Emplace Velocity2D
    auto v1Res = world.Emplace<Velocity2D>(e1, Velocity2D{100.0f, -50.0f, 1.5f, 0.05f});
    TEST_ASSERT(v1Res.has_value());
    TEST_ASSERT(world.Has<Velocity2D>(e1));
    TEST_ASSERT(!world.Has<Velocity2D>(e2));
    TEST_ASSERT(!world.Has<Velocity2D>(e3));

    auto* v1 = world.TryGet<Velocity2D>(e1);
    TEST_ASSERT(v1 != nullptr);
    TEST_ASSERT(v1->vx == 100.0f);
    TEST_ASSERT(v1->vy == -50.0f);

    // Emplace on e2 and e3
    (void)world.Emplace<Velocity2D>(e2, Velocity2D{200.0f, 0.0f});
    (void)world.Emplace<Velocity2D>(e3, Velocity2D{300.0f, 50.0f});

    const auto& velPool = world.GetPool<Velocity2D>();
    TEST_ASSERT(velPool.Size() == 3);
    TEST_ASSERT(velPool.Contains(e1));
    TEST_ASSERT(velPool.Contains(e2));
    TEST_ASSERT(velPool.Contains(e3));

    // DirectAtEntityIndex O(1)
    TEST_ASSERT(velPool.DirectAtEntityIndex(e2.index).vx == 200.0f);

    // Remove middle entity (e2) - sparse set swap-and-pop
    TEST_ASSERT(world.Remove<Velocity2D>(e2));
    TEST_ASSERT(!world.Has<Velocity2D>(e2));
    TEST_ASSERT(velPool.Size() == 2);
    TEST_ASSERT(world.Has<Velocity2D>(e1));
    TEST_ASSERT(world.Has<Velocity2D>(e3));
    TEST_ASSERT(world.TryGet<Velocity2D>(e3)->vx == 300.0f);

    // Destroying an entity automatically cleans all its attached components
    TEST_ASSERT(world.Destroy(e3));
    TEST_ASSERT(velPool.Size() == 1);
    TEST_ASSERT(!velPool.Contains(e3));
    TEST_ASSERT(velPool.Contains(e1));
}

void TestLockstepAnimationGroup() {
    World world;
    (void)world.Init({.maxEntities = 64});

    (void)world.RegisterComponentPool<Sprite2D>(64);
    (void)world.RegisterComponentPool<SpriteAnimator2D>(64);

    EntityId e1 = *world.Spawn();
    EntityId e2 = *world.Spawn();
    EntityId e3 = *world.Spawn();

    const auto& sprPool  = world.GetPool<Sprite2D>();
    const auto& animPool = world.GetPool<SpriteAnimator2D>();

    // Entity 1 only has Sprite2D -> groupSize is 0
    (void)world.Emplace<Sprite2D>(e1, Sprite2D{});
    TEST_ASSERT(sprPool.Size() == 1);
    TEST_ASSERT(sprPool.GroupSize() == 0);

    // Entity 2 has both Sprite2D and SpriteAnimator2D -> promoted into lockstep group!
    (void)world.Emplace<Sprite2D>(e2, Sprite2D{});
    (void)world.Emplace<SpriteAnimator2D>(e2, SpriteAnimator2D{.frameCount = 8});
    TEST_ASSERT(sprPool.GroupSize() == 1);
    TEST_ASSERT(animPool.GroupSize() == 1);
    TEST_ASSERT(sprPool.Entities()[0] == e2);
    TEST_ASSERT(animPool.Entities()[0] == e2);

    // Entity 3 has both -> group grows to 2
    (void)world.Emplace<SpriteAnimator2D>(e3, SpriteAnimator2D{.frameCount = 4});
    (void)world.Emplace<Sprite2D>(e3, Sprite2D{});
    TEST_ASSERT(sprPool.GroupSize() == 2);
    TEST_ASSERT(animPool.GroupSize() == 2);

    // Demote Entity 2 by removing its animator
    TEST_ASSERT(world.Remove<SpriteAnimator2D>(e2));
    TEST_ASSERT(sprPool.GroupSize() == 1);
    TEST_ASSERT(animPool.GroupSize() == 1);
    TEST_ASSERT(sprPool.Entities()[0] == e3);
    TEST_ASSERT(animPool.Entities()[0] == e3);
}

void TestTransformSolverAndHierarchy() {
    World world;
    (void)world.Init({.maxEntities = 64});

    // Spawn root and child
    EntityId root = *world.Spawn({.hasSpatial = true, .isStaticSpatial = false});
    EntityId child = *world.Spawn({.hasSpatial = true, .isStaticSpatial = false});
    EntityId grandChild = *world.Spawn({.hasSpatial = true, .isStaticSpatial = false});

    world.SetLocalTransform(root, LocalTransform2D{
        .x = 100.0f,
        .y = 50.0f,
        .scaleX = 1.0f,
        .scaleY = 1.0f,
        .rotationRad = 0.0f
    });

    world.SetLocalTransform(child, LocalTransform2D{
        .x = 20.0f,
        .y = 10.0f,
        .scaleX = 1.0f,
        .scaleY = 1.0f,
        .rotationRad = 0.0f
    });

    world.SetLocalTransform(grandChild, LocalTransform2D{
        .x = 5.0f,
        .y = 2.0f,
        .scaleX = 1.0f,
        .scaleY = 1.0f,
        .rotationRad = 0.0f
    });

    // Parent child to root, and grandChild to child
    TEST_ASSERT(world.SetParent(child, root).has_value());
    TEST_ASSERT(world.SetParent(grandChild, child).has_value());

    const auto& solver = world.Solver();
    TEST_ASSERT(solver.GetDepth(root.index) == 0);
    TEST_ASSERT(solver.GetDepth(child.index) == 1);
    TEST_ASSERT(solver.GetDepth(grandChild.index) == 2);
    TEST_ASSERT(solver.GetParentIndex(child.index) == root.index);
    TEST_ASSERT(solver.GetParentIndex(grandChild.index) == child.index);

    // Propagate
    world.PropagateTransforms();

    const WorldTransform2D& wRoot = world.GetWorldTransform(root);
    const WorldTransform2D& wChild = world.GetWorldTransform(child);
    const WorldTransform2D& wGrandChild = world.GetWorldTransform(grandChild);

    TEST_ASSERT(std::abs(wRoot.x - 100.0f) < 0.001f);
    TEST_ASSERT(std::abs(wRoot.y - 50.0f) < 0.001f);

    TEST_ASSERT(std::abs(wChild.x - 120.0f) < 0.001f);
    TEST_ASSERT(std::abs(wChild.y - 60.0f) < 0.001f);

    TEST_ASSERT(std::abs(wGrandChild.x - 125.0f) < 0.001f);
    TEST_ASSERT(std::abs(wGrandChild.y - 62.0f) < 0.001f);

    // Cycle detection: child cannot become parent of root!
    auto cycleRes = world.SetParent(root, grandChild);
    TEST_ASSERT(!cycleRes.has_value());
    TEST_ASSERT(cycleRes.error() == WorldErrc::HierarchyCycleDetected);

    // Self parenting is prohibited
    auto selfRes = world.SetParent(root, root);
    TEST_ASSERT(!selfRes.has_value());

    // Unparent child (parent to null) and propagate transforms
    TEST_ASSERT(world.SetParent(child, kNullEntity).has_value());
    TEST_ASSERT(solver.GetParentIndex(child.index) == 0);
    world.PropagateTransforms();
    TEST_ASSERT(solver.GetDepth(child.index) == 0);
    TEST_ASSERT(solver.GetDepth(grandChild.index) == 1);
}

void TestDeferredCommandTape() {
    World world;
    (void)world.Init({.maxEntities = 64});
    (void)world.RegisterComponentPool<Velocity2D>(64);

    EntityId e1 = *world.Spawn({.hasSpatial = true});
    EntityId e2 = *world.Spawn({.hasSpatial = true});

    // 1. Defer emplace
    TEST_ASSERT(world.DeferEmplace<Velocity2D>(e1, Velocity2D{12.0f, 34.0f}).has_value());
    TEST_ASSERT(!world.Has<Velocity2D>(e1)); // Not applied immediately

    // 2. Defer set parent
    TEST_ASSERT(world.DeferSetParent(e2, e1).has_value());
    TEST_ASSERT(world.Solver().GetParentIndex(e2.index) == 0);

    // 3. Flush tape
    TEST_ASSERT(world.FlushCommands().has_value());
    TEST_ASSERT(world.Has<Velocity2D>(e1));
    TEST_ASSERT(world.TryGet<Velocity2D>(e1)->vx == 12.0f);
    TEST_ASSERT(world.Solver().GetParentIndex(e2.index) == e1.index);

    // 4. Defer destroy
    TEST_ASSERT(world.DeferDestroy(e2).has_value());
    TEST_ASSERT(world.IsAlive(e2)); // Still alive until flush
    TEST_ASSERT(world.FlushCommands().has_value());
    TEST_ASSERT(!world.IsAlive(e2));
}

void TestWorldViewQueries() {
    World world;
    (void)world.Init({.maxEntities = 64});
    (void)world.RegisterComponentPool<LocalTransform2D>(64);
    (void)world.RegisterComponentPool<Velocity2D>(64);
    (void)world.RegisterComponentPool<Sprite2D>(64);

    // e1: Transform + Velocity
    EntityId e1 = *world.Spawn();
    (void)world.Emplace<LocalTransform2D>(e1, LocalTransform2D{.x = 10.0f});
    (void)world.Emplace<Velocity2D>(e1, Velocity2D{.vx = 100.0f});

    // e2: Transform only
    EntityId e2 = *world.Spawn();
    (void)world.Emplace<LocalTransform2D>(e2, LocalTransform2D{.x = 20.0f});

    // e3: Transform + Velocity + Sprite
    EntityId e3 = *world.Spawn();
    (void)world.Emplace<LocalTransform2D>(e3, LocalTransform2D{.x = 30.0f});
    (void)world.Emplace<Velocity2D>(e3, Velocity2D{.vx = 300.0f});
    (void)world.Emplace<Sprite2D>(e3, Sprite2D{});

    // Query View<LocalTransform2D, Velocity2D>: should find e1 and e3
    u32 countBoth = 0;
    f32 sumVx = 0.0f;
    for (auto [id, t, v] : world.View<LocalTransform2D, Velocity2D>()) {
        ++countBoth;
        sumVx += v.vx;
        TEST_ASSERT(id == e1 || id == e3);
    }
    TEST_ASSERT(countBoth == 2);
    TEST_ASSERT(sumVx == 400.0f);

    // Query View with Exclude: View<LocalTransform2D>(Exclude<Velocity2D>{}) -> only e2
    u32 countExclude = 0;
    for (auto [id, t] : world.View<LocalTransform2D>(Exclude<Velocity2D>{})) {
        ++countExclude;
        TEST_ASSERT(id == e2);
        TEST_ASSERT(t.x == 20.0f);
    }
    TEST_ASSERT(countExclude == 1);
}

void TestSimulationTickAndKinematics() {
    World world;
    (void)world.Init({.maxEntities = 64});
    (void)world.RegisterComponentPool<LocalTransform2D>(64);
    (void)world.RegisterComponentPool<Velocity2D>(64);
    (void)world.RegisterComponentPool<Sprite2D>(64);
    (void)world.RegisterComponentPool<SpriteAnimator2D>(64);

    EntityId e = *world.Spawn({.hasSpatial = true, .isStaticSpatial = false});
    world.SetLocalTransform(e, LocalTransform2D{.x = 0.0f, .y = 0.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    (void)world.Emplace<Velocity2D>(e, Velocity2D{.vx = 120.0f, .vy = 60.0f, .linearDamping = 0.0f});

    // Setup dummy time modulator and mock step
    time::ChannelModulator modulator;
    modulator.AdvanceTick();
    const time::SimStepView step(1, 0, modulator);

    assets::AssetBank bank;
    (void)bank.Init();

    // 1 tick at 120Hz = 1/120s
    world.BeginTick(step);
    world.StepKinematicsAndAnimations(step, bank);
    world.EndTick(step);

    const LocalTransform2D& tAfter = world.GetLocalTransform(e);
    TEST_ASSERT(std::abs(tAfter.x - 1.0f) < 0.01f);
    TEST_ASSERT(std::abs(tAfter.y - 0.5f) < 0.01f);
}

void TestDeterminismAuditHash() {
    World w1, w2;
    (void)w1.Init({.maxEntities = 64});
    (void)w2.Init({.maxEntities = 64});

    (void)w1.RegisterComponentPool<LocalTransform2D>(64);
    (void)w1.RegisterComponentPool<Velocity2D>(64);
    (void)w2.RegisterComponentPool<LocalTransform2D>(64);
    (void)w2.RegisterComponentPool<Velocity2D>(64);

    for (int i = 0; i < 5; ++i) {
        EntityId e1 = *w1.Spawn({.hasSpatial = true});
        EntityId e2 = *w2.Spawn({.hasSpatial = true});
        const LocalTransform2D t{.x = static_cast<f32>(i * 10), .y = static_cast<f32>(i * 5)};
        w1.SetLocalTransform(e1, t);
        w2.SetLocalTransform(e2, t);
        (void)w1.Emplace<Velocity2D>(e1, Velocity2D{static_cast<f32>(i), 0.0f});
        (void)w2.Emplace<Velocity2D>(e2, Velocity2D{static_cast<f32>(i), 0.0f});
    }

    w1.PropagateTransforms();
    w2.PropagateTransforms();

    const u64 hash1 = w1.ComputeAuditHash();
    const u64 hash2 = w2.ComputeAuditHash();
    TEST_ASSERT(hash1 != 0);
    TEST_ASSERT(hash1 == hash2); // Determinism contract: identical state produces identical hash

    // Perturb 1 float in w2
    w2.GetPool<Velocity2D>().Components()[0].vx += 0.001f;
    const u64 hashPerturbed = w2.ComputeAuditHash();
    TEST_ASSERT(hash1 != hashPerturbed); // Audit hash must detect divergence immediately
}

void TestWorldExtractorAndPrefab() {
    World world;
    (void)world.Init({.maxEntities = 64});
    (void)world.RegisterComponentPool<LocalTransform2D>(64);
    (void)world.RegisterComponentPool<Sprite2D>(64);
    (void)world.RegisterComponentPool<SpriteAnimator2D>(64);

    assets::AssetBank bank;
    (void)bank.Init();
    auto scopeRes = bank.BeginScope();
    TEST_ASSERT(scopeRes.has_value());
    const assets::ScopeId scope = *scopeRes;

    // Create an aligned mock cooked prefab blob
    constexpr u16 kPrefabEntities = 2;
    constexpr usize kBlobSize = sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntityRecord) * kPrefabEntities;
    alignas(16) std::array<u8, kBlobSize> blobData{};
    auto* hdr = reinterpret_cast<CookedPrefabHeader*>(blobData.data());
    hdr->magic = CookedPrefabHeader::kMagic;
    hdr->version = 1;
    hdr->entityCount = kPrefabEntities;
    hdr->totalBytes = static_cast<u32>(blobData.size());

    auto* recs = reinterpret_cast<CookedPrefabEntityRecord*>(blobData.data() + sizeof(CookedPrefabHeader));
    recs[0].localTransform = LocalTransform2D{.x = 100.0f, .y = 200.0f, .scaleX = 1.0f, .scaleY = 1.0f};
    recs[0].flags = 0x01; // HasSprite
    recs[0].parentLocalIndex = 0xFFFF; // Root

    recs[1].localTransform = LocalTransform2D{.x = 10.0f, .y = 20.0f, .scaleX = 1.0f, .scaleY = 1.0f};
    recs[1].flags = 0x01; // HasSprite
    recs[1].parentLocalIndex = 0; // Child of root

    // Commit blob to AssetBank
    const assets::AssetId blobId{0x12345678ULL};
    bool needsLoad = false;
    auto slotRes = bank.AcquireSlot(blobId, assets::AssetKind::Blob, scope, assets::Residency::Sim, needsLoad);
    TEST_ASSERT(slotRes.has_value());
    TEST_ASSERT(bank.CommitBlob(*slotRes, blobData, CookedPrefabHeader::kSchemaHash, false).has_value());

    const assets::BlobHandle prefabHandle = bank.ToBlobHandle(*slotRes);
    TEST_ASSERT(prefabHandle.IsValid());

    // Instantiate prefab
    std::array<EntityId, 2> spawned{};
    auto instRes = WorldExtractor::InstantiatePrefab(world, bank, prefabHandle, LocalTransform2D{.x = 50.0f, .y = 50.0f}, spawned);
    TEST_ASSERT(instRes.has_value());
    TEST_ASSERT(spawned[0].IsValid());
    TEST_ASSERT(spawned[1].IsValid());
    TEST_ASSERT(world.IsAlive(spawned[0]));
    TEST_ASSERT(world.IsAlive(spawned[1]));

    // Check hierarchy: spawned[1] is child of spawned[0]
    TEST_ASSERT(world.Solver().GetParentIndex(spawned[1].index) == spawned[0].index);

    // Extract to heap-allocated RenderList (5MB SoA buffer)
    auto list = std::make_unique<renderer::RenderList>();
    time::RenderTimeView renderTime{};
    WorldExtractor::Extract(world, bank, renderTime, *list);
    TEST_ASSERT(list->SortedKeys().size() >= 1);
}

} // namespace

int main() {
    std::cout << "Running EngineN World Subsystem Tests...\n";

    TestCompileTimeConstantsAndLayout();
    TestEntityLifecycleAndRecycling();
    TestComponentPoolSparseSet();
    TestLockstepAnimationGroup();
    TestTransformSolverAndHierarchy();
    TestDeferredCommandTape();
    TestWorldViewQueries();
    TestSimulationTickAndKinematics();
    TestDeterminismAuditHash();
    TestWorldExtractorAndPrefab();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

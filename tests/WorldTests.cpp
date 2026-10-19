#include "world/World.h"
#include "world/WorldExtractor.h"
#include "renderer/RenderList.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

#define EN_TEST_ASSERT(cond)                                                                     \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

constexpr bool Near(en::f32 a, en::f32 b, en::f32 eps = 1e-4f) noexcept {
    return std::abs(a - b) <= eps;
}

struct alignas(16) CustomTestComp {
    static constexpr en::u8 kTypeId = 24;
    en::u32 tag{0};
    en::f32 weight{1.0f};
    en::u64 payload{0};
};
static_assert(en::world::Component<CustomTestComp>);


void TestMemoryLayoutsAndEntityLifecycle() {
    using namespace en;
    using namespace en::world;

    static_assert(sizeof(EntityId) == 4);
    static_assert(sizeof(LocalTransform2D) == 24);
    static_assert(sizeof(PrevWorldSnapshot2D) == 24);
    static_assert(sizeof(WorldTransform2D) == 32);
    static_assert(sizeof(Velocity2D) == 16);
    static_assert(sizeof(Sprite2D) == 32);
    static_assert(sizeof(SpriteAnimator2D) == 16);
    static_assert(sizeof(CameraRig2D) == 40);
    static_assert(sizeof(WorldConfig) == 12);
    static_assert(sizeof(PoolOps) == 32);
    static_assert(sizeof(ComponentPool<Velocity2D>) == 32);
    static_assert(sizeof(TransformSolver) == 64);
    static_assert(sizeof(World) == 192);
    static_assert(alignof(World) == 64);

    World world{};
    EN_TEST_ASSERT(world.Init(WorldConfig{.maxEntities = 64, .defaultComponentPoolCapacity = 64, .commandBufferBytes = 4096}).has_value());
    EN_TEST_ASSERT(!world.IsAlive(kNullEntity));
    EN_TEST_ASSERT(!world.IsAlive(EntityId{0, 1}));

    auto e1 = world.Spawn();
    EN_TEST_ASSERT(e1.has_value());
    EN_TEST_ASSERT(e1->index == 1 && e1->generation == 1);
    EN_TEST_ASSERT(world.IsAlive(*e1));
    EN_TEST_ASSERT(world.AliveCount() == 1);

    EN_TEST_ASSERT(world.Destroy(*e1));
    EN_TEST_ASSERT(!world.IsAlive(*e1));
    EN_TEST_ASSERT(!world.Destroy(*e1));
    EN_TEST_ASSERT(world.AliveCount() == 0);
    EN_TEST_ASSERT(world.GetComponentMaskByIndex(1) == 0ULL);

    // Exhaust fresh slots 2..63 so the intrusive FIFO freelist recycles slot 1 with generation 2.
    std::vector<EntityId> alive;
    for (u16 i = 2; i < 64; ++i) {
        auto r = world.Spawn();
        EN_TEST_ASSERT(r.has_value());
        alive.push_back(*r);
    }
    auto recycled = world.Spawn();
    EN_TEST_ASSERT(recycled.has_value());
    EN_TEST_ASSERT(recycled->index == 1 && recycled->generation == 2);
    EN_TEST_ASSERT(!world.IsAlive(*e1));
    EN_TEST_ASSERT(world.IsAlive(*recycled));

    auto exhausted = world.Spawn();
    EN_TEST_ASSERT(!exhausted.has_value() && exhausted.error() == WorldErrc::EntityPoolExhausted);

    world.Clear();
    EN_TEST_ASSERT(world.AliveCount() == 0);
    EN_TEST_ASSERT(!world.IsAlive(*recycled));
}

void TestHierarchyCycleDepthAndPropagation() {
    using namespace en;
    using namespace en::world;

    World world{};
    EN_TEST_ASSERT(world.Init(WorldConfig{.maxEntities = 128}).has_value());

    const EntityId root = *world.Spawn({.localTransform = {.x = 10.0f, .y = 20.0f, .scaleX = 2.0f, .scaleY = 2.0f}});
    const EntityId child = *world.Spawn({.localTransform = {.x = 5.0f, .y = -3.0f}, .parent = root});
    const EntityId grandChild = *world.Spawn({.localTransform = {.x = 1.0f, .y = 2.0f}, .parent = child});

    EN_TEST_ASSERT(world.SetParent(root, root).error() == WorldErrc::HierarchyCycleDetected);
    EN_TEST_ASSERT(world.SetParent(root, grandChild).error() == WorldErrc::HierarchyCycleDetected);

    world.PropagateTransforms();

    const WorldTransform2D& wtChild = world.GetWorldTransform(child);
    EN_TEST_ASSERT(Near(wtChild.x, 10.0f + 5.0f * 2.0f));
    EN_TEST_ASSERT(Near(wtChild.y, 20.0f + (-3.0f) * 2.0f));

    const WorldTransform2D& wtGrand = world.GetWorldTransform(grandChild);
    EN_TEST_ASSERT(Near(wtGrand.x, 20.0f + 1.0f * 2.0f));
    EN_TEST_ASSERT(Near(wtGrand.y, 14.0f + 2.0f * 2.0f));

    // Subtree depth limit enforcement: build a chain of 30 nodes and try attaching a 3-node subtree at its leaf.
    EntityId prev = *world.Spawn();
    for (u8 d = 0; d < 30; ++d) {
        EntityId next = *world.Spawn({.parent = prev});
        prev = next;
    }
    EN_TEST_ASSERT(world.SetParent(root, prev).error() == WorldErrc::HierarchyDepthExceeded);

    // Destroying child detaches grandChild to root.
    EN_TEST_ASSERT(world.Destroy(child));
    world.PropagateTransforms();
    EN_TEST_ASSERT(Near(world.GetWorldTransform(grandChild).x, 1.0f));
    EN_TEST_ASSERT(Near(world.GetWorldTransform(grandChild).y, 2.0f));
}

void TestAnimationsAndViews() {
    using namespace en;
    using namespace en::world;

    World world{};
    EN_TEST_ASSERT(world.Init(WorldConfig{.maxEntities = 128}).has_value());
    EN_TEST_ASSERT(world.RegisterComponentPool<CustomTestComp>(64).has_value());

    const EntityId eStaticSprite = *world.Spawn();
    const EntityId eAnimA        = *world.Spawn();
    const EntityId eAnimB        = *world.Spawn();

    (void)world.Emplace<Sprite2D>(eStaticSprite, Sprite2D{.baseDepth = 100});
    (void)world.Emplace<Sprite2D>(eAnimA, Sprite2D{.baseDepth = 200});
    (void)world.Emplace<SpriteAnimator2D>(eAnimA, SpriteAnimator2D{
        .atlas = {1, 1}, .firstFrame = 2, .frameCount = 4, .currentFrame = 0, .ticksPerFrame = 2
    });
    (void)world.Emplace<SpriteAnimator2D>(eAnimB, SpriteAnimator2D{
        .atlas = {1, 1}, .firstFrame = 6, .frameCount = 2, .currentFrame = 0, .ticksPerFrame = 1
    });
    (void)world.Emplace<Sprite2D>(eAnimB, Sprite2D{.baseDepth = 300});
    (void)world.Emplace<CustomTestComp>(eAnimB, CustomTestComp{.tag = 42});

    u32 groupVisited = 0;
    for (auto [id, sp, anim] : world.View<Sprite2D, SpriteAnimator2D>()) {
        EN_TEST_ASSERT(id == eAnimA || id == eAnimB);
        EN_TEST_ASSERT(sp.baseDepth == 200 || sp.baseDepth == 300);
        EN_TEST_ASSERT(anim.frameCount >= 2);
        ++groupVisited;
    }
    EN_TEST_ASSERT(groupVisited == 2);

    u32 filteredVisited = 0;
    for (auto [id, sp] : world.View<Sprite2D>(Exclude<CustomTestComp>{})) {
        EN_TEST_ASSERT(id != eAnimB);
        (void)sp;
        ++filteredVisited;
    }
    EN_TEST_ASSERT(filteredVisited == 2);

    time::TimeModulator modulator{};
    const time::SimStepView step(1, 0, modulator);
    for (int t = 0; t < 2; ++t) {
        world.BeginTick(step);
        world.StepAmbientVelocities(step);
        world.StepAnimations(step);
        world.EndTick(step);
    }

    EN_TEST_ASSERT(world.TryGet<SpriteAnimator2D>(eAnimA)->currentFrame == 1);
}

void TestDeferredCommandsAndExtractor() {
    using namespace en;
    using namespace en::world;

    World world{};
    EN_TEST_ASSERT(world.Init(WorldConfig{.maxEntities = 128}).has_value());
    assets::AssetBank bank{};

    struct PrefabBlobBuf {
        CookedPrefabHeader hdr;
        CookedPrefabEntityRecord recs[2];
    } blobBuf{};

    blobBuf.hdr = CookedPrefabHeader{
        .magic       = CookedPrefabHeader::kMagic,
        .version     = 1,
        .entityCount = 2,
        .totalBytes  = sizeof(PrefabBlobBuf),
    };
    blobBuf.recs[0] = CookedPrefabEntityRecord{
        .localTransform   = {.x = 100.0f, .y = 200.0f, .scaleX = 1.0f, .scaleY = 1.0f},
        .atlasOrTextureId = {12345ULL},
        .parentLocalIndex = 0xFFFF,
        .firstFrame       = 1,
        .frameCount       = 1,
        .ticksPerFrame    = 10,
        .baseDepth        = 1000,
        .flags            = 0x01u,
    };
    blobBuf.recs[1] = CookedPrefabEntityRecord{
        .localTransform   = {.x = 16.0f, .y = -8.0f, .scaleX = 1.0f, .scaleY = 1.0f},
        .atlasOrTextureId = {12345ULL},
        .parentLocalIndex = 0,
        .firstFrame       = 2,
        .frameCount       = 4,
        .ticksPerFrame    = 4,
        .baseDepth        = 2000,
        .flags            = 0x03u,
    };
    EN_TEST_ASSERT(bank.Init({.maxSlots = 16, .maxBlobs = 16, .blobArenaBytes = 4096}).has_value());
    bool needsLoad = false;
    auto slotRes = bank.AcquireSlot(assets::AssetId::Hash("prefab/test.eprefab"), assets::AssetKind::Blob, 0, assets::Residency::Sim, needsLoad);
    EN_TEST_ASSERT(slotRes.has_value());
    const std::span<const u8> blobSpan{reinterpret_cast<const u8*>(&blobBuf), sizeof(blobBuf)};
    EN_TEST_ASSERT(bank.CommitBlob(*slotRes, blobSpan, CookedPrefabHeader::kSchemaHash, false).has_value());
    const assets::BlobHandle blobHandle = bank.ToBlobHandle(*slotRes);

    EntityId spawned[2]{};
    auto rootRes = WorldExtractor::InstantiatePrefab(
        world, bank, blobHandle, LocalTransform2D{.x = 50.0f, .y = 60.0f}, spawned
    );
    EN_TEST_ASSERT(rootRes.has_value());
    EN_TEST_ASSERT(spawned[0] == *rootRes);
    EN_TEST_ASSERT(Near(world.GetWorldTransform(spawned[1]).x, 66.0f));
    EN_TEST_ASSERT(Near(world.GetWorldTransform(spawned[1]).y, 52.0f));

    EN_TEST_ASSERT(world.DeferEmplace(spawned[0], Velocity2D{.vx = 120.0f, .vy = 0.0f}).has_value());
    EN_TEST_ASSERT(world.DeferEmplace(spawned[0], CameraRig2D{.zoom = 2.0f, .viewSlot = 0, .active = true}).has_value());
    EN_TEST_ASSERT(world.FlushCommands().has_value());

    time::TimeModulator modulator{};
    const time::SimStepView step(1, 0, modulator);
    world.BeginTick(step);
    world.StepAmbientVelocities(step);
    world.StepAnimations(step);
    world.EndTick(step);

    EN_TEST_ASSERT(Near(world.GetWorldTransform(spawned[0]).x, 51.0f));
    EN_TEST_ASSERT(Near(world.GetWorldTransform(spawned[1]).x, 67.0f));
    EN_TEST_ASSERT(Near(world.GetPrevWorldByIndex(spawned[0].index).x, 50.0f));

    time::RenderTimeView renderTime{};
    for (u8 c = 0; c < 8; ++c) {
        renderTime.channelAlpha.alpha[c] = 0.5f;
    }
    renderTime.rawAlpha = 0.5f;

    renderer::RenderList renderList{};
    WorldExtractor::Extract(world, bank, renderTime, renderList);
    EN_TEST_ASSERT(Near(renderList.View(0).worldCenterX, 50.5f));
    EN_TEST_ASSERT(renderList.Stats().submittedQuads == 2);

    const u64 hashA = world.ComputeAuditHash();
    const u64 hashB = world.ComputeAuditHash();
    EN_TEST_ASSERT(hashA == hashB && hashA != 0ULL);
}

} // namespace

int main() {
    TestMemoryLayoutsAndEntityLifecycle();
    TestHierarchyCycleDepthAndPropagation();
    TestAnimationsAndViews();
    TestDeferredCommandsAndExtractor();
    std::printf("All EngineN Phase 6 (world) final tests passed!\n");
    return 0;
}

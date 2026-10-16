#include "gameplay/GameplayPipeline.h"
#include "gameplay/GameplayTypes.h"
#include "gameplay/KinematicSolver2D.h"
#include "gameplay/SpatialIndex2D.h"
#include "gameplay/StateAndCombatSolver.h"
#include "input/ActionBuffer.h"
#include "world/World.h"
#include "assets/AssetBank.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace en;
using namespace en::world;
using namespace en::gameplay;

u32 g_testsPassed = 0;
u32 g_testsFailed = 0;

void AssertTrue(bool condition, std::string_view expression, int line) {
    if (condition) {
        ++g_testsPassed;
    } else {
        ++g_testsFailed;
        std::cerr << "[FAIL] Line " << line << ": " << expression << "\n";
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond, __LINE__)

void TestGameplayTypesLayout() {
    static_assert(sizeof(CompactAabb2D) == 8);
    static_assert(sizeof(KinematicMotor2D) == 32);
    static_assert(sizeof(KinematicCarrier2D) == 16);
    static_assert(sizeof(MotionStack2D) == 32);
    static_assert(sizeof(StateMachine2D) == 16);
    static_assert(sizeof(HitboxRig2D) == 32);
    static_assert(sizeof(DamageReceiver2D) == 16);
    static_assert(sizeof(SafeGroundBreadcrumb2D) == 16);
    static_assert(sizeof(CameraController2D) == 64);
    static_assert(sizeof(CookedCollisionMapHeader) == 32);
    static_assert(sizeof(CookedAnimRigHeader) == 32);

    TEST_ASSERT(sizeof(CompactAabb2D) == 8);
    TEST_ASSERT(sizeof(KinematicMotor2D) == 32);
    TEST_ASSERT(sizeof(KinematicCarrier2D) == 16);
    TEST_ASSERT(sizeof(MotionStack2D) == 32);
    TEST_ASSERT(sizeof(StateMachine2D) == 16);
    TEST_ASSERT(sizeof(HitboxRig2D) == 32);
    TEST_ASSERT(sizeof(DamageReceiver2D) == 16);
    TEST_ASSERT(sizeof(SafeGroundBreadcrumb2D) == 16);
    TEST_ASSERT(sizeof(CameraController2D) == 64);

    // CompactAabb2D Q8.8 roundtrip checks
    CompactAabb2D bounds = CompactAabb2D::FromMeters(1.5f, -2.25f, 0.5f, 1.0f);
    TEST_ASSERT(std::abs(bounds.OffsetX() - 1.5f) < 0.01f);
    TEST_ASSERT(std::abs(bounds.OffsetY() - (-2.25f)) < 0.01f);
    TEST_ASSERT(std::abs(bounds.HalfW() - 0.5f) < 0.01f);
    TEST_ASSERT(std::abs(bounds.HalfH() - 1.0f) < 0.01f);
}

void TestSpatialIndexSweepsAndRaycasts() {
    SpatialIndex2D spatial;
    alignas(64) std::array<u8, 128 * 1024> arenaMemory{};
    spatial.BindArena(arenaMemory);

    // Construct a mock collision map blob with 1 chunk (8x8 tiles = 64 tiles)
    // Map dimensions: chunkCountX = 1, chunkCountY = 1
    // Tile size: 1.0f (tileSize_q8 = 256)
    constexpr u16 kChunks = 1;
    constexpr u32 kTiles = 64;
    constexpr usize kBlobSize = sizeof(CookedCollisionMapHeader) +
                                sizeof(CookedCollisionChunk8x8) * kChunks +
                                sizeof(CookedTileCell) * kTiles;
    alignas(16) std::vector<u8> blobData(kBlobSize, 0);

    auto* hdr = reinterpret_cast<CookedCollisionMapHeader*>(blobData.data());
    hdr->magic = CookedCollisionMapHeader::kMagic;
    hdr->version = 1;
    hdr->tileSize_q8 = 256; // 1.0f meter
    hdr->chunkCountX = 1;
    hdr->chunkCountY = 1;
    hdr->originX = 0.0f;
    hdr->originY = 0.0f;
    hdr->fineTileTableOffset = sizeof(CookedCollisionMapHeader) + sizeof(CookedCollisionChunk8x8);

    auto* chunk = reinterpret_cast<CookedCollisionChunk8x8*>(blobData.data() + sizeof(CookedCollisionMapHeader));
    chunk->fineTileBaseIndex = 0;

    auto* tiles = reinterpret_cast<CookedTileCell*>(blobData.data() + hdr->fineTileTableOffset);
    // Row 0 (y = 0..1): solid ground tiles
    for (int x = 0; x < 8; ++x) {
        tiles[x].shape = TileShape::FullSolid;
        chunk->solidBitmask |= (1ULL << x);
        chunk->pureFullBlockBitmask |= (1ULL << x);
    }
    // Row 4, Col 2..5: One-way platform
    for (int x = 2; x <= 5; ++x) {
        const int idx = 4 * 8 + x;
        tiles[idx].shape = TileShape::OneWayTop;
        chunk->solidBitmask |= (1ULL << idx);
    }

    assets::BlobRecord blob{};
    blob.data = blobData.data();
    blob.size = blobData.size();

    auto bindRes = spatial.BindCollisionMapBlob(blob);
    TEST_ASSERT(bindRes.has_value());

    // 1. Sweep downward from (3.0f, 4.0f) with deltaY = -5.0f into solid ground (y = 0..1, top at y = 1.0f)
    const f32 halfW = 0.4f;
    const f32 halfH = 0.5f;
    SlabImpact2D groundImpact = spatial.SweepAabb(
        kNullEntity, 3.0f, 4.0f, halfW, halfH, 0.0f, -5.0f, 0xFFFF, false
    );
    TEST_ASSERT(groundImpact.hit);
    TEST_ASSERT(groundImpact.toi < 1.0f);
    TEST_ASSERT(groundImpact.normalY > 0.0f); // Points upward

    // 2. Sweep downward onto one-way platform at Row 4 (y = 4.0f..5.0f, top at y = 5.0f)
    SlabImpact2D oneWayImpact = spatial.SweepAabb(
        kNullEntity, 3.0f, 7.0f, halfW, halfH, 0.0f, -4.0f, 0xFFFF, false
    );
    TEST_ASSERT(oneWayImpact.hit);
    TEST_ASSERT(oneWayImpact.toi < 1.0f);
    TEST_ASSERT(oneWayImpact.normalY > 0.0f);

    // 3. Sweep downward onto one-way platform with ignoreOneWay = true: should pass through!
    SlabImpact2D ignoreImpact = spatial.SweepAabb(
        kNullEntity, 3.0f, 7.0f, halfW, halfH, 0.0f, -6.0f, 0xFFFF, true
    );
    // It should ignore row 4 one-way and hit the solid ground at y = 1.0f instead
    TEST_ASSERT(ignoreImpact.hit);
    TEST_ASSERT(ignoreImpact.toi < 1.0f);

    // 4. Raycast downward into ground
    RayHit2D rayDown = spatial.Raycast(3.0f, 5.0f, 0.0f, -1.0f, 10.0f, 0xFFFF);
    TEST_ASSERT(rayDown.hit);
    TEST_ASSERT(std::abs(rayDown.pointY - 1.0f) < 0.05f);
    TEST_ASSERT(rayDown.normalY > 0.0f);
}

void RegisterAllGameplayPools(World& world) {
    (void)world.RegisterComponentPool<LocalTransform2D>(64);
    (void)world.RegisterComponentPool<KinematicCarrier2D>(64);
    (void)world.RegisterComponentPool<KinematicMotor2D>(64);
    (void)world.RegisterComponentPool<MotionStack2D>(64);
    (void)world.RegisterComponentPool<StateMachine2D>(64);
    (void)world.RegisterComponentPool<HitboxRig2D>(64);
    (void)world.RegisterComponentPool<DamageReceiver2D>(64);
    (void)world.RegisterComponentPool<SafeGroundBreadcrumb2D>(64);
    (void)world.RegisterComponentPool<CameraController2D>(64);
}

void TestDynamicGridAndSoftPushbox() {
    World world;
    (void)world.Init({.maxEntities = 64});
    RegisterAllGameplayPools(world);

    SpatialIndex2D spatial;
    alignas(64) std::array<u8, 128 * 1024> arenaMemory{};
    spatial.BindArena(arenaMemory);

    // Spawn 2 actors overlapping horizontally
    EntityId a1 = *world.Spawn({.hasSpatial = true});
    EntityId a2 = *world.Spawn({.hasSpatial = true});

    world.SetLocalTransform(a1, LocalTransform2D{.x = 10.0f, .y = 5.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    world.SetLocalTransform(a2, LocalTransform2D{.x = 10.2f, .y = 5.0f, .scaleX = 1.0f, .scaleY = 1.0f});

    KinematicMotor2D m1{};
    m1.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 1.0f);
    m1.bodyLayer = 1;
    m1.solidMask = 1;
    m1.pushboxGroup = 1;
    m1.flags = MotorFlags::SoftPushbox;
    (void)world.Emplace<KinematicMotor2D>(a1, m1);

    KinematicMotor2D m2{};
    m2.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 1.0f);
    m2.bodyLayer = 1;
    m2.solidMask = 1;
    m2.pushboxGroup = 1;
    m2.flags = MotorFlags::SoftPushbox;
    (void)world.Emplace<KinematicMotor2D>(a2, m2);

    world.PropagateTransforms();

    // Rebuild dynamic counting-sort prefix-sum grid
    spatial.RebuildDynamicGrid(world);

    // Soft pushbox separation query
    f32 sepVx = 0.0f;
    spatial.ComputeSoftPushboxSeparation(a1, 1, 10.0f, 5.0f, 0.5f, 1.0f, sepVx);
    TEST_ASSERT(sepVx < 0.0f); // a1 pushed to the left away from a2
}

void TestCarrierAndRiderStep() {
    World world;
    (void)world.Init({.maxEntities = 64});
    RegisterAllGameplayPools(world);

    SpatialIndex2D spatial;
    alignas(64) std::array<u8, 64 * 1024> arenaMemory{};
    spatial.BindArena(arenaMemory);

    // Carrier platform
    EntityId carrier = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(carrier, LocalTransform2D{.x = 100.0f, .y = 50.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    (void)world.Emplace<KinematicCarrier2D>(carrier, KinematicCarrier2D{
        .deltaX = 2.5f,
        .deltaY = 0.0f,
        .bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 2.0f, 0.5f)
    });

    // Rider entity standing on carrier
    EntityId rider = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(rider, LocalTransform2D{.x = 100.0f, .y = 51.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    KinematicMotor2D riderMotor{};
    riderMotor.carrierId = carrier;
    riderMotor.contacts = ContactFlags::Ground;
    riderMotor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f);
    (void)world.Emplace<KinematicMotor2D>(rider, riderMotor);

    world.PropagateTransforms();

    time::ChannelModulator modulator;
    modulator.BakeTickState();
    const time::SimStepView step(1, 0, modulator);

    // Step carriers and riders
    KinematicSolver2D::StepCarriersAndRiders(world, spatial, step);

    // Carrier moves by deltaX (100.0f + 2.5f = 102.5f)
    // Rider on carrier receives the 2.5f displacement
    const auto& riderLt = world.GetLocalTransform(rider);
    TEST_ASSERT(std::abs(riderLt.x - 102.5f) < 0.01f);
}

void TestCombatOverlapAndResolution() {
    World world;
    (void)world.Init({.maxEntities = 64});
    RegisterAllGameplayPools(world);

    assets::AssetBank bank;
    (void)bank.Init();

    SpatialIndex2D spatial;
    alignas(64) std::array<u8, 64 * 1024> arenaMemory{};
    spatial.BindArena(arenaMemory);

    // Attacker
    EntityId attacker = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(attacker, LocalTransform2D{.x = 10.0f, .y = 10.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    HitboxRig2D attRig{};
    attRig.hitLayerMask = 0x01; // Strikes layer 1
    attRig.hurtLayerMask = 0x02;
    attRig.swingId = 1;
    attRig.primaryHitbox = CompactAabb2D::FromMeters(0.5f, 0.0f, 0.5f, 0.5f); // Hitbox from x=10.5..11.5
    attRig.rigFlags = static_cast<u8>(HitboxRigFlags::HitboxActive);
    (void)world.Emplace<HitboxRig2D>(attacker, attRig);

    // Defender
    EntityId defender = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(defender, LocalTransform2D{.x = 11.0f, .y = 10.0f, .scaleX = 1.0f, .scaleY = 1.0f});
    HitboxRig2D defRig{};
    defRig.hitLayerMask = 0x02;
    defRig.hurtLayerMask = 0x01; // Can be hurt by layer 1
    defRig.hurtbox = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 0.5f); // Hurtbox around x=10.5..11.5
    defRig.rigFlags = static_cast<u8>(HitboxRigFlags::HurtboxActive);
    (void)world.Emplace<HitboxRig2D>(defender, defRig);

    DamageReceiver2D defRecv{};
    defRecv.hp = 100;
    defRecv.maxHp = 100;
    defRecv.teamId = 2;
    (void)world.Emplace<DamageReceiver2D>(defender, defRecv);

    world.PropagateTransforms();
    spatial.RebuildDynamicGrid(world);

    // Gather combat overlaps
    std::array<ResolvedHitEvent, 16> outHits{};
    const u32 hitCount = spatial.GatherCombatOverlaps(world, bank, outHits);
    TEST_ASSERT(hitCount == 1);
    TEST_ASSERT(outHits[0].attacker == attacker);
    TEST_ASSERT(outHits[0].defender == defender);

    // Resolve hits through GameplayPipeline
    GameplayPipeline pipeline;
    (void)pipeline.Init(world);

    time::ChannelModulator modulator;
    modulator.BakeTickState();
    const time::SimStepView step(1, 0, modulator);

    GameplayPolicyTable policies{};
    StateAndCombatSolver::ResolveCombatHits(
        world, step, std::span<const ResolvedHitEvent>(outHits.data(), hitCount), policies, pipeline
    );

    // Defender took damage and iframe ticks were set
    const auto* recvAfter = world.TryGet<DamageReceiver2D>(defender);
    TEST_ASSERT(recvAfter != nullptr);
    TEST_ASSERT(recvAfter->hp < 100);
    TEST_ASSERT(recvAfter->iframeTicks > 0);

    // Attacker SplitMix Bloom mask has recorded this defender, so a subsequent gather in same swing is suppressed!
    const u32 secondHitCount = spatial.GatherCombatOverlaps(world, bank, outHits);
    TEST_ASSERT(secondHitCount == 0);
}

void TestGameplayPipelineLifecycleAndFlags() {
    World world;
    (void)world.Init({.maxEntities = 64});

    GameplayPipeline pipeline;
    auto initRes = pipeline.Init(world);
    TEST_ASSERT(initRes.has_value());

    // Persistent world flags (4096-bit bitset)
    TEST_ASSERT(!pipeline.TestWorldFlag(0));
    TEST_ASSERT(!pipeline.TestWorldFlag(42));
    TEST_ASSERT(!pipeline.TestWorldFlag(4095));

    pipeline.SetWorldFlag(42, true);
    pipeline.SetWorldFlag(1000, true);
    TEST_ASSERT(pipeline.TestWorldFlag(42));
    TEST_ASSERT(pipeline.TestWorldFlag(1000));
    TEST_ASSERT(!pipeline.TestWorldFlag(43));

    pipeline.SetWorldFlag(42, false);
    TEST_ASSERT(!pipeline.TestWorldFlag(42));
    TEST_ASSERT(pipeline.TestWorldFlag(1000));

    // SimCue emission and draining
    SimCue cue{};
    cue.kind = SimCueKind::CameraTraumaPulse;
    cue.worldX = 12.3f;
    cue.worldY = 45.6f;
    pipeline.EmitCue(cue);

    auto cues = pipeline.DrainCues();
    TEST_ASSERT(cues.size() == 1);
    TEST_ASSERT(cues[0].kind == SimCueKind::CameraTraumaPulse);
    TEST_ASSERT(std::abs(cues[0].worldX - 12.3f) < 0.001f);

    // Second drain is empty
    auto emptyCues = pipeline.DrainCues();
    TEST_ASSERT(emptyCues.empty());

    // Audit hash determinism
    const u64 hash1 = pipeline.ComputeAuditHash(world);
    const u64 hash2 = pipeline.ComputeAuditHash(world);
    TEST_ASSERT(hash1 == hash2);

    pipeline.Shutdown();
}

void TestKinematicMotorsAndMotionStacksStep() {
    World world;
    (void)world.Init({.maxEntities = 64});
    RegisterAllGameplayPools(world);

    SpatialIndex2D spatial;
    alignas(64) std::array<u8, 128 * 1024> arenaMemory{};
    spatial.BindArena(arenaMemory);

    // Mock ground at y = 0..1 (top at y = 1.0f)
    constexpr u16 kChunks = 1;
    constexpr u32 kTiles = 64;
    constexpr usize kBlobSize = sizeof(CookedCollisionMapHeader) +
                                sizeof(CookedCollisionChunk8x8) * kChunks +
                                sizeof(CookedTileCell) * kTiles;
    alignas(16) std::vector<u8> blobData(kBlobSize, 0);
    auto* hdr = reinterpret_cast<CookedCollisionMapHeader*>(blobData.data());
    hdr->magic = CookedCollisionMapHeader::kMagic;
    hdr->version = 1;
    hdr->tileSize_q8 = 256;
    hdr->chunkCountX = 1;
    hdr->chunkCountY = 1;
    hdr->fineTileTableOffset = sizeof(CookedCollisionMapHeader) + sizeof(CookedCollisionChunk8x8);

    auto* chunk = reinterpret_cast<CookedCollisionChunk8x8*>(blobData.data() + sizeof(CookedCollisionMapHeader));
    auto* tiles = reinterpret_cast<CookedTileCell*>(blobData.data() + hdr->fineTileTableOffset);
    for (int x = 0; x < 8; ++x) {
        tiles[x].shape = TileShape::FullSolid;
        chunk->solidBitmask |= (1ULL << x);
        chunk->pureFullBlockBitmask |= (1ULL << x);
    }
    assets::BlobRecord blob{};
    blob.data = blobData.data();
    blob.size = blobData.size();
    (void)spatial.BindCollisionMapBlob(blob);

    // Spawn falling actor at y = 3.0f (halfH = 0.5f, bottom at 2.5f)
    EntityId actor = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(actor, LocalTransform2D{.x = 3.0f, .y = 3.0f, .scaleX = 1.0f, .scaleY = 1.0f});

    KinematicMotor2D motor{};
    motor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.5f);
    motor.solidMask = 0xFFFF;
    motor.flags = MotorFlags::SnapToGround;
    (void)world.Emplace<KinematicMotor2D>(actor, motor);

    MotionStack2D motion{};
    motion.gravityScale_q8 = 256; // 1.0x gravity
    (void)world.Emplace<MotionStack2D>(actor, motion);

    world.PropagateTransforms();

    time::ChannelModulator modulator;
    modulator.BakeTickState();
    const time::SimStepView step(1, 0, modulator);

    // Step 1: In the air, gravity pulls actor downward
    KinematicSolver2D::StepMotorsAndMotionStacks(world, spatial, step);
    const auto* mAfter1 = world.TryGet<KinematicMotor2D>(actor);
    TEST_ASSERT(mAfter1 != nullptr);
    TEST_ASSERT(mAfter1->vy < 0.0f); // Falling

    // Step multiple ticks until actor hits the ground (60 ticks = 0.5s)
    for (int i = 0; i < 60; ++i) {
        KinematicSolver2D::StepMotorsAndMotionStacks(world, spatial, step);
    }

    const auto* mGrounded = world.TryGet<KinematicMotor2D>(actor);
    const auto& ltGrounded = world.GetLocalTransform(actor);
    TEST_ASSERT(mGrounded != nullptr);
    TEST_ASSERT(HasContact(mGrounded->contacts, ContactFlags::Ground));
    TEST_ASSERT(mGrounded->vy == 0.0f); // Landed cleanly

    // Actor's bottom should rest exactly on top of ground (y = 1.0f -> center at y = 1.5f)
    TEST_ASSERT(std::abs(ltGrounded.y - 1.5f) < 0.02f);
}

void TestFullGameplayPipelineTick() {
    World world;
    (void)world.Init({.maxEntities = 64});

    GameplayPipeline pipeline;
    auto initRes = pipeline.Init(world);
    TEST_ASSERT(initRes.has_value());

    assets::AssetBank bank;
    (void)bank.Init();

    time::ChannelModulator modulator;
    modulator.BakeTickState();
    const time::SimStepView step(1, 0, modulator);

    input::TickInput tickInput{};
    input::ActionBuffer actionBuffer{};

    // Spawn player entity with motor and motion stack
    EntityId player = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(player, LocalTransform2D{.x = 10.0f, .y = 5.0f, .scaleX = 1.0f, .scaleY = 1.0f});

    KinematicMotor2D motor{};
    motor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f);
    (void)world.Emplace<KinematicMotor2D>(player, motor);

    MotionStack2D motion{};
    motion.targetWalkVx = 4.0f;
    (void)world.Emplace<MotionStack2D>(player, motion);

    // Spawn dedicated camera entity tracking player
    EntityId camera = *world.Spawn({.hasSpatial = true});
    world.SetLocalTransform(camera, LocalTransform2D{.x = 10.0f, .y = 5.0f, .scaleX = 1.0f, .scaleY = 1.0f});

    CameraController2D cam{};
    cam.primaryTarget = player;
    cam.cameraFlags = static_cast<u8>(CameraCtrlFlags::None);
    (void)world.Emplace<CameraController2D>(camera, cam);

    world.PropagateTransforms();

    // Run 10 ticks of full gameplay pipeline
    for (int t = 0; t < 10; ++t) {
        pipeline.Tick(world, bank, step, tickInput, actionBuffer);
    }

    // Player walked forward
    const auto& playerLt = world.GetLocalTransform(player);
    TEST_ASSERT(playerLt.x > 10.0f);

    // Camera tracked player
    const auto* camAfter = world.TryGet<CameraController2D>(camera);
    TEST_ASSERT(camAfter != nullptr);
    TEST_ASSERT(camAfter->velX >= 0.0f);

    pipeline.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Gameplay Subsystem Tests...\n" << std::flush;

    std::cout << "  [1/8] TestGameplayTypesLayout...\n" << std::flush;
    TestGameplayTypesLayout();

    std::cout << "  [2/8] TestSpatialIndexSweepsAndRaycasts...\n" << std::flush;
    TestSpatialIndexSweepsAndRaycasts();

    std::cout << "  [3/8] TestDynamicGridAndSoftPushbox...\n" << std::flush;
    TestDynamicGridAndSoftPushbox();

    std::cout << "  [4/8] TestCarrierAndRiderStep...\n" << std::flush;
    TestCarrierAndRiderStep();

    std::cout << "  [5/8] TestCombatOverlapAndResolution...\n" << std::flush;
    TestCombatOverlapAndResolution();

    std::cout << "  [6/8] TestGameplayPipelineLifecycleAndFlags...\n" << std::flush;
    TestGameplayPipelineLifecycleAndFlags();

    std::cout << "  [7/8] TestKinematicMotorsAndMotionStacksStep...\n" << std::flush;
    TestKinematicMotorsAndMotionStacksStep();

    std::cout << "  [8/8] TestFullGameplayPipelineTick...\n" << std::flush;
    TestFullGameplayPipelineTick();

    std::cout << "Test Summary: " << g_testsPassed << " passed, " << g_testsFailed << " failed.\n" << std::flush;
    return g_testsFailed == 0 ? 0 : 1;
}

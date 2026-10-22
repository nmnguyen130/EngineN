#include "physics/Narrowphase2D.h"
#include "physics/PhysicsSystem.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace en::tests {

[[nodiscard]] constexpr bool VerifyConstexprPhysicsLayer1() noexcept {
    // 1. Verify exact byte layouts and zero-padding invariants
    static_assert(sizeof(CompactAabb2D) == 8 && alignof(CompactAabb2D) == 4);
    static_assert(sizeof(CollisionFilter2D) == 8 && alignof(CollisionFilter2D) == 4);
    static_assert(sizeof(Collider2D) == 24 && alignof(Collider2D) == 8);
    static_assert(sizeof(RigidBody2D) == 32 && alignof(RigidBody2D) == 32);
    static_assert(sizeof(CharacterMotor2D) == 32 && alignof(CharacterMotor2D) == 32);
    static_assert(sizeof(DistanceJoint2D) == 32 && alignof(DistanceJoint2D) == 32);
    static_assert(sizeof(RayHit2D) == 32 && alignof(RayHit2D) == 32);
    static_assert(sizeof(SweepHit2D) == 32 && alignof(SweepHit2D) == 32);
    static_assert(sizeof(ContactManifold2D) == 64 && alignof(ContactManifold2D) == 64);
    static_assert(sizeof(ContactEvent2D) == 32 && alignof(ContactEvent2D) == 32);
    static_assert(sizeof(PhysicsConfig) == 48 && alignof(PhysicsConfig) == 16);
    static_assert(sizeof(CookedCollisionMapHeader) == 32 && alignof(CookedCollisionMapHeader) == 16);
    static_assert(sizeof(SpatialBox2D) == 24 && alignof(SpatialBox2D) == 8);
    static_assert(sizeof(SpatialIndex2D) == 64 && alignof(SpatialIndex2D) == 64);
    static_assert(sizeof(PhysicsSystem) == 192 && alignof(PhysicsSystem) == 64);

    // 2. Verify CollisionFilter2D category/mask and Box2D groupIndex precedence
    constexpr CollisionFilter2D playerFilter{0x0002U, 0x0001U | 0x0004U, -5, 0U};
    constexpr CollisionFilter2D allyFilter{0x0002U, 0x0001U | 0x0002U, -5, 0U};
    constexpr CollisionFilter2D enemyFilter{0x0004U, 0x0002U, 0, 0U};
    static_assert(!playerFilter.ShouldCollide(allyFilter));
    static_assert(playerFilter.ShouldCollide(enemyFilter));

    // 3. Verify automatic mass and rotational inertia calculation from Collider2D density
    Collider2D boxCol{};
    boxCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 1.0f, 0.5f);
    RigidBody2D dynBody{};
    dynBody.ComputeMassFromCollider(boxCol, false);
    if (dynBody.invMass <= 0.49f || dynBody.invMass >= 0.51f || dynBody.invInertia <= 0.0f) {
        return false;
    }

    // 4. Verify constexpr RaycastAabb & SweepAabbVsAabb
    constexpr RayHit2D rHit = Narrowphase2D::RaycastAabb(0.0f, 0.0f, 1.0f, 0.0f, 10.0f, 4.0f, -1.0f, 6.0f, 1.0f);
    static_assert(rHit.Hit());
    static_assert(rHit.distance > 3.999f && rHit.distance < 4.001f);
    static_assert(rHit.normalX == -1.0f && rHit.normalY == 0.0f);

    constexpr SweepHit2D sHit = Narrowphase2D::SweepAabbVsAabb(
        0.0f, 5.0f, 0.5f, 0.5f, 0.0f, -4.0f, -5.0f, 0.0f, 5.0f, 2.0f);
    static_assert(sHit.Hit());
    static_assert(sHit.toi > 0.624f && sHit.toi < 0.626f);
    static_assert(sHit.normalY == 1.0f);

    // 5. Verify constexpr 2-point AABB manifold and speculative contact gap
    ContactManifold2D m{};
    if (!Narrowphase2D::CollideAabbAabb(0.0f, 1.9f, 0.5f, 0.5f, 0.0f, 1.0f, 2.0f, 0.5f, m, 0.0f)) {
        return false;
    }
    if (m.pointCount != 2U || m.normalY != -1.0f || m.separation[0] >= 0.0f) {
        return false;
    }

    ContactManifold2D specM{};
    if (!Narrowphase2D::CollideAabbAabb(0.0f, 2.3f, 0.5f, 0.5f, 0.0f, 1.0f, 2.0f, 0.5f, specM, 0.5f)) {
        return false;
    }
    if (specM.separation[0] <= 0.29f || specM.separation[0] >= 0.31f) {
        return false;
    }

    return true;
}

static_assert(VerifyConstexprPhysicsLayer1(), "Compile-time verification of Phase 8 Physics v2 Layer 1 failed.");

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

void TestSpatialIndexSegmentsRaycastAndShapeCast(TestSuite& suite) noexcept {
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());

    PhysicsSystem phys;
    EXPECT_TRUE(suite, phys.Init(world).has_value());

    // Build a 16x16 tilemap (tileSize = 1.0m):
    // Row y=0: Solid floor across x=[0..15]
    // Tile (8, 3): OneWay platform
    std::vector<TileMaterialFlags> tiles(16U * 16U, TileMaterialFlags::Empty);
    for (u32 x = 0U; x < 16U; ++x) {
        tiles[x] = TileMaterialFlags::Solid;
    }
    tiles[3U * 16U + 8U] = TileMaterialFlags::OneWay;

    EXPECT_TRUE(suite, phys.BuildTileMap(16U, 16U, 1.0f, 0.0f, 0.0f, tiles).has_value());
    EXPECT_EQ(suite, phys.Spatial().SampleTileAtWorld(2.5f, 0.5f), TileMaterialFlags::Solid);
    EXPECT_EQ(suite, phys.Spatial().SampleTileAtWorld(8.5f, 3.5f), TileMaterialFlags::OneWay);
    EXPECT_EQ(suite, phys.Spatial().SampleTileAtWorld(8.5f, 7.5f), TileMaterialFlags::Empty);

    // Spawn a dynamic circle collider entity at (6.0, 5.0) with radius 0.5
    const EntityId orb = world.Spawn(EntitySpawnDesc{.local = {6.0f, 5.0f}}).value_or(kNullEntity);
    Collider2D orbCol{};
    orbCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 0.5f);
    orbCol.shape = ShapeKind2D::Circle;
    orbCol.filter.categoryBits = 0x0002U;
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(orb, orbCol).has_value());

    // Spawn an arbitrary-angle static Segment ramp from (2.0, 1.0) to (6.0, 3.0)
    const EntityId ramp = world.Spawn(EntitySpawnDesc{.local = {0.0f, 0.0f}}).value_or(kNullEntity);
    Collider2D rampCol{};
    rampCol.localBounds = CompactAabb2D::FromSegment(2.0f, 1.0f, 6.0f, 3.0f);
    rampCol.shape = ShapeKind2D::Segment;
    rampCol.filter.categoryBits = 0x0004U;
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(ramp, rampCol).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 120.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;
    phys.Step(world, step);

    // Raycast downward into static floor at x=1.0 from y=5.0
    const RayHit2D floorRay = phys.Raycast(1.0f, 5.0f, 0.0f, -1.0f, 10.0f, 0x0001U);
    EXPECT_TRUE(suite, floorRay.Hit());
    EXPECT_NEAR(suite, floorRay.distance, 4.0f, 1e-3f);
    EXPECT_NEAR(suite, floorRay.normalY, 1.0f, 1e-3f);

    // Raycast downward into the arbitrary-angle Segment ramp at x=4.0 (where ramp y == 2.0) from y=5.0
    const RayHit2D segRay = phys.Raycast(4.0f, 5.0f, 0.0f, -1.0f, 10.0f, 0x0004U);
    EXPECT_TRUE(suite, segRay.Hit());
    EXPECT_EQ(suite, segRay.hitEntity, ramp);
    EXPECT_EQ(suite, segRay.shapeKind, ShapeKind2D::Segment);
    EXPECT_NEAR(suite, segRay.distance, 3.0f, 2e-2f);
    EXPECT_NEAR(suite, segRay.normalX, -0.44721f, 1e-2f);
    EXPECT_NEAR(suite, segRay.normalY,  0.89442f, 1e-2f);

    // Raycast horizontally into the circle entity at y=5.0 from x=2.0
    const RayHit2D orbRay = phys.Raycast(2.0f, 5.0f, 1.0f, 0.0f, 10.0f, 0x0002U);
    EXPECT_TRUE(suite, orbRay.Hit());
    EXPECT_EQ(suite, orbRay.hitEntity, orb);
    EXPECT_NEAR(suite, orbRay.distance, 3.5f, 1e-2f);

    // One-way platform sweep: moving UP through tile (8, 3) passes cleanly; moving DOWN from above lands on top
    const SweepHit2D upSweep = phys.SweepAabb(kNullEntity, 8.5f, 2.4f, 0.4f, 0.5f, 0.0f, 2.5f, 0x0001U, false);
    EXPECT_TRUE(suite, !upSweep.Hit());

    const SweepHit2D downSweep = phys.CastCircle(kNullEntity, 8.5f, 4.8f, 0.5f, 0.0f, -1.0f, 0x0001U, false);
    EXPECT_TRUE(suite, downSweep.Hit());
    EXPECT_TRUE(suite, downSweep.isOneWay != 0U);
    EXPECT_NEAR(suite, downSweep.normalY, 1.0f, 1e-3f);

    std::array<EntityId, 4> found{};
    EXPECT_EQ(suite, phys.QueryCircle(6.0f, 5.0f, 1.0f, 0x0002U, found), 1U);
    EXPECT_EQ(suite, found[0], orb);

    phys.Shutdown();
    world.Shutdown();
}

void TestCharacterMotorMoveAndSlideArbitrarySegmentSlopeAndKinematicCarrier(TestSuite& suite) noexcept {
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());

    PhysicsSystem phys;
    EXPECT_TRUE(suite, phys.Init(world).has_value());

    // Build a 16x8 tilemap with solid ground at y=0
    std::vector<TileMaterialFlags> tiles(16U * 8U, TileMaterialFlags::Empty);
    for (u32 x = 0U; x < 16U; ++x) {
        tiles[x] = TileMaterialFlags::Solid;
    }
    EXPECT_TRUE(suite, phys.BuildTileMap(16U, 8U, 1.0f, 0.0f, 0.0f, tiles).has_value());

    // Spawn an arbitrary-angle Segment slope from (2.5, 1.0) to (6.5, 2.5) (20.55 deg slope, zero hardcoded tile flags!)
    const EntityId slopeEdge = world.Spawn(EntitySpawnDesc{.local = {0.0f, 0.0f}}).value_or(kNullEntity);
    Collider2D slopeCol{};
    slopeCol.localBounds = CompactAabb2D::FromSegment(2.5f, 1.0f, 6.5f, 2.5f);
    slopeCol.shape = ShapeKind2D::Segment;
    slopeCol.filter.categoryBits = 0x0001U;
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(slopeEdge, slopeCol).has_value());

    // 1. Spawn a CharacterMotor2D at x=2.0 above the floor and walk up the Segment slope
    const EntityId hero = world.Spawn(EntitySpawnDesc{.local = {2.0f, 1.5f}}).value_or(kNullEntity);
    CharacterMotor2D motor{};
    motor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.35f, 0.5f);
    motor.vx = 6.0f;
    EXPECT_TRUE(suite, world.Emplace<CharacterMotor2D>(hero, motor).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 60.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;

    bool touchedSlope = false;
    for (u32 t = 0U; t < 30U; ++t) {
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
        const CharacterMotor2D* m = world.TryGet<CharacterMotor2D>(hero);
        if (m && HasPhysicsFlag(m->contacts, ContactFlags::OnSlope)) {
            touchedSlope = true;
        }
    }
    EXPECT_TRUE(suite, touchedSlope);
    EXPECT_TRUE(suite, world.GetWorldTransform(hero).y > 1.8f);

    // 2. Spawn a BodyType2D::Kinematic RigidBody2D + Collider2D platform and a rider standing on top of it
    const EntityId platform = world.Spawn(EntitySpawnDesc{.local = {10.0f, 3.0f}}).value_or(kNullEntity);
    Collider2D platCol{};
    platCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 1.5f, 0.25f);
    platCol.filter.categoryBits = 0x0001U;
    RigidBody2D platBody{};
    platBody.bodyType = BodyType2D::Kinematic;
    platBody.invMass = 0.0f;
    platBody.vx = 15.0f; // 0.25m per 1/60s tick
    platBody.vy = 6.0f;  // 0.10m per 1/60s tick
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(platform, platCol).has_value());
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(platform, platBody).has_value());

    const EntityId rider = world.Spawn(EntitySpawnDesc{.local = {10.0f, 3.75f}}).value_or(kNullEntity);
    CharacterMotor2D riderMotor{};
    riderMotor.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.35f, 0.5f);
    EXPECT_TRUE(suite, world.Emplace<CharacterMotor2D>(rider, riderMotor).has_value());

    world.BeginTick(step);
    phys.Step(world, step);
    world.EndTick(step);

    const CharacterMotor2D* rm = world.TryGet<CharacterMotor2D>(rider);
    EXPECT_TRUE(suite, rm != nullptr);
    EXPECT_EQ(suite, rm->carrierId, platform);
    EXPECT_TRUE(suite, rm->IsGrounded());
    EXPECT_NEAR(suite, world.GetWorldTransform(rider).x, 10.25f, 1e-2f);

    phys.Shutdown();
    world.Shutdown();
}

void TestRotationalTgsSolverObbClippingDistanceJointsAndSensors(TestSuite& suite) noexcept {
    // 1. Verify 2-point Sutherland-Hodgman OBB clipping
    ContactManifold2D obbManifold{};
    EXPECT_TRUE(suite, Narrowphase2D::CollideObbObb(
        0.0f, 1.9f, 1.0f, 0.5f, 1.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 0.0f,
        obbManifold, 0.0f));
    EXPECT_EQ(suite, obbManifold.pointCount, 2U);

    World world;
    EXPECT_TRUE(suite, world.Init().has_value());

    PhysicsSystem phys;
    PhysicsConfig cfg{};
    cfg.sleepTicksThreshold = 20U;
    EXPECT_TRUE(suite, phys.Init(world, cfg).has_value());

    std::vector<TileMaterialFlags> tiles(20U * 8U, TileMaterialFlags::Empty);
    for (u32 x = 0U; x < 20U; ++x) tiles[x] = TileMaterialFlags::Solid;
    EXPECT_TRUE(suite, phys.BuildTileMap(20U, 8U, 1.0f, 0.0f, 0.0f, tiles).has_value());

    // 2. Spawn a 3-box vertical stack at x=3.0 and verify stable settling under Soft-Step TGS
    EntityId stackBoxes[3]{};
    for (u32 i = 0U; i < 3U; ++i) {
        stackBoxes[i] = world.Spawn(EntitySpawnDesc{.local = {3.0f, 1.55f + static_cast<f32>(i) * 1.05f}}).value_or(kNullEntity);
        Collider2D col{};
        col.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 0.5f);
        col.SetFriction(0.5f);
        RigidBody2D rb{};
        rb.SetBoxMass(1.0f, 0.5f, 0.5f, true);
        EXPECT_TRUE(suite, world.Emplace<Collider2D>(stackBoxes[i], col).has_value());
        EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(stackBoxes[i], rb).has_value());
    }

    // 3. Spawn an off-center dynamic beam with fixedRotation = false dropping onto a narrow fulcrum at x=12.0
    const EntityId fulcrum = world.Spawn(EntitySpawnDesc{.local = {12.0f, 1.5f}}).value_or(kNullEntity);
    Collider2D fulcCol{};
    fulcCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.25f, 0.5f);
    RigidBody2D fulcRb{};
    fulcRb.bodyType = BodyType2D::Static;
    fulcRb.invMass = 0.0f;
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(fulcrum, fulcCol).has_value());
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(fulcrum, fulcRb).has_value());

    const EntityId beam = world.Spawn(EntitySpawnDesc{.local = {12.6f, 2.4f}}).value_or(kNullEntity);
    Collider2D beamCol{};
    beamCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.8f, 0.2f);
    beamCol.shape = ShapeKind2D::Obb;
    RigidBody2D beamRb{};
    beamRb.ComputeMassFromCollider(beamCol, false); // Rotational inertia enabled!
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(beam, beamCol).has_value());
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(beam, beamRb).has_value());

    // 4. Spawn a soft-spring DistanceJoint2D suspending a bob at x=16.0 below a static ceiling anchor at (16.0, 6.0)
    const EntityId anchor = world.Spawn(EntitySpawnDesc{.local = {16.0f, 6.0f}}).value_or(kNullEntity);
    RigidBody2D anchorRb{};
    anchorRb.bodyType = BodyType2D::Static;
    anchorRb.invMass = 0.0f;
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(anchor, anchorRb).has_value());

    const EntityId bob = world.Spawn(EntitySpawnDesc{.local = {16.0f, 4.5f}}).value_or(kNullEntity);
    Collider2D bobCol{};
    bobCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.25f, 0.25f);
    bobCol.shape = ShapeKind2D::Circle;
    RigidBody2D bobRb{};
    bobRb.ComputeMassFromCollider(bobCol, false);
    bobRb.SetLinearDamping(1.5f);
    DistanceJoint2D joint{};
    joint.entityA = anchor;
    joint.entityB = bob;
    joint.SetSoftSpring(1.5f, 10.0f, 1.0f);
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(bob, bobCol).has_value());
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(bob, bobRb).has_value());
    EXPECT_TRUE(suite, world.Emplace<DistanceJoint2D>(bob, joint).has_value());

    // 5. Spawn a sensor volume at (8.0, 3.2) and a bouncing ball dropping through it
    const EntityId sensorZone = world.Spawn(EntitySpawnDesc{.local = {8.0f, 3.2f}}).value_or(kNullEntity);
    Collider2D sensorCol{};
    sensorCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 1.0f, 0.4f);
    sensorCol.SetSensor(true);
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(sensorZone, sensorCol).has_value());

    const EntityId ball = world.Spawn(EntitySpawnDesc{.local = {8.0f, 4.5f}}).value_or(kNullEntity);
    Collider2D ballCol{};
    ballCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.4f);
    ballCol.shape = ShapeKind2D::Circle;
    ballCol.SetRestitution(0.45f);
    RigidBody2D ballRb{};
    ballRb.SetCircleMass(1.0f, 0.4f, true);
    EXPECT_TRUE(suite, world.Emplace<Collider2D>(ball, ballCol).has_value());
    EXPECT_TRUE(suite, world.Emplace<RigidBody2D>(ball, ballRb).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 120.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;

    bool sawSensorEnter = false;
    bool sawSensorExit = false;
    bool sawHitImpact = false;
    bool beamRotated = false;

    for (u32 t = 0U; t < 120U; ++t) {
        world.BeginTick(step);
        phys.Step(world, step);
        for (const ContactEvent2D& ev : phys.DrainContactEvents()) {
            if (ev.kind == ContactEventKind::SensorEnter) sawSensorEnter = true;
            if (ev.kind == ContactEventKind::SensorExit)  sawSensorExit = true;
            if (ev.kind == ContactEventKind::HitImpact)   sawHitImpact = true;
        }
        if (std::abs(world.GetWorldTransform(beam).worldRotRad) > 0.02f) {
            beamRotated = true;
        }
        world.EndTick(step);
    }

    EXPECT_TRUE(suite, sawSensorEnter);
    EXPECT_TRUE(suite, sawSensorExit);
    EXPECT_TRUE(suite, sawHitImpact);
    EXPECT_TRUE(suite, beamRotated); // Proves rotational r x n torque works!
    EXPECT_TRUE(suite, world.GetWorldTransform(bob).y > 3.5f); // Proves DistanceJoint2D holds the bob suspended!

    const f32 y0 = world.GetWorldTransform(stackBoxes[0]).y;
    const f32 y1 = world.GetWorldTransform(stackBoxes[1]).y;
    const f32 y2 = world.GetWorldTransform(stackBoxes[2]).y;
    EXPECT_TRUE(suite, y0 > 1.35f && y0 < 1.65f);
    EXPECT_TRUE(suite, y1 > y0 + 0.85f);
    EXPECT_TRUE(suite, y2 > y1 + 0.85f);

    phys.Shutdown();
    world.Shutdown();
}

void TestSpeculativeContactsBulletCcdAndDeterminism(TestSuite& suite) noexcept {
    const auto runSimulation = [&]() noexcept -> u64 {
        World world;
        (void)world.Init();
        PhysicsSystem phys;
        (void)phys.Init(world);

        const EntityId wall = world.Spawn(EntitySpawnDesc{.local = {5.0f, 2.0f}}).value_or(kNullEntity);
        Collider2D wallCol{};
        wallCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.10f, 2.0f);
        RigidBody2D wallRb{};
        wallRb.bodyType = BodyType2D::Static;
        wallRb.invMass = 0.0f;
        (void)world.Emplace<Collider2D>(wall, wallCol);
        (void)world.Emplace<RigidBody2D>(wall, wallRb);

        const EntityId bullet = world.Spawn(EntitySpawnDesc{.local = {4.20f, 2.0f}}).value_or(kNullEntity);
        Collider2D bulletCol{};
        bulletCol.localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.10f, 0.10f);
        bulletCol.SetRestitution(0.0f);
        RigidBody2D bulletRb{};
        bulletRb.SetBoxMass(0.5f, 0.10f, 0.10f, true);
        bulletRb.vx = 120.0f;
        bulletRb.flags |= BodyFlags2D::BulletCCD;
        bulletRb.flags &= ~BodyFlags2D::UseGravity;
        (void)world.Emplace<Collider2D>(bullet, bulletCol);
        (void)world.Emplace<RigidBody2D>(bullet, bulletRb);

        BakedTickState baked{};
        baked.scaledDt.fill(1.0f / 120.0f);
        baked.effectiveScale.fill(1.0f);
        SimStepView step{};
        step.baked = &baked;
        step.substepTotal = 1U;
        step.valid = true;

        for (u32 t = 0U; t < 5U; ++t) {
            world.BeginTick(step);
            phys.Step(world, step);
            world.EndTick(step);
        }

        EXPECT_TRUE(suite, phys.TotalSpeculativeHits() > 0ULL);
        EXPECT_TRUE(suite, world.GetWorldTransform(bullet).x <= 4.91f);

        const u64 audit = phys.ComputeAuditHash(world);
        phys.Shutdown();
        world.Shutdown();
        return audit;
    };

    const u64 hashRun1 = runSimulation();
    const u64 hashRun2 = runSimulation();
    EXPECT_EQ(suite, hashRun1, hashRun2);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};
    TestSpatialIndexSegmentsRaycastAndShapeCast(suite);
    TestCharacterMotorMoveAndSlideArbitrarySegmentSlopeAndKinematicCarrier(suite);
    TestRotationalTgsSolverObbClippingDistanceJointsAndSensors(suite);
    TestSpeculativeContactsBulletCcdAndDeterminism(suite);

    std::printf("Phase 8 Physics v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}

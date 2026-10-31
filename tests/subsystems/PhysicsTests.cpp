#include "tests/subsystems/CtsHarness.h"

#include "core/Math.h"
#include "core/Serialization.h"
#include "physics/PhysicsSystem.h"
#include "time/TimeSystem.h"

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

struct TestRng {
    u64 state{0x853C49E6748FEA9BULL};

    [[nodiscard]] constexpr u64 NextU64() noexcept {
        state = Mix64(state);
        return state;
    }
    [[nodiscard]] constexpr f32 Range(f32 lo, f32 hi) noexcept {
        const f32 t = static_cast<f32>(NextU64() & 0xFFFFFFULL) / static_cast<f32>(0xFFFFFFU);
        return lo + (hi - lo) * t;
    }
};

bool CtsNarrowphaseTilemapRunsAndOneWayPlatforms(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());

    std::array<TileFlag, 16U * 8U> tiles{};
    tiles.fill(TileFlag::Empty);
    for (u32 x = 0U; x < 16U; ++x) tiles[x] = TileFlag::Solid;
    for (u32 x = 4U; x <= 8U; ++x) tiles[3U * 16U + x] = TileFlag::OneWay;

    CTS_CHECK(r, phys.BuildTileMap(16U, 8U, 1.0f, {0.0f, 0.0f}, tiles).has_value());
    CTS_CHECK(r, phys.SampleTile(5, 0) == TileFlag::Solid);
    CTS_CHECK(r, phys.SampleTile(5, 3) == TileFlag::OneWay);
    CTS_CHECK(r, phys.SampleTileWorld({5.5f, 0.5f}) == TileFlag::Solid);

    const RayHit2D rHit = phys.Raycast({5.5f, 6.0f}, {0.0f, -1.0f}, 10.0f);
    CTS_CHECK(r, rHit && Near(rHit.point.y, 1.0f, 1e-3f) && Near(rHit.normal.y, 1.0f));

    const SweepResult2D upSweep = phys.SweepBox({5.0f, 1.5f, 0.8f, 1.0f}, {0.0f, 4.0f});
    CTS_CHECK(r, !upSweep);

    const SweepResult2D downSweep = phys.SweepBox({5.0f, 4.5f, 0.8f, 1.0f}, {0.0f, -2.0f});
    CTS_CHECK(r, downSweep && downSweep.oneWay == 1U && Near(downSweep.normal.y, 1.0f));
    CTS_CHECK(r, Near(downSweep.time, 0.25f, 1e-2f));

    const SweepResult2D dropSweep = phys.SweepBox({5.0f, 4.5f, 0.8f, 1.0f}, {0.0f, -2.0f}, 0xFFFFU, kNullEntity, true);
    CTS_CHECK(r, !dropSweep);

    const EntityId hero = *world.Spawn({.local = {.x = 1.5f, .y = 1.5f}});
    (void)world.Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.velocity = {10.0f, 0.0f}, .type = BodyType::Dynamic});

    TimeSystem time;
    for (u32 f = 0U; f < 60U; ++f) {
        time.BeginFrame(kFixedDtNs);
        while (const TickStep step = time.PollTick()) {
            world.BeginTick(step);
            phys.Step(world, step);
            world.EndTick(step);
        }
    }

    const WorldTransform2D wt = world.GetWorldTransform(hero);
    const PhysicsBody2D&   hb = *world.TryGet<PhysicsBody2D>(hero);
    CTS_CHECK(r, hb.IsGrounded() && !hb.TouchingWall());
    CTS_CHECK(r, wt.x > 6.0f && Near(wt.y, 1.5f, 2e-2f));
    return true;
}

bool CtsMovingPlatformRidersStepUpAndTriggers(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world).has_value());

    const EntityId plat = *world.Spawn({.local = {.x = 0.0f, .y = 0.0f}});
    (void)world.Emplace<Collider2D>(plat, Collider2D{.halfExtents = {2.0f, 0.5f}, .layerMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(plat, PhysicsBody2D{.velocity = {4.0f, 2.0f}, .type = BodyType::Kinematic});

    const EntityId rider = *world.Spawn({.local = {.x = 0.0f, .y = 1.0f}});
    (void)world.Emplace<Collider2D>(rider, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 0xFFFFU});
    (void)world.Emplace<PhysicsBody2D>(rider, PhysicsBody2D{.type = BodyType::Dynamic});

    const EntityId trig = *world.Spawn({.local = {.x = 2.0f, .y = 2.0f}});
    (void)world.Emplace<Collider2D>(trig, Collider2D{
        .halfExtents = {0.8f, 0.8f},
        .layerMask   = 4U,
        .queryMask   = 2U,
        .isTrigger   = 1U,
        .userTag     = 77U,
    });

    u32 trigEnters = 0U;
    u32 trigExits  = 0U;
    TimeSystem time;
    for (u32 f = 0U; f < 120U; ++f) {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);

        for (const ContactEvent2D& ev : phys.DrainContactEvents()) {
            if (ev.kind == ContactEventKind::TriggerEnter && ev.userTag == 77U) ++trigEnters;
            if (ev.kind == ContactEventKind::TriggerExit && ev.userTag == 77U) ++trigExits;
        }
    }

    const WorldTransform2D platWt  = world.GetWorldTransform(plat);
    const WorldTransform2D riderWt = world.GetWorldTransform(rider);
    const PhysicsBody2D&   riderBd = *world.TryGet<PhysicsBody2D>(rider);
    CTS_CHECK(r, Near(platWt.x, 4.0f, 2e-2f) && Near(platWt.y, 2.0f, 2e-2f));
    CTS_CHECK(r, Near(riderWt.x, 4.0f, 5e-2f) && Near(riderWt.y, 3.0f, 5e-2f));
    CTS_CHECK(r, riderBd.IsGrounded() && riderBd.carrier == plat);
    CTS_CHECK(r, trigEnters == 1U && trigExits == 1U);

    // Moving solid lateral push and crush against a static wall.
    world.Clear();
    phys.Clear();

    const EntityId wall = *world.Spawn({.local = {.x = 4.0f, .y = 1.0f}});
    (void)world.Emplace<Collider2D>(wall, Collider2D{.halfExtents = {0.5f, 3.0f}, .layerMask = 1U, .isStatic = 1U});

    const EntityId pusher = *world.Spawn({.local = {.x = 0.0f, .y = 1.0f}});
    (void)world.Emplace<Collider2D>(pusher, Collider2D{.halfExtents = {1.0f, 1.0f}, .layerMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(pusher, PhysicsBody2D{.velocity = {6.0f, 0.0f}, .type = BodyType::Kinematic});

    const EntityId victim = *world.Spawn({.local = {.x = 1.6f, .y = 1.0f}});
    (void)world.Emplace<Collider2D>(victim, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(victim, PhysicsBody2D{.gravityScale = 0.0f, .type = BodyType::Dynamic});

    bool crushedSeen = false;
    for (u32 f = 0U; f < 60U; ++f) {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
        if (world.TryGet<PhysicsBody2D>(victim)->IsCrushed()) {
            crushedSeen = true;
            break;
        }
    }
    CTS_CHECK(r, world.GetWorldTransform(victim).x > 2.5f && crushedSeen);
    return true;
}

bool CtsLedgeStepUpSweepMoveCornerCorrectionAndQueries(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    PhysicsSystem phys;
    CTS_CHECK(r, phys.Init(world, {.maxStepHeight = 0.25f, .groundSnapDist = 0.15f, .cornerCorrectionDist = 0.20f}).has_value());

    const EntityId floorEnt = *world.Spawn({.local = {.x = 0.0f, .y = -0.5f}});
    (void)world.Emplace<Collider2D>(floorEnt, Collider2D{.halfExtents = {10.0f, 0.5f}, .layerMask = 1U, .isStatic = 1U});

    const EntityId stepEnt = *world.Spawn({.local = {.x = 4.0f, .y = 0.09f}});
    (void)world.Emplace<Collider2D>(stepEnt, Collider2D{.halfExtents = {2.0f, 0.09f}, .layerMask = 1U, .isStatic = 1U});

    const EntityId walker = *world.Spawn({.local = {.x = 1.0f, .y = 0.5f}});
    (void)world.Emplace<Collider2D>(walker, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
    (void)world.Emplace<PhysicsBody2D>(walker, PhysicsBody2D{.velocity = {6.0f, 0.0f}, .type = BodyType::Dynamic});

    TimeSystem time;
    for (u32 f = 0U; f < 40U; ++f) {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
    }

    const WorldTransform2D walkerWt = world.GetWorldTransform(walker);
    CTS_CHECK(r, walkerWt.x > 2.5f && Near(walkerWt.y, 0.68f, 3e-2f));

    std::array<EntityId, 8> hits{};
    const u32 boxCount = phys.QueryBox({0.0f, -1.0f, 6.0f, 3.0f}, 0xFFFFU, hits);
    CTS_CHECK(r, boxCount == 3U);
    const u32 circleCount = phys.QueryCircle({walkerWt.x, walkerWt.y}, 0.6f, 2U, hits);
    CTS_CHECK(r, circleCount == 1U && hits[0] == walker);

    // Verify RaycastAll multi-hit sorted query.
    std::array<RayHit2D, 8> rayHits{};
    const u32 rCount = phys.RaycastAll({-5.0f, 0.5f}, {1.0f, 0.0f}, 20.0f, 0xFFFFU, rayHits);
    CTS_CHECK(r, rCount >= 1U && rayHits[0].entity == walker);

    // Verify SpatialGrid2D-accelerated PhysicsSystem::SweepMove (replaces legacy O(N) WorldSystems::SweepMove).
    const EntityId mover = *world.Spawn({.local = {.x = 12.0f, .y = 0.0f}});
    const EntityId wall  = *world.Spawn({.local = {.x = 17.0f, .y = 0.0f}});
    (void)world.Emplace<Collider2D>(mover, Collider2D{.halfExtents = {1.0f, 1.0f}, .layerMask = 2U, .queryMask = 4U});
    (void)world.Emplace<Collider2D>(wall, Collider2D{.halfExtents = {1.0f, 5.0f}, .layerMask = 4U, .isStatic = 1U});

    const SweepResult2D smHit = phys.SweepMove(world, mover, {6.0f, 3.0f});
    CTS_CHECK(r, smHit && smHit.entity == wall && smHit.normal == Vec2{-1.0f, 0.0f});
    const WorldTransform2D moverWt = world.GetWorldTransform(mover);
    CTS_CHECK(r, Near(moverWt.x, 15.0f, 2e-2f) && Near(moverWt.y, 3.0f, 2e-2f));

    // Verify upward jump ceiling corner correction: jumper slightly overlaps ceiling right edge by 0.10m (< 0.20m).
    const EntityId ceilBlock = *world.Spawn({.local = {.x = 20.0f, .y = 4.0f}});
    (void)world.Emplace<Collider2D>(ceilBlock, Collider2D{.halfExtents = {1.0f, 0.5f}, .layerMask = 8U, .isStatic = 1U});
    const EntityId jumper = *world.Spawn({.local = {.x = 21.3f, .y = 2.5f}});
    (void)world.Emplace<Collider2D>(jumper, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 8U});
    (void)world.Emplace<PhysicsBody2D>(jumper, PhysicsBody2D{.velocity = {0.0f, 18.0f}, .gravityScale = 0.0f, .type = BodyType::Dynamic});

    for (u32 f = 0U; f < 20U; ++f) {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
    }
    const WorldTransform2D jumperWt = world.GetWorldTransform(jumper);
    // Corner correction nudges jumper right past x=21.4m so it continues upward past the ceiling at y=4.0m!
    CTS_CHECK(r, jumperWt.x >= 21.4f && jumperWt.y > 4.5f);

    // Verify airborne Ledge Hop (ground corner correction): jumping actor clipping top 0.10m of a ledge pops onto it.
    const EntityId ledge = *world.Spawn({.local = {.x = 30.0f, .y = 0.0f}});
    (void)world.Emplace<Collider2D>(ledge, Collider2D{.halfExtents = {1.0f, 1.0f}, .layerMask = 16U, .isStatic = 1U});
    const EntityId hopper = *world.Spawn({.local = {.x = 28.5f, .y = 1.40f}});
    (void)world.Emplace<Collider2D>(hopper, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 16U});
    (void)world.Emplace<PhysicsBody2D>(hopper, PhysicsBody2D{.velocity = {6.0f, 0.5f}, .gravityScale = 0.0f, .type = BodyType::Dynamic});
    for (u32 f = 0U; f < 20U; ++f) {
        time.BeginFrame(kFixedDtNs);
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
    }
    CTS_CHECK(r, world.GetWorldTransform(hopper).x > 29.0f && world.GetWorldTransform(hopper).y >= 1.49f);

    // Verify Sort-and-Unique GatherContactEvents: large multi-cell overlapping colliders emit exactly 1 BeginContact.
    const EntityId bigA = *world.Spawn({.local = {.x = -20.0f, .y = 0.0f}});
    const EntityId bigB = *world.Spawn({.local = {.x = -20.0f, .y = 0.0f}});
    (void)world.Emplace<Collider2D>(bigA, Collider2D{.halfExtents = {3.5f, 3.5f}, .layerMask = 32U, .queryMask = 32U});
    (void)world.Emplace<Collider2D>(bigB, Collider2D{.halfExtents = {3.5f, 3.5f}, .layerMask = 32U, .queryMask = 32U});
    time.BeginFrame(kFixedDtNs);
    {
        const TickStep step = time.PollTick();
        world.BeginTick(step);
        phys.Step(world, step);
        world.EndTick(step);
    }
    u32 bigPairEvents = 0U;
    for (const ContactEvent2D& ev : phys.ContactEvents()) {
        if ((ev.entityA == bigA && ev.entityB == bigB) || (ev.entityA == bigB && ev.entityB == bigA)) {
            ++bigPairEvents;
        }
    }
    CTS_CHECK(r, bigPairEvents == 1U);

    AssetBank bank;
    CTS_CHECK(r, bank.Init().has_value());
    const u8 scope = *bank.BeginScope();

    std::array<u8, sizeof(TileMapHeader) + 16U> blobBuf{};
    BinaryWriter writer(blobBuf);
    CTS_CHECK(r, writer.Write(TileMapHeader{.width = 4U, .height = 4U, .tileSize = 1.0f}));
    for (u32 i = 0U; i < 16U; ++i) {
        CTS_CHECK(r, writer.Write(i < 4U ? TileFlag::Solid : TileFlag::Empty));
    }
    const BlobHandle goodBlob = bank.RegisterBlob("maps/test.col"_aid, scope, writer.Written());
    CTS_CHECK(r, phys.BindCollisionMapBlob(bank, goodBlob).has_value());
    CTS_CHECK(r, phys.SampleTile(2, 0) == TileFlag::Solid && phys.SampleTile(2, 1) == TileFlag::Empty);

    blobBuf[0] = 0xFFU;
    const BlobHandle badBlob = bank.RegisterBlob("maps/bad.col"_aid, scope, blobBuf);
    CTS_CHECK(r, phys.BindCollisionMapBlob(bank, badBlob).error() == PhysicsErrc::CorruptedBlob);
    return true;
}

bool DstDeterministicLockstepPhysicsStress(CtsRunner& r) noexcept {
    const auto buildSim = [](World& w, PhysicsSystem& p) noexcept -> bool {
        if (!w.Init({.maxEntities = 256U, .defaultPoolCapacity = 256U}).has_value()) return false;
        if (!p.Init(w).has_value()) return false;

        std::array<TileFlag, 32U * 16U> tiles{};
        tiles.fill(TileFlag::Empty);
        for (u32 x = 0U; x < 32U; ++x) {
            tiles[x]             = TileFlag::Solid;
            tiles[15U * 32U + x] = TileFlag::Solid;
        }
        for (u32 y = 0U; y < 16U; ++y) {
            tiles[y * 32U]       = TileFlag::Solid;
            tiles[y * 32U + 31U] = TileFlag::Solid;
        }
        if (!p.BuildTileMap(32U, 16U, 1.0f, {0.0f, 0.0f}, tiles).has_value()) return false;

        TestRng rng{};
        for (u32 i = 0U; i < 32U; ++i) {
            const EntityId id = *w.Spawn({
                .local = {.x = rng.Range(3.0f, 28.0f), .y = rng.Range(3.0f, 12.0f)},
            });
            (void)w.Emplace<Collider2D>(id, Collider2D{
                .halfExtents = {0.35f, 0.35f},
                .layerMask   = 2U,
                .queryMask   = 3U,
            });
            (void)w.Emplace<PhysicsBody2D>(id, PhysicsBody2D{
                .velocity    = {rng.Range(-12.0f, 12.0f), rng.Range(-8.0f, 12.0f)},
                .restitution = 0.5f,
                .type        = BodyType::Dynamic,
            });
        }
        return true;
    };

    World         wA, wB;
    PhysicsSystem pA, pB;
    CTS_CHECK(r, buildSim(wA, pA) && buildSim(wB, pB));

    TimeSystem tA, tB;
    for (u32 tick = 0U; tick < 300U; ++tick) {
        tA.BeginFrame(kFixedDtNs);
        tB.BeginFrame(kFixedDtNs);
        const TickStep sA = tA.PollTick();
        const TickStep sB = tB.PollTick();

        wA.BeginTick(sA);
        pA.Step(wA, sA);
        wA.EndTick(sA);

        wB.BeginTick(sB);
        pB.Step(wB, sB);
        wB.EndTick(sB);

        CTS_CHECK(r, pA.ComputeAuditHash(wA) == pB.ComputeAuditHash(wB));
        CTS_CHECK(r, wA.ComputeAuditHash() == wB.ComputeAuditHash());
    }

    r.RecordFuzzOps(300U * 32U);
    r.MixDigest(pA.ComputeAuditHash(wA));
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "PhysicsTests (CTS+DST)"};
    runner.RunCase("Narrowphase, Tilemap Runs & One-Way Platforms", CtsNarrowphaseTilemapRunsAndOneWayPlatforms);
    runner.RunCase("Moving Platform Riders, Push/Crush & Triggers", CtsMovingPlatformRidersStepUpAndTriggers);
    runner.RunCase("Ledge Step-Up, SweepMove, Corner Correction & Queries", CtsLedgeStepUpSweepMoveCornerCorrectionAndQueries);
    runner.RunCase("DST: 300-Tick Lockstep Multi-Body Stress", DstDeterministicLockstepPhysicsStress);
    return runner.Finish();
}

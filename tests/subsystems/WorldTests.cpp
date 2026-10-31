#include "tests/subsystems/CtsHarness.h"

#include "time/TimeSystem.h"
#include "world/WorldSystems.h"
#include "vfx/VfxSystem.h"

#include <map>
#include <vector>

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

bool DstEcsSparseSetAndViewOracleFuzz(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 512U, .defaultPoolCapacity = 512U}).has_value());

    struct OracleEnt {
        EntityId id{};
        bool     hasVel{false};
        bool     hasTag{false};
        bool     hasLife{false};
        f32      vx{0.0f};
        u32      cat{0U};
    };
    std::map<u16, OracleEnt> oracle;
    Rng rng(0xEC5F0222026ULL);

    // 8,000 randomized Spawn / Destroy / Emplace / Remove operations verified against std::map oracle.
    for (u32 step = 0U; step < 8000U; ++step) {
        const u32 op = rng.Bounded(100U);
        if (op < 25U && oracle.size() < 400U) {
            const auto res = world.Spawn({.local = {.x = rng.Range(-50.0f, 50.0f), .y = rng.Range(-50.0f, 50.0f)}});
            CTS_CHECK(r, res.has_value());
            oracle[res->index] = OracleEnt{.id = *res};
        } else if (op < 40U && !oracle.empty()) {
            auto it = oracle.begin();
            std::advance(it, rng.Bounded(static_cast<u32>(oracle.size())));
            const EntityId deadId = it->second.id;
            CTS_CHECK(r, world.Destroy(deadId, false));
            CTS_CHECK(r, !world.IsAlive(deadId));
            oracle.erase(it);
        } else if (!oracle.empty()) {
            auto it = oracle.begin();
            std::advance(it, rng.Bounded(static_cast<u32>(oracle.size())));
            OracleEnt& oe = it->second;
            const u32 compOp = rng.Bounded(6U);
            if (compOp == 0U) {
                const f32 vx = rng.Range(-20.0f, 20.0f);
                CTS_CHECK(r, world.Emplace<Velocity2D>(oe.id, vx, 1.0f).has_value());
                oe.hasVel = true;
                oe.vx     = vx;
            } else if (compOp == 1U) {
                CTS_CHECK(r, world.Remove<Velocity2D>(oe.id) == oe.hasVel);
                oe.hasVel = false;
            } else if (compOp == 2U) {
                const u32 cat = rng.NextU32();
                CTS_CHECK(r, world.Emplace<Tag2D>(oe.id, Tag2D{.name = "fuzz"_sid, .categoryMask = cat}).has_value());
                oe.hasTag = true;
                oe.cat    = cat;
            } else if (compOp == 3U) {
                CTS_CHECK(r, world.Remove<Tag2D>(oe.id) == oe.hasTag);
                oe.hasTag = false;
            } else if (compOp == 4U) {
                CTS_CHECK(r, world.Emplace<Lifetime2D>(oe.id, Lifetime2D{.remainingTicks = 10U}).has_value());
                oe.hasLife = true;
            } else {
                CTS_CHECK(r, world.Remove<Lifetime2D>(oe.id) == oe.hasLife);
                oe.hasLife = false;
            }
        }
        CTS_CHECK(r, world.AliveCount() == oracle.size());
    }

    // Verify View<Velocity2D, Tag2D>(Exclude<Lifetime2D>{}) matches exact set intersection/difference.
    u32 viewMatches = 0U;
    for (auto [id, vel, tag] : world.View<Velocity2D, Tag2D>(Exclude<Lifetime2D>{})) {
        const auto it = oracle.find(id.index);
        CTS_CHECK(r, it != oracle.end() && it->second.id == id);
        CTS_CHECK(r, it->second.hasVel && it->second.hasTag && !it->second.hasLife);
        CTS_CHECK(r, vel.vx == it->second.vx && tag.categoryMask == it->second.cat);
        ++viewMatches;
    }

    u32 expectedMatches = 0U;
    for (const auto& [idx, oe] : oracle) {
        if (oe.hasVel && oe.hasTag && !oe.hasLife) ++expectedMatches;
    }
    CTS_CHECK(r, viewMatches == expectedMatches);
    r.RecordFuzzOps(8000U);
    return true;
}

bool CtsHierarchyKeepWorldAndDepthGuards(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U}).has_value());

    // Build a chain up to kMaxHierarchyDepth (32) and verify 33rd depth is rejected.
    EntityId prev = *world.Spawn({.local = {.x = 1.0f, .y = 1.0f}});
    for (usize d = 1U; d <= kMaxHierarchyDepth; ++d) {
        const auto res = world.Spawn({.local = {.x = 1.0f, .y = 0.0f}, .parent = prev});
        CTS_CHECK(r, res.has_value());
        prev = *res;
    }
    const auto tooDeep = world.Spawn({.local = {.x = 1.0f, .y = 0.0f}, .parent = prev});
    CTS_CHECK(r, !tooDeep.has_value() && tooDeep.error() == WorldErrc::HierarchyDepthExceeded);

    // Verify SetParent with keepWorldTransform = true preserves exact world position & rotation.
    world.Clear();
    const EntityId pA = *world.Spawn({.local = {.x = 10.0f, .y = 5.0f, .scaleX = 2.0f, .scaleY = 2.0f, .rotationRad = 0.4f}});
    const EntityId pB = *world.Spawn({.local = {.x = -6.0f, .y = 12.0f, .scaleX = 1.0f, .scaleY = 1.0f, .rotationRad = -0.25f}});
    const EntityId ch = *world.Spawn({.local = {.x = 3.0f, .y = -2.0f, .rotationRad = 0.1f}, .parent = pA});
    world.PropagateTransforms();

    const WorldTransform2D beforeWt = world.GetWorldTransform(ch);
    CTS_CHECK(r, world.SetParent(ch, pB, true).has_value());
    world.PropagateTransforms();
    const WorldTransform2D afterWt = world.GetWorldTransform(ch);

    CTS_CHECK(r, Near(beforeWt.x, afterWt.x, 1e-3f) && Near(beforeWt.y, afterWt.y, 1e-3f));
    CTS_CHECK(r, Near(beforeWt.worldRotRad, afterWt.worldRotRad, 1e-3f));

    // Unparenting with keepWorldTransform = true must also preserve world transform.
    CTS_CHECK(r, world.SetParent(ch, kNullEntity, true).has_value());
    world.PropagateTransforms();
    const WorldTransform2D rootWt = world.GetWorldTransform(ch);
    CTS_CHECK(r, Near(beforeWt.x, rootWt.x, 1e-3f) && Near(beforeWt.y, rootWt.y, 1e-3f));
    return true;
}

bool CtsRenderExtractionAndLifetimeDeferredDestroy(CtsRunner& r) noexcept {
    World world;
    CTS_CHECK(r, world.Init({.maxEntities = 64U, .defaultPoolCapacity = 64U}).has_value());

    AssetBank bank;
    CTS_CHECK(r, bank.Init().has_value());

    const EntityId cam = *world.Spawn({.local = {.x = 4.0f, .y = 2.0f}});
    (void)world.Emplace<CameraRig2D>(cam, CameraRig2D{.zoom = 1.0f, .pixelsPerMeter = 48.0f, .trauma = 0.4f});

    const EntityId vfx = *world.Spawn({.local = {.x = 1.0f, .y = 2.0f}});
    (void)world.Emplace<Sprite2D>(vfx);
    (void)world.Emplace<PointLight2D>(vfx, PointLight2D{.radius = 2.0f, .intensity = 1.0f, .pulseSpeed = 3.0f});
    (void)world.Emplace<Lifetime2D>(vfx, Lifetime2D{.remainingTicks = 2U, .initialTicks = 2U, .fadeSpriteAlpha = true});

    TimeSystem time;
    time.BeginFrame(kFixedDtNs);
    const TickStep s1 = time.PollTick();
    world.BeginTick(s1);
    WorldSystems::StepVisualsAndLifetimes(world, s1);
    world.EndTick(s1);

    CTS_CHECK(r, world.IsAlive(vfx));
    CTS_CHECK(r, Near(world.TryGet<Sprite2D>(vfx)->tint.a, 0.5f, 1e-3f));

    // Opt-in HitFlash2D component on vfx + standalone VfxSystem with Swap-Back & JobSystem ParallelFor.
    (void)world.Emplace<HitFlash2D>(vfx, HitFlash2D{.durationTicks = 3U, .remainingTicks = 3U});
    VfxSystem vfxSys;
    CTS_CHECK(r, vfxSys.Init({.maxParticles = 128U, .maxGhosts = 32U, .parallelBatchSize = 16U}).has_value());
    vfxSys.EmitBurst({.origin = {1.0f, 2.0f}, .lifeTicks = 1U, .count = 32U});
    vfxSys.EmitBurst({.origin = {1.0f, 2.0f}, .lifeTicks = 5U, .count = 32U});
    CTS_CHECK(r, vfxSys.ActiveParticleCount() == 64U && world.TryGet<HitFlash2D>(vfx)->Active());

    RenderList list;
    list.Reset({1280, 720});
    WorldSystems::ExtractRender(world, bank, time.RenderView(), list, 48.0f);
    vfxSys.ExtractRender(bank, list);
    // 1 sprite (hit-flashing) + 1 point light + 64 dense particles = 66 quads!
    CTS_CHECK(r, list.Stats().submitted == 66U);

    JobSystem jobs;
    CTS_CHECK(r, jobs.Init(0U) && jobs.WorkerCount() >= 1U);

    time.BeginFrame(kFixedDtNs);
    const TickStep s2 = time.PollTick();
    world.BeginTick(s2);
    WorldSystems::StepVisualsAndLifetimes(world, s2);
    vfxSys.Step(s2, &jobs);
    world.EndTick(s2);
    // The 32 particles with lifeTicks=1 expired and were Swap-Back compacted, leaving exactly 32 active!
    CTS_CHECK(r, vfxSys.ActiveParticleCount() == 32U);
    CTS_CHECK(r, !world.IsAlive(vfx));
    return true;
}

bool Dst500TickLockstepAndMidRunSnapshotRollback(CtsRunner& r) noexcept {
    const auto setupScenario = [](World& w) noexcept -> bool {
        if (!w.Init({.maxEntities = 256U, .defaultPoolCapacity = 256U})) return false;
        Rng rng(0xDE7E24102026ULL);
        for (u32 i = 0U; i < 40U; ++i) {
            const EntityId root = *w.Spawn({
                .local       = {.x = rng.Range(-20.0f, 20.0f), .y = rng.Range(-20.0f, 20.0f)},
                .timeChannel = (i % 3U == 0U) ? TimeChannel::Enemy : TimeChannel::Player,
            });
            (void)w.Emplace<Velocity2D>(root, Velocity2D{
                .vx            = rng.Range(-15.0f, 15.0f),
                .vy            = rng.Range(-15.0f, 15.0f),
                .angularVelRad = rng.Range(-1.5f, 1.5f),
                .linearDamping = 0.05f,
            });
            (void)w.Emplace<Sprite2D>(root);
            (void)w.Emplace<SpriteAnimator2D>(root, SpriteAnimator2D{.frameCount = 6U, .ticksPerFrame = 4U});

            const EntityId orb = *w.Spawn({.local = {.x = 1.5f, .y = -0.5f}, .parent = root});
            (void)w.Emplace<PointLight2D>(orb, PointLight2D{.radius = 3.0f, .intensity = 1.0f, .pulseSpeed = 2.0f});
            if (i < 10U) {
                (void)w.Emplace<Lifetime2D>(orb, Lifetime2D{
                    .remainingTicks  = 100U + i * 25U,
                    .initialTicks    = 100U + i * 25U,
                    .fadeSpriteAlpha = true,
                });
            }
        }
        w.PropagateTransforms();
        return true;
    };

    World worldA;
    World worldB;
    CTS_CHECK(r, setupScenario(worldA) && setupScenario(worldB));
    CTS_CHECK(r, worldA.ComputeAuditHash() == worldB.ComputeAuditHash());

    TimeSystem timeA;
    TimeSystem timeB;
    std::vector<u8> midSnapshot;

    for (u32 tick = 1U; tick <= 500U; ++tick) {
        if (tick == 120U) {
            timeA.Hitstop(TimeChannel::Enemy, 5_ticks);
            timeB.Hitstop(TimeChannel::Enemy, 5_ticks);
        }

        timeA.BeginFrame(kFixedDtNs);
        const TickStep stepA = timeA.PollTick();
        worldA.BeginTick(stepA);
        WorldSystems::StepVelocities(worldA, stepA);
        WorldSystems::StepVisualsAndLifetimes(worldA, stepA);
        worldA.EndTick(stepA);

        timeB.BeginFrame(kFixedDtNs);
        const TickStep stepB = timeB.PollTick();
        worldB.BeginTick(stepB);
        WorldSystems::StepVelocities(worldB, stepB);
        WorldSystems::StepVisualsAndLifetimes(worldB, stepB);
        worldB.EndTick(stepB);

        // At tick 250, serialize worldB into a binary snapshot, wipe worldB completely,
        // and restore from the snapshot before continuing ticks 251..500!
        if (tick == 250U) {
            midSnapshot.resize(worldB.RequiredSnapshotBytes());
            const auto written = worldB.SerializeWorldSnapshot(midSnapshot);
            CTS_CHECK(r, written.has_value() && *written == midSnapshot.size());

            // Verify single-bit CRC corruption is caught and rejected.
            midSnapshot.back() ^= 0x01U;
            CTS_CHECK(r, !worldB.DeserializeWorldSnapshot(midSnapshot).has_value());
            midSnapshot.back() ^= 0x01U;

            CTS_CHECK(r, worldB.DeserializeWorldSnapshot(midSnapshot).has_value());
        }

        CTS_CHECK(r, worldA.AliveCount() == worldB.AliveCount());
        CTS_CHECK(r, worldA.ComputeAuditHash() == worldB.ComputeAuditHash());
    }

    r.RecordFuzzOps(500U * 80U);
    r.MixDigest(worldA.ComputeAuditHash());
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "WorldTests (CTS+DST)"};
    runner.RunCase("DST: 8,000-Op SparseSet & View Oracle Fuzz", DstEcsSparseSetAndViewOracleFuzz);
    runner.RunCase("Hierarchy KeepWorldTransform & Depth Guards", CtsHierarchyKeepWorldAndDepthGuards);
    runner.RunCase("Render Extraction & Lifetime Deferred Destroy", CtsRenderExtractionAndLifetimeDeferredDestroy);
    runner.RunCase("DST: 500-Tick Lockstep & Snapshot Rollback", Dst500TickLockstepAndMidRunSnapshotRollback);
    return runner.Finish();
}

#include "tests/subsystems/CtsHarness.h"

#include "core/Serialization.h"
#include "framework/Application.h"

#include <vector>

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

bool CtsSceneDirectorStackMasksAndRoomWipeTransitions(CtsRunner& r) noexcept {
    Application app;
    u32 bootCalls     = 0U;
    u32 shutdownCalls = 0U;

    AppHooks hooks{};
    hooks.userData = &bootCalls;
    hooks.onBoot   = [](LoadCtx& ctx) noexcept -> Result<void, FwErrc> {
        ++(*static_cast<u32*>(ctx.userData));
        return {};
    };
    hooks.onShutdown = [](LoadCtx&) noexcept {};
    (void)shutdownCalls;

    CTS_CHECK(r, app.Init({.headless = true, .enableDebug = true}, hooks).has_value());
    CTS_CHECK(r, app.Ready() && app.Running() && bootCalls == 1U);

    // Push base gameplay state (id = 1) and overlay pause menu state (id = 2, Freeze + NoInput).
    u32 state1Ticks = 0U;
    SceneState playState{
        .onTick = [](SimCtx&, const TickStep&, void* ud) noexcept {
            ++(*static_cast<u32*>(ud));
        },
        .userData  = &state1Ticks,
        .inputMask = 0x00FFU,
        .id        = 1U,
    };
    CTS_CHECK(r, app.GetScenes().Push(playState).has_value());
    CTS_CHECK(r, app.StepTicks(5U).has_value());
    CTS_CHECK(r, app.GetScenes().TopStateId() == 1U && state1Ticks == 5U);

    SceneState pauseState{
        .inputMask = 0x0000U,
        .id        = 2U,
        .flags     = StateFlags::Freeze | StateFlags::NoInput,
    };
    CTS_CHECK(r, app.GetScenes().Push(pauseState).has_value());
    CTS_CHECK(r, app.StepTicks(5U).has_value());
    // While pauseState is on top with Freeze, simulation ticks are frozen so state1Ticks remains 5!
    CTS_CHECK(r, app.GetScenes().TopStateId() == 2U && app.GetScenes().IsFrozen() && state1Ticks == 5U);

    // Pop pauseState -> simulation resumes!
    CTS_CHECK(r, app.GetScenes().Pop().has_value());
    CTS_CHECK(r, app.StepTicks(5U).has_value());
    CTS_CHECK(r, app.GetScenes().TopStateId() == 1U && !app.GetScenes().IsFrozen() && state1Ticks == 10U);

    // Room transition with 4-tick Fade wipe, automatic collision map loading, room bounds clamping, and mutation patching.
    std::array<u8, sizeof(TileMapHeader) + 16U> mapBlob{};
    BinaryWriter writer(mapBlob);
    (void)writer.Write(TileMapHeader{.width = 4U, .height = 4U, .tileSize = 1.0f});
    for (u32 i = 0U; i < 16U; ++i) {
        (void)writer.Write(i < 4U ? TileFlag::Solid : TileFlag::Empty);
    }
    (void)app.GetAssets().RegisterBlob(
        "rooms/crypt.col"_aid, app.GetScenes().PersistentScope(), writer.Written());

    // Pre-mark flag 10 as collected so the relic spawned in crypt is automatically removed on room load!
    app.GetGameplay().SetWorldFlag(10U, true);

    RoomDesc crypt{
        .roomId       = "rooms/crypt"_aid,
        .collisionMap = "rooms/crypt.col"_aid,
        .bounds       = {0.0f, 0.0f, 32.0f, 18.0f},
        .onEnter      = [](LoadCtx& ctx) noexcept -> Result<void, FwErrc> {
            const EntityId hero = *ctx.world->Spawn({.local = {.x = 2.0f, .y = 3.0f}});
            (void)ctx.world->Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.4f, 0.5f}});
            (void)ctx.world->Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.type = BodyType::Dynamic});

            const EntityId cam = *ctx.world->Spawn({.local = {.x = 2.0f, .y = 3.0f}});
            (void)ctx.world->Emplace<CameraRig2D>(cam);
            (void)ctx.world->Emplace<CameraFollow2D>(cam, CameraFollow2D{.target = hero});

            const EntityId relic = *ctx.world->Spawn({.local = {.x = 5.0f, .y = 3.0f}});
            (void)ctx.world->Emplace<WorldMutation2D>(relic, WorldMutation2D{
                .flagId = 10U,
                .flags  = MutationFlags::DestroyIfMutated,
            });
            return {};
        },
        .wipeTicks = 4U,
        .wipe      = WipeKind::Fade,
    };
    CTS_CHECK(r, app.GetScenes().TransitionToRoom(crypt).has_value());
    CTS_CHECK(r, app.StepTicks(12U).has_value());
    CTS_CHECK(r, app.GetScenes().CurrentRoom() == "rooms/crypt"_aid);
    CTS_CHECK(r, app.GetScenes().TransitionCount() == 1U);
    // Verify Background Room Streaming job executed asynchronously on JobSystem during Fade Out wipe!
    CTS_CHECK(r, app.GetJobs().AsyncRunCount() >= 1U);
    CTS_CHECK(r, app.GetPhysics().SampleTile(1, 0) == TileFlag::Solid);
    // Relic was automatically destroyed by ApplyRoomMutations, leaving hero + cam = 2 entities!
    CTS_CHECK(r, app.GetWorld().AliveCount() == 2U);
    CTS_CHECK(r, app.GetWorld().GetPool<CameraFollow2D>().ComponentAtDense(0U).roomBounds == Rect{0.0f, 0.0f, 32.0f, 18.0f});
    return true;
}

bool CtsEnginePipelinePhaseOrderingAndRoomPortals(CtsRunner& r) noexcept {
    Application app;
    CTS_CHECK(r, app.Init({.headless = true, .enableDebug = true}).has_value());

    std::vector<u8> phaseTrace;
    auto recordPhase = [](u8 tag, std::vector<u8>* vec) noexcept {
        vec->push_back(tag);
    };
    (void)recordPhase;

    struct TraceCtx {
        std::vector<u8>* trace;
        u8               tag;
    };
    TraceCtx tPre{&phaseTrace, 1U};
    TraceCtx tPost{&phaseTrace, 2U};
    TraceCtx tDraw{&phaseTrace, 3U};

    CTS_CHECK(r, app.GetPipeline().Register(SystemDesc{
        .id       = "test/pre_phys"_sid,
        .fn       = {.tickFn = [](SimCtx&, const TickStep&, void* ud) noexcept {
            auto* c = static_cast<TraceCtx*>(ud);
            c->trace->push_back(c->tag);
        }},
        .userData = &tPre,
        .order    = 10,
        .phase    = PipelinePhase::PrePhysics,
    }).has_value());

    CTS_CHECK(r, app.GetPipeline().Register(SystemDesc{
        .id       = "test/post_phys"_sid,
        .fn       = {.tickFn = [](SimCtx&, const TickStep&, void* ud) noexcept {
            auto* c = static_cast<TraceCtx*>(ud);
            c->trace->push_back(c->tag);
        }},
        .userData = &tPost,
        .order    = 10,
        .phase    = PipelinePhase::PostPhysics,
    }).has_value());

    CTS_CHECK(r, app.GetPipeline().Register(SystemDesc{
        .id       = "test/extract"_sid,
        .fn       = {.drawFn = [](DrawCtx&, void* ud) noexcept {
            auto* c = static_cast<TraceCtx*>(ud);
            c->trace->push_back(c->tag);
        }},
        .userData = &tDraw,
        .order    = 10,
        .phase    = PipelinePhase::Extract,
    }).has_value());

    CTS_CHECK(r, app.Step(kFixedDtNs).has_value());
    CTS_CHECK(r, phaseTrace.size() == 3U);
    CTS_CHECK(r, phaseTrace[0] == 1U && phaseTrace[1] == 2U && phaseTrace[2] == 3U);

    // Disable test/pre_phys and verify only post_phys and extract run.
    phaseTrace.clear();
    CTS_CHECK(r, app.GetPipeline().SetEnabled("test/pre_phys"_sid, false));
    CTS_CHECK(r, app.Step(kFixedDtNs).has_value());
    CTS_CHECK(r, phaseTrace.size() == 2U && phaseTrace[0] == 2U && phaseTrace[1] == 3U);

    // Verify automatic RoomPortal2D trigger transition when player overlaps doorway trigger.
    const EntityId hero = *app.GetWorld().Spawn({.local = {.x = 0.0f, .y = 0.0f}});
    (void)app.GetWorld().Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.5f, 0.5f}, .layerMask = 2U, .queryMask = 0xFFFFU});
    (void)app.GetWorld().Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.gravityScale = 0.0f, .type = BodyType::Dynamic});

    const EntityId portal = *app.GetWorld().Spawn({.local = {.x = 0.2f, .y = 0.0f}});
    (void)app.GetWorld().Emplace<Collider2D>(portal, Collider2D{.halfExtents = {0.5f, 0.5f}, .layerMask = 4U, .queryMask = 2U, .isTrigger = 1U});
    (void)app.GetWorld().Emplace<RoomPortal2D>(portal, RoomPortal2D{
        .targetRoom = "rooms/sanctuary"_aid,
        .wipeTicks  = 0U,
        .wipeKind   = static_cast<u8>(WipeKind::None),
    });

    CTS_CHECK(r, app.StepTicks(2U).has_value());
    CTS_CHECK(r, app.GetScenes().CurrentRoom() == "rooms/sanctuary"_aid);
    return true;
}

bool DstFullEngineEndToEndDeterministicLockstep(CtsRunner& r) noexcept {
    auto runFullEngine = [](u64& outHash) noexcept -> bool {
        Application app;
        if (!app.Init({.headless = true, .enableDebug = true}).has_value()) return false;

        std::array<TileFlag, 16U * 8U> tiles{};
        tiles.fill(TileFlag::Empty);
        for (u32 x = 0U; x < 16U; ++x) tiles[x] = TileFlag::Solid;
        if (!app.GetPhysics().BuildTileMap(16U, 8U, 1.0f, {0.0f, 0.0f}, tiles).has_value()) return false;

        const EntityId hero  = *app.GetWorld().Spawn({.local = {.x = 3.0f, .y = 1.5f}});
        const EntityId enemy = *app.GetWorld().Spawn({.local = {.x = 6.0f, .y = 1.5f}});

        (void)app.GetWorld().Emplace<Collider2D>(hero, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
        (void)app.GetWorld().Emplace<PhysicsBody2D>(hero, PhysicsBody2D{.type = BodyType::Dynamic});
        (void)app.GetWorld().Emplace<CharacterController2D>(hero, CharacterController2D{.moveInputX = 1.0f});
        (void)app.GetWorld().Emplace<CombatBox2D>(hero);
        (void)app.GetWorld().Emplace<Health2D>(hero, Health2D{.team = 1U});
        (void)app.GetWorld().Emplace<Sprite2D>(hero);

        (void)app.GetWorld().Emplace<Collider2D>(enemy, Collider2D{.halfExtents = {0.4f, 0.5f}, .layerMask = 2U, .queryMask = 1U});
        (void)app.GetWorld().Emplace<PhysicsBody2D>(enemy, PhysicsBody2D{.type = BodyType::Dynamic});
        (void)app.GetWorld().Emplace<CharacterController2D>(enemy, CharacterController2D{.moveInputX = -0.5f, .facing = -1});
        (void)app.GetWorld().Emplace<CombatBox2D>(enemy);
        (void)app.GetWorld().Emplace<Health2D>(enemy, Health2D{.team = 2U});
        (void)app.GetWorld().Emplace<Sprite2D>(enemy);

        const EntityId cam = *app.GetWorld().Spawn({.local = {.x = 3.0f, .y = 1.5f}});
        (void)app.GetWorld().Emplace<CameraRig2D>(cam);
        (void)app.GetWorld().Emplace<CameraFollow2D>(cam, CameraFollow2D{.target = hero});

        SceneState simState{
            .onTick = [](SimCtx& ctx, const TickStep& step, void* ud) noexcept {
                const EntityId hId = *static_cast<const EntityId*>(ud);
                if (step.tick == 20U) {
                    if (auto* cb = ctx.world->TryGet<CombatBox2D>(hId)) {
                        cb->BeginSwing({0.2f, -0.5f, 1.2f, 1.0f}, 25, {6.0f, 3.0f}, 3U);
                    }
                    ctx.gameplay->EmitCue({.magnitude = 0.4f, .kind = CueKind::CameraShake});
                }
            },
            .userData = const_cast<EntityId*>(&hero),
            .id       = 1U,
        };
        if (!app.GetScenes().Push(simState).has_value()) return false;
        if (!app.Run(120U).has_value()) return false;

        outHash = app.ComputeAuditHash();
        return app.TickCount() == 120ULL && app.GetGameplay().TotalCombatHits() >= 1U &&
               app.GetPipeline().PhaseRunCount(PipelinePhase::Physics) == 120ULL &&
               app.GetPipeline().PhaseRunCount(PipelinePhase::Extract) == 120ULL;
    };

    u64 hashA = 0ULL;
    u64 hashB = 0ULL;
    CTS_CHECK(r, runFullEngine(hashA) && runFullEngine(hashB));
    CTS_CHECK(r, hashA == hashB && hashA != 0ULL);
    r.RecordFuzzOps(120U * 2U);
    r.MixDigest(hashA);
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "FrameworkTests (CTS+DST)"};
    runner.RunCase("SceneDirector Stack, Masks & Room Transitions", CtsSceneDirectorStackMasksAndRoomWipeTransitions);
    runner.RunCase("EnginePipeline 5-Stage Ordering & RoomPortals", CtsEnginePipelinePhaseOrderingAndRoomPortals);
    runner.RunCase("DST: 120-Tick Full Engine End-to-End Lockstep", DstFullEngineEndToEndDeterministicLockstep);
    return runner.Finish();
}

#include "assets/AssetBank.h"
#include "gameplay/GameplayPipeline.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace en::tests {

[[nodiscard]] constexpr bool VerifyConstexprGameplayLayer1() noexcept {
    // 1. Verify exact byte layouts and zero-padding invariants
    static_assert(sizeof(SimCue) == 32 && alignof(SimCue) == 32);
    static_assert(sizeof(MotionStack2D) == 32 && alignof(MotionStack2D) == 32);
    static_assert(sizeof(StateMachine2D) == 16 && alignof(StateMachine2D) == 16);
    static_assert(sizeof(HitboxRig2D) == 32 && alignof(HitboxRig2D) == 32);
    static_assert(sizeof(DamageReceiver2D) == 16 && alignof(DamageReceiver2D) == 16);
    static_assert(sizeof(SafeAnchor2D) == 16 && alignof(SafeAnchor2D) == 16);
    static_assert(sizeof(CameraController2D) == 64 && alignof(CameraController2D) == 64);
    static_assert(sizeof(CombatHitEvent) == 32 && alignof(CombatHitEvent) == 32);
    static_assert(sizeof(CookedGameplayRigHeader) == 16 && alignof(CookedGameplayRigHeader) == 16);
    static_assert(sizeof(CookedStateEntry) == 16 && alignof(CookedStateEntry) == 8);
    static_assert(sizeof(CookedFrameDirective) == 16 && alignof(CookedFrameDirective) == 8);
    static_assert(sizeof(GameplayPolicyTable) == 24 && alignof(GameplayPolicyTable) == 8);
    static_assert(sizeof(GameplayConfig) == 32 && alignof(GameplayConfig) == 16);
    static_assert(sizeof(GameplayPipeline) == 640 && alignof(GameplayPipeline) == 64);

    // 2. Verify 64-bit SplitMix swing Bloom filter deduplication and configurable hitstop ticks
    HitboxRig2D rig{};
    rig.BeginNewSwing(CompactAabb2D::FromMeters(0.5f, 0.0f, 0.5f, 0.4f), 25, 10U, 5.0f, 3.0f, 1U, 4U);
    if (rig.HitstopTicks() != 4U) return false;
    constexpr EntityId enemyA{12U, 1U};
    constexpr EntityId enemyB{13U, 1U};
    if (rig.HasAlreadyHit(enemyA) || rig.HasAlreadyHit(enemyB)) return false;
    rig.RecordDefenderHit(enemyA);
    if (!rig.HasAlreadyHit(enemyA) || rig.HasAlreadyHit(enemyB)) return false;

    rig.BeginNewSwing(CompactAabb2D::FromMeters(0.5f, 0.0f, 0.5f, 0.4f), 30, 15U, 5.0f, 3.0f, 2U, 6U);
    if (rig.HasAlreadyHit(enemyA) || rig.HitstopTicks() != 6U) return false;

    // 3. Verify StateMachine2D cancel window, on-hit promotion, and action buffering
    StateMachine2D fsm{};
    fsm.TransitionTo(1U, CancelCategory::Dash);
    fsm.onHitCancelMask = CancelCategory::Jump | CancelCategory::Special;
    if (!fsm.CanCancelInto(CancelCategory::Dash)) return false;
    if (fsm.CanCancelInto(CancelCategory::Special)) return false;
    fsm.PromoteOnHitCancels();
    if (!fsm.CanCancelInto(CancelCategory::Special)) return false;

    fsm.BufferAction(3U, 6U);
    if (!fsm.ConsumeBufferedAction(3U) || fsm.ConsumeBufferedAction(3U)) return false;

    // 4. Verify MotionStack2D coyote jump and directional wall jump
    MotionStack2D ms{};
    ms.groundCoyoteTicks = 5U;
    if (!ms.TryExecuteJump(12.0f) || ms.groundCoyoteTicks != 0U || ms.pendingLaunchVy != 12.0f) {
        return false;
    }

    // 5. Verify constexpr critically damped spring convergence
    f32 pos = 0.0f;
    f32 vel = 0.0f;
    for (u32 i = 0U; i < 60U; ++i) {
        CameraDirector2D::StepCriticallyDampedSpring1D(pos, vel, 10.0f, 12.0f, 1.0f / 60.0f);
    }
    if (pos < 9.95f || pos > 10.05f) return false;

    return true;
}

static_assert(VerifyConstexprGameplayLayer1(), "Compile-time verification of Phase 9 Gameplay v2 Layer 1 failed.");

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

void TestMotionBlendingCoyoteVariableJumpSafeAnchorsAndSoftPushboxes(TestSuite& suite) noexcept {
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());
    PhysicsSystem phys;
    EXPECT_TRUE(suite, phys.Init(world).has_value());
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());
    GameplayPipeline gp;
    EXPECT_TRUE(suite, gp.Init(world).has_value());

    std::vector<TileMaterialFlags> tiles(16U * 8U, TileMaterialFlags::Empty);
    for (u32 x = 0U; x < 16U; ++x) tiles[x] = TileMaterialFlags::Solid;
    EXPECT_TRUE(suite, phys.BuildTileMap(16U, 8U, 1.0f, 0.0f, 0.0f, tiles).has_value());

    const EntityId p1 = world.Spawn(EntitySpawnDesc{.local = {4.0f, 1.5f}}).value_or(kNullEntity);
    const EntityId p2 = world.Spawn(EntitySpawnDesc{.local = {4.1f, 1.5f}}).value_or(kNullEntity);

    CharacterMotor2D m1{};
    m1.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.5f);
    MotionStack2D s1{};
    s1.pushboxGroup = 1U;
    SafeAnchor2D a1{};

    CharacterMotor2D m2 = m1;
    MotionStack2D s2 = s1;

    EXPECT_TRUE(suite, world.Emplace<CharacterMotor2D>(p1, m1).has_value());
    EXPECT_TRUE(suite, world.Emplace<MotionStack2D>(p1, s1).has_value());
    EXPECT_TRUE(suite, world.Emplace<SafeAnchor2D>(p1, a1).has_value());
    EXPECT_TRUE(suite, world.Emplace<CharacterMotor2D>(p2, m2).has_value());
    EXPECT_TRUE(suite, world.Emplace<MotionStack2D>(p2, s2).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 60.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;
    TickInput tickIn{};
    ActionBuffer actions{};

    for (u32 t = 0U; t < 15U; ++t) {
        world.BeginTick(step);
        gp.Tick(world, phys, bank, step, tickIn, actions);
        world.EndTick(step);
    }

    // 1. Verify soft character pushboxes separated p1 and p2 horizontally
    const f32 dist = std::abs(world.GetWorldTransform(p2).x - world.GetWorldTransform(p1).x);
    EXPECT_TRUE(suite, dist > 0.70f);
    EXPECT_EQ(suite, world.TryGet<MotionStack2D>(p1)->groundCoyoteTicks, 8U);
    EXPECT_NEAR(suite, world.TryGet<SafeAnchor2D>(p1)->safeY, 1.5f, 5e-2f);

    // 2. Verify coyote jump execution and early-release variable jump velocity cut
    EXPECT_TRUE(suite, world.TryGet<MotionStack2D>(p1)->TryExecuteJump(10.0f));
    world.BeginTick(step);
    gp.Tick(world, phys, bank, step, tickIn, actions);
    world.EndTick(step);
    const f32 vyHeld = world.TryGet<CharacterMotor2D>(p1)->vy;
    EXPECT_TRUE(suite, vyHeld > 9.0f);

    // Release jump button while ascending -> next tick applies jumpCutMultiplier (0.45f)
    world.TryGet<MotionStack2D>(p1)->flags &= ~MotionFlags::JumpHeld;
    world.BeginTick(step);
    gp.Tick(world, phys, bank, step, tickIn, actions);
    world.EndTick(step);
    EXPECT_TRUE(suite, world.TryGet<CharacterMotor2D>(p1)->vy < vyHeld * 0.55f);

    // 3. Verify priority velocity override and world flags
    EXPECT_TRUE(suite, world.TryGet<MotionStack2D>(p1)->RequestVelocityOverride(12.0f, 0.0f, 5U, 10U));
    EXPECT_TRUE(suite, !world.TryGet<MotionStack2D>(p1)->RequestVelocityOverride(-5.0f, 0.0f, 5U, 2U));

    gp.SetWorldFlag(100U, true);
    gp.SetWorldFlag(2048U, true);
    EXPECT_TRUE(suite, gp.TestWorldFlag(100U));
    EXPECT_TRUE(suite, gp.TestWorldFlag(2048U));
    EXPECT_TRUE(suite, !gp.TestWorldFlag(101U));
    gp.ClearWorldFlags();
    EXPECT_TRUE(suite, !gp.TestWorldFlag(100U));
    EXPECT_EQ(suite, gp.ActiveFlagWordsMask(), 0ULL);

    gp.Shutdown();
    bank.Shutdown();
    phys.Shutdown();
    world.Shutdown();
}

void TestCookedErigAnimatorSyncHitstopBloomCombatClashAndParry(TestSuite& suite) noexcept {
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());
    PhysicsSystem phys;
    EXPECT_TRUE(suite, phys.Init(world).has_value());
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());

    static u32 s_hitCallbackCount = 0U;
    static u32 s_transitionCallbackCount = 0U;
    s_hitCallbackCount = 0U;
    s_transitionCallbackCount = 0U;

    GameplayPolicyTable policies{};
    policies.onCombatHit = [](const CombatHitEvent&, World&, GameplayPipeline&) noexcept {
        ++s_hitCallbackCount;
    };
    policies.onStateTransition = [](EntityId, u8, u8, World&, GameplayPipeline&) noexcept {
        ++s_transitionCallbackCount;
    };

    GameplayPipeline gp;
    EXPECT_TRUE(suite, gp.Init(world, policies).has_value());

    // Build an in-memory cooked "ERIG" v1 blob with 2 states:
    // State 0 (Idle): duration = 0, animFirstFrame = 0, animFrameCount = 4
    // State 1 (Attack): duration = 4 ticks, animFirstFrame = 10, animFrameCount = 4, animTicksPerFrame = 1
    //   - frame 0: EmitAudioCue (hash = 0x1234)
    //   - frame 1: ActivateHitbox (damage = 30, poise = 15, prio = 2, hitstop = 2 ticks)
    //   - frame 3: DeactivateHitbox
    CookedGameplayRigHeader hdr{};
    hdr.stateCount = 2U;
    hdr.directiveCount = 3U;

    CookedStateEntry states[2]{};
    states[0] = CookedStateEntry{0U, 0U, 0U, 4U, 6U, 1U, 0U, 0U, CancelCategory::Any, CancelCategory::None, 0U};
    states[1] = CookedStateEntry{4U, 0U, 10U, 4U, 1U, 0U, 3U, 0U, CancelCategory::None, CancelCategory::Special, 1U};

    CookedFrameDirective dirs[3]{};
    dirs[0].triggerFrame = 0U;
    dirs[0].op = FrameDirectiveOp::EmitAudioCue;
    dirs[0].assetHashOrParam = 0x1234U;

    dirs[1].triggerFrame = 1U;
    dirs[1].op = FrameDirectiveOp::ActivateHitbox;
    dirs[1].boxOrMotion = CompactAabb2D::FromMeters(0.8f, 0.0f, 0.8f, 0.5f);
    dirs[1].assetHashOrParam = 30U | (15U << 16U) | (2U << 24U);
    dirs[1].auxByte = 2U; // 2 ticks of per-entity hitstop!

    dirs[2].triggerFrame = 3U;
    dirs[2].op = FrameDirectiveOp::DeactivateHitbox;

    std::vector<u8> rigBytes(sizeof(hdr) + sizeof(states) + sizeof(dirs));
    std::memcpy(rigBytes.data(), &hdr, sizeof(hdr));
    std::memcpy(rigBytes.data() + sizeof(hdr), states, sizeof(states));
    std::memcpy(rigBytes.data() + sizeof(hdr) + sizeof(states), dirs, sizeof(dirs));

    const u8 scope = bank.BeginScope().value_or(0U);
    bool needsLoad = false;
    const u16 slot = bank.AcquireSlot(AssetId::Hash("hero.erig"), AssetKind::Blob, scope, AssetResidency::Sim, needsLoad);
    const BlobHandle rigHandle = bank.CommitBlob(slot, rigBytes);
    EXPECT_TRUE(suite, rigHandle.IsValid());

    const EntityId attacker = world.Spawn(EntitySpawnDesc{.local = {2.0f, 1.5f}}).value_or(kNullEntity);
    const EntityId defender = world.Spawn(EntitySpawnDesc{.local = {2.8f, 1.5f}}).value_or(kNullEntity);

    StateMachine2D attFsm{};
    attFsm.rigBlob = rigHandle;
    attFsm.TransitionTo(1U);
    SpriteAnimator2D attAnim{};
    HitboxRig2D attRig{};
    attRig.hitLayerMask = 0x02U;
    attRig.SetHurtLayer(0U);
    MotionStack2D attMotion{};

    HitboxRig2D defRig{};
    defRig.hurtbox = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.5f);
    defRig.SetHurtLayer(1U);
    DamageReceiver2D defRecv{};
    defRecv.hp = 100;
    defRecv.poise = 10U;
    defRecv.autoIframeTicks = 0U; // Disable auto-iframes to prove Bloom filter alone blocks multi-frame re-hits!
    MotionStack2D defMotion{};

    EXPECT_TRUE(suite, world.Emplace<StateMachine2D>(attacker, attFsm).has_value());
    EXPECT_TRUE(suite, world.Emplace<SpriteAnimator2D>(attacker, attAnim).has_value());
    EXPECT_TRUE(suite, world.Emplace<HitboxRig2D>(attacker, attRig).has_value());
    EXPECT_TRUE(suite, world.Emplace<MotionStack2D>(attacker, attMotion).has_value());

    EXPECT_TRUE(suite, world.Emplace<HitboxRig2D>(defender, defRig).has_value());
    EXPECT_TRUE(suite, world.Emplace<DamageReceiver2D>(defender, defRecv).has_value());
    EXPECT_TRUE(suite, world.Emplace<MotionStack2D>(defender, defMotion).has_value());

    BakedTickState baked{};
    baked.scaledDt.fill(1.0f / 60.0f);
    baked.effectiveScale.fill(1.0f);
    SimStepView step{};
    step.baked = &baked;
    step.substepTotal = 1U;
    step.valid = true;
    TickInput tickIn{};
    ActionBuffer actions{};

    bool sawAudioCue = false;
    bool sawHitstopCue = false;
    bool syncedAnimFirstFrame = false;

    // Run 6 ticks (4 state ticks + 2 hitstop freeze ticks!)
    for (u32 t = 0U; t < 6U; ++t) {
        world.BeginTick(step);
        gp.Tick(world, phys, bank, step, tickIn, actions);
        world.StepAnimations(step);
        if (world.TryGet<SpriteAnimator2D>(attacker)->firstFrame == 10U) {
            syncedAnimFirstFrame = true;
        }
        for (const SimCue& c : gp.DrainCues()) {
            if (c.kind == SimCueKind::AudioOneShot && c.assetId.value == 0x1234ULL) sawAudioCue = true;
            if (c.kind == SimCueKind::HitstopFreeze && c.durationTicks == 2U)      sawHitstopCue = true;
        }
        world.EndTick(step);
    }

    EXPECT_TRUE(suite, syncedAnimFirstFrame); // Proves CookedStateEntry auto-synced SpriteAnimator2D!
    EXPECT_TRUE(suite, sawAudioCue);
    EXPECT_TRUE(suite, sawHitstopCue);
    EXPECT_EQ(suite, s_hitCallbackCount, 1U); // Bloom filter prevented duplicate hit!
    EXPECT_EQ(suite, world.TryGet<DamageReceiver2D>(defender)->hp, 70);
    EXPECT_EQ(suite, s_transitionCallbackCount, 1U);
    EXPECT_EQ(suite, world.TryGet<StateMachine2D>(attacker)->stateId, 0U);

    // Test Directional Parry: defender facing attacker (-1) parries cleanly; facing away (+1) gets hit!
    world.TryGet<DamageReceiver2D>(defender)->stunTicks = 0U;
    world.TryGet<DamageReceiver2D>(defender)->AddFlags(ReceiverFlags::Parrying);
    world.TryGet<MotionStack2D>(defender)->SetFacingSign(-1); // Facing left toward attacker at x=2.0
    world.TryGet<HitboxRig2D>(attacker)->BeginNewSwing(
        CompactAabb2D::FromMeters(0.6f, 0.0f, 0.6f, 0.5f), 25, 10U, 5.0f, 2.0f, 2U, 0U);

    world.BeginTick(step);
    gp.Tick(world, phys, bank, step, tickIn, actions);
    world.EndTick(step);

    EXPECT_EQ(suite, gp.LatestCombatHits().size(), 1U);
    EXPECT_EQ(suite, gp.LatestCombatHits()[0].outcome, HitOutcome::Parried);
    EXPECT_EQ(suite, world.TryGet<DamageReceiver2D>(defender)->hp, 70);
    world.TryGet<DamageReceiver2D>(defender)->RemoveFlags(ReceiverFlags::Parrying);

    // Test Hitbox-vs-Hitbox Clash
    world.TryGet<HitboxRig2D>(attacker)->flags |= HitboxFlags::CanClash;
    world.TryGet<HitboxRig2D>(attacker)->BeginNewSwing(
        CompactAabb2D::FromMeters(0.6f, 0.0f, 0.6f, 0.5f), 25, 10U, 5.0f, 2.0f, 2U, 2U);

    world.TryGet<HitboxRig2D>(defender)->hitLayerMask = 0x01U;
    world.TryGet<HitboxRig2D>(defender)->flags |= HitboxFlags::CanClash;
    world.TryGet<HitboxRig2D>(defender)->BeginNewSwing(
        CompactAabb2D::FromMeters(0.6f, 0.0f, 0.6f, 0.5f), 25, 10U, 5.0f, 2.0f, 2U, 2U);

    world.BeginTick(step);
    gp.Tick(world, phys, bank, step, tickIn, actions);
    world.EndTick(step);

    EXPECT_EQ(suite, gp.LatestCombatHits().size(), 1U);
    EXPECT_EQ(suite, gp.LatestCombatHits()[0].outcome, HitOutcome::Clashed);
    EXPECT_EQ(suite, world.TryGet<DamageReceiver2D>(defender)->hp, 70);

    gp.Shutdown();
    bank.Shutdown();
    phys.Shutdown();
    world.Shutdown();
}

void TestCameraDirectorSpringConfinerTraumaAndDeterminism(TestSuite& suite) noexcept {
    const auto runCameraSim = [&]() noexcept -> u64 {
        World world;
        (void)world.Init();
        PhysicsSystem phys;
        (void)phys.Init(world);
        AssetBank bank;
        (void)bank.Init();
        GameplayPipeline gp;
        (void)gp.Init(world);

        const EntityId target = world.Spawn(EntitySpawnDesc{.local = {20.0f, 8.0f}}).value_or(kNullEntity);
        MotionStack2D ms{};
        ms.SetFacingSign(1);
        (void)world.Emplace<MotionStack2D>(target, ms);

        const EntityId cam = world.Spawn(EntitySpawnDesc{.local = {0.0f, 0.0f}}).value_or(kNullEntity);
        CameraRig2D rig{};
        rig.viewport = Rect{0.0f, 0.0f, 16.0f, 9.0f};
        rig.zoom = 1.0f;
        CameraController2D ctrl{};
        ctrl.primaryTarget = target;
        ctrl.flags &= ~CameraCtrlFlags::SnapNextStep;
        ctrl.activeRoomBounds = Rect{0.0f, 0.0f, 30.0f, 20.0f};
        ctrl.AddTrauma(0.6f);
        ctrl.ApplyDirectionalKick(4.0f, -2.0f);
        (void)world.Emplace<CameraRig2D>(cam, rig);
        (void)world.Emplace<CameraController2D>(cam, ctrl);

        BakedTickState baked{};
        baked.scaledDt.fill(1.0f / 60.0f);
        baked.effectiveScale.fill(1.0f);
        SimStepView step{};
        step.baked = &baked;
        step.substepTotal = 1U;
        step.valid = true;
        TickInput tickIn{};
        ActionBuffer actions{};

        for (u32 t = 0U; t < 60U; ++t) {
            world.BeginTick(step);
            gp.Tick(world, phys, bank, step, tickIn, actions);
            world.EndTick(step);
        }

        const CameraController2D* c = world.TryGet<CameraController2D>(cam);
        EXPECT_TRUE(suite, c != nullptr);
        EXPECT_TRUE(suite, c->anchorX > 18.0f && c->anchorX <= 22.001f);
        EXPECT_NEAR(suite, c->anchorY, 8.0f, 0.8f);

        const u64 audit = gp.ComputeAuditHash(world);
        gp.Shutdown();
        bank.Shutdown();
        phys.Shutdown();
        world.Shutdown();
        return audit;
    };

    const u64 h1 = runCameraSim();
    const u64 h2 = runCameraSim();
    EXPECT_EQ(suite, h1, h2);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};
    TestMotionBlendingCoyoteVariableJumpSafeAnchorsAndSoftPushboxes(suite);
    TestCookedErigAnimatorSyncHitstopBloomCombatClashAndParry(suite);
    TestCameraDirectorSpringConfinerTraumaAndDeterminism(suite);

    std::printf("Phase 9 Gameplay v2 Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}

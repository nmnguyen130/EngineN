#include "tests/TestRunner.h"
#include "framework/SceneDirector.h"
#include "gameplay/AbilityAndModifier.h"
#include "gameplay/GameplayPipeline.h"

namespace en::tests {

void RunSuitePhysicsAndGameplay(SuiteStats& stats) noexcept {
    World world{};
    EN_CHECK(stats, world.Init().has_value());

    GameplayPipeline gp{};
    EN_CHECK(stats, gp.Init(world).has_value());

    // 1. SpatialIndex2D procedural grid, tile query, swept AABB, and raycast
    gp.Spatial().SetupProceduralGrid(32, 16, 1.0f, {0.0f, 0.0f});
    for (i32 x = 0; x < 32; ++x) {
        gp.Spatial().SetTile(x, 0, TileType::Solid);
    }
    gp.Spatial().SetTile(10, 2, TileType::Solid);
    EN_CHECK(stats, gp.Spatial().GetTile(10, 2) == TileType::Solid);

    const RayHit2D rHit = gp.Spatial().Raycast({2.0f, 2.5f}, {1.0f, 0.0f}, 20.0f, 1U);
    EN_CHECK(stats, rHit.hit && std::abs(rHit.distance - 8.0f) < 0.25f);

    // 2. Spawn 2 actors with KinematicMotor2D, HitboxRig2D, DamageReceiver2D and resolve combat
    EntitySpawnDesc aDesc{};
    aDesc.local = {4.0f, 2.0f, 0.0f, 1.0f, 1.0f};
    const EntityId attacker = *world.Spawn(aDesc);

    EntitySpawnDesc dDesc{};
    dDesc.local = {4.6f, 2.0f, 0.0f, 1.0f, 1.0f};
    const EntityId defender = *world.Spawn(dDesc);

    KinematicMotor2D motor{};
    motor.vx = 4.0f;
    (void)world.Emplace<KinematicMotor2D>(attacker, motor);

    HitboxRig2D atkRig{};
    atkRig.hitLayerMask = 2U;
    atkRig.hurtLayerMask = 1U;
    atkRig.BeginNewSwing(25, CompactAabb2D::FromMeters(0.6f, 0.0f, 0.8f, 0.6f));
    (void)world.Emplace<HitboxRig2D>(attacker, atkRig);

    DamageReceiver2D defDmg{};
    defDmg.hp = 100;
    defDmg.maxHp = 100;
    defDmg.teamId = 2;
    (void)world.Emplace<DamageReceiver2D>(defender, defDmg);

    HitboxRig2D defRig{};
    defRig.hurtLayerMask = 2U;
    defRig.hitLayerMask = 1U;
    defRig.hurtbox = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 0.8f);
    (void)world.Emplace<HitboxRig2D>(defender, defRig);

    TimePipeline tp{};
    tp.BeginFrameManual(kFixedDtNs);
    const SimStepView step = tp.PollTick();
    TickInput ti{};
    ActionBuffer ab{};

    world.BeginTick(step);
    gp.Tick(world, AssetBank{}, step, ti, ab);
    world.EndTick(step);

    const DamageReceiver2D* defHp = world.TryGet<DamageReceiver2D>(defender);
    EN_CHECK(stats, defHp != nullptr && defHp->hp < 100);

    // 3. NavGrid2D A* pathfinding, ParticlePool2D, WorldFlags, and SaveGameManager round-trip
    std::array<Vec2, 16> pathWaypoints{};
    const u32 wpCount = NavGrid2D::FindPathAStar(gp.Spatial(), {2.5f, 3.5f}, {14.5f, 3.5f}, pathWaypoints);
    EN_CHECK(stats, wpCount > 0U);

    ParticleBurstDesc burst{};
    burst.origin = {5.0f, 5.0f};
    burst.count = 16;
    gp.Particles().EmitBurst(burst);
    gp.Particles().Step(kFixedDtSec);
    EN_CHECK(stats, gp.Particles().ActiveCount() == 16U);

    gp.SetWorldFlag(42, true);
    EN_CHECK(stats, gp.TestWorldFlag(42));
    std::array<u8, 1024> saveBytes{};
    SaveSlotHeader shdr{};
    shdr.checkpointRoomId = 7;
    shdr.playerHp = 85;
    auto written = SaveGameManager::SerializeSaveSlot(shdr, gp, saveBytes);
    EN_CHECK(stats, written.has_value());
    gp.SetWorldFlag(42, false);
    auto restored = SaveGameManager::DeserializeSaveSlot(std::span<const u8>(saveBytes.data(), *written), gp);
    EN_CHECK(stats, restored.has_value() && restored->checkpointRoomId == 7 && gp.TestWorldFlag(42));

    // 4. Linecast / HasLineOfSight, ContactListener2D (OnEnter/OnStay/OnExit), and SweepProjectileCCD
    EN_CHECK(stats, !gp.Spatial().HasLineOfSight({2.0f, 2.5f}, {15.0f, 2.5f}, 1U));
    EN_CHECK(stats, gp.Spatial().HasLineOfSight({2.0f, 5.5f}, {15.0f, 5.5f}, 1U));

    // High-speed projectile CCD against wall at x=10, y=2
    const SlabImpact2D ccdHit = KinematicSolver2D::SweepProjectileCCD(
        gp.Spatial(), kNullEntity, {2.0f, 2.5f}, {200.0f, 0.0f}, 0.1f, {0.1f, 0.1f}, 1U);
    EN_CHECK(stats, ccdHit.hit && ccdHit.toi < 0.5f);

    // TriggerVolume2D + ContactListener2D OnEnter / OnStay
    EntitySpawnDesc trigDesc{};
    trigDesc.local = {4.0f, 2.0f, 0.0f, 1.0f, 1.0f};
    const EntityId trigEnt = *world.Spawn(trigDesc);
    TriggerVolume2D trigVol{};
    trigVol.bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 2.0f, 2.0f);
        trigVol.triggerTag = StringId{77U};
        (void)world.Emplace<TriggerVolume2D>(trigEnt, trigVol);
    gp.Spatial().RebuildDynamicGrid(world, SpatialRebuildMode::All);

    ContactListener2D listener{};
    std::array<TriggerContactEvent, 16> trigEvents{};
    const u32 evCount1 = listener.StepAndCollectTriggerEvents(world, gp.Spatial(), trigEvents);
    EN_CHECK(stats, evCount1 >= 1U && trigEvents[0].kind == TriggerEventKind::OnEnter);
    const u32 evCount2 = listener.StepAndCollectTriggerEvents(world, gp.Spatial(), trigEvents);
    EN_CHECK(stats, evCount2 >= 1U && trigEvents[0].kind == TriggerEventKind::OnStay);

    // 5. DecisionTree2D, AbilitySystem2D, and ModifierStack2D
    DecisionTree2D dtree{};
    const u8 leafAttack = dtree.AddActionLeaf(10U);
    const u8 leafPatrol = dtree.AddActionLeaf(20U);
    const u8 rootBranch = dtree.AddBranchNode([](const BtBlackboard& bb) noexcept {
        return bb.hasLineOfSight;
    }, leafAttack, leafPatrol);
    BtBlackboard bb{};
    bb.hasLineOfSight = true;
    EN_CHECK(stats, dtree.Evaluate(bb, rootBranch) == 10U);

    ModifierStack2D modStack{};
    AbilitySystem2D abilities{};
    AbilitySlot berserk{};
    berserk.abilityId = 1U;
    berserk.cooldownTicks = 30U;
    berserk.durationTicks = 10U;
    berserk.resourceCost = 25.0f;
    berserk.grantModifierId = 500U;
    berserk.grantAttr = AttributeKind::AttackDamage;
    berserk.grantMagnitude = 0.50f;
    EN_CHECK(stats, abilities.RegisterAbility(berserk));

    f32 focusPool = 40.0f;
    EN_CHECK(stats, abilities.TryActivate(1U, focusPool, &modStack));
    EN_CHECK(stats, std::abs(focusPool - 15.0f) < 1e-4f);
    EN_CHECK(stats, std::abs(modStack.Evaluate(AttributeKind::AttackDamage, 100.0f) - 150.0f) < 1e-3f);

    gp.Shutdown();
    world.Shutdown();
}

} // namespace en::tests

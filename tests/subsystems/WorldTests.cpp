#include "time/TimeSystem.h"
#include "world/WorldSystems.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace en;
using namespace en::literals;

bool TestEcsHierarchyAndDeferredCommands() noexcept {
    World world;
    if (!world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U})) return false;

    const auto parentRes = world.Spawn({.local = {.x = 2.0f, .y = 3.0f}});
    if (!parentRes) return false;
    const EntityId parent = *parentRes;

    const auto childRes = world.Spawn({
        .local  = {.x = 1.0f, .y = -1.0f},
        .parent = parent,
    });
    if (!childRes) return false;
    const EntityId child = *childRes;

    const auto cyc = world.SetParent(parent, child);
    if (cyc || cyc.error() != WorldErrc::HierarchyCycle) {
        return false;
    }

    if (!world.Emplace<Velocity2D>(parent, 120.0f, 0.0f)) return false;
    if (!world.Emplace<Sprite2D>(child)) return false;
    if (!world.Emplace<Lifetime2D>(child, Lifetime2D{.remainingTicks = 2U, .initialTicks = 2U, .fadeSpriteAlpha = true})) {
        return false;
    }

    const WorldTransform2D childWt0 = world.GetWorldTransform(child);
    if (std::abs(childWt0.x - 3.0f) > 1e-4f || std::abs(childWt0.y - 2.0f) > 1e-4f) {
        return false;
    }

    TimeSystem time;
    time.BeginFrame(kFixedDtNs);
    const TickStep step = time.PollTick();
    world.BeginTick(step);
    WorldSystems::StepVelocities(world, step);
    WorldSystems::StepVisualsAndLifetimes(world, step);
    world.EndTick(step);

    const WorldTransform2D childWt1 = world.GetWorldTransform(child);
    if (std::abs(childWt1.x - 4.0f) > 1e-4f) return false;
    if (std::abs(world.TryGet<Sprite2D>(child)->tint.a - 0.5f) > 1e-4f) return false;

    AssetBank bank;
    if (!bank.Init()) return false;
    RenderList renderList;
    renderList.Reset({1280, 720});
    RenderStep rv = time.RenderView();
    rv.alpha = 0.5f;
    WorldSystems::ExtractRender(world, bank, rv, renderList);
    if (renderList.Stats().submitted != 1U) return false;

    world.Teleport(parent, {.x = -2.0f, .y = 0.0f});
    if (std::abs(world.GetWorldTransform(child).x - (-1.0f)) > 1e-4f ||
        std::abs(world.GetPrevWorldByIndex(child.index).x - (-1.0f)) > 1e-4f) {
        return false;
    }

    if (!world.DeferEmplace<Tag2D>(child, Tag2D{.name = "hero_child"_sid})) return false;
    if (!world.FlushCommands() || !world.Has<Tag2D>(child)) return false;

    if (!world.DeferDestroy(parent, true) || !world.FlushCommands()) return false;
    return world.AliveCount() == 0U && !world.IsAlive(parent) && !world.IsAlive(child);
}

bool TestClonePrefabAndSnapshotSerialization() noexcept {
    World world;
    if (!world.Init({.maxEntities = 128U, .defaultPoolCapacity = 128U})) return false;

    const EntityId root = *world.Spawn({.local = {.x = 4.0f, .y = 8.0f}});
    const EntityId leaf = *world.Spawn({.local = {.x = 1.0f, .y = 2.0f}, .parent = root});
    (void)world.Emplace<Velocity2D>(root, 3.0f, 6.0f);
    (void)world.Emplace<Tag2D>(leaf, Tag2D{.name = "blade"_sid, .categoryMask = 7U});
    world.PropagateTransforms();

    const auto clonedRoot = world.CloneEntity(root, {.x = -4.0f, .y = -8.0f}, true);
    if (!clonedRoot || world.AliveCount() != 4U || !world.Has<Velocity2D>(*clonedRoot)) {
        return false;
    }

    std::array<u8, 1024> prefabBuf{};
    const auto prefabBytes = world.SerializePrefabTemplate(root, prefabBuf);
    if (!prefabBytes) return false;

    AssetBank bank;
    if (!bank.Init()) return false;
    const u8 scope = *bank.BeginScope();
    const BlobHandle blob = bank.RegisterBlob(
        "prefabs/hero.bin"_aid, scope, {prefabBuf.data(), *prefabBytes});
    const auto instRes = world.InstantiatePrefab(bank, blob, {.x = 10.0f, .y = 20.0f});
    if (!instRes || world.AliveCount() != 6U) return false;

    const u64 hashBefore = world.ComputeAuditHash();
    std::vector<u8> snapBuf(world.RequiredSnapshotBytes());
    const auto snapBytes = world.SerializeWorldSnapshot(snapBuf);
    if (!snapBytes) return false;

    world.Clear();
    if (world.AliveCount() != 0U) return false;
    if (!world.DeserializeWorldSnapshot({snapBuf.data(), *snapBytes})) return false;
    return world.AliveCount() == 6U && world.ComputeAuditHash() == hashBefore;
}

} // namespace

int main() {
    if (!TestEcsHierarchyAndDeferredCommands()) return 1;
    if (!TestClonePrefabAndSnapshotSerialization()) return 2;
    std::printf("WorldTests: ALL PASSED\n");
    return 0;
}

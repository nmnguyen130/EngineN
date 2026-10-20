#include "tests/TestRunner.h"
#include "framework/Application.h"

namespace en::tests {

void RunSuiteDebugAndFramework(SuiteStats& stats) noexcept {
    using namespace en::literals;

    // 1. DebugSystem: DebugBatcher gizmos, CVar registration & console command execution
    DebugSystem dbg{};
    EN_CHECK(stats, dbg.Init(256 * 1024));
    f32 testSpeed = 5.0f;
    EN_CHECK(stats, dbg.Console().RegisterCVarF32("g.speed", &testSpeed, 1.0f, 20.0f));
    dbg.Console().ExecuteImmediate("g.speed 12.5");
    EN_CHECK(stats, std::abs(testSpeed - 12.5f) < 1e-4f);

    dbg.Shapes().AddCircle({0.0f, 0.0f}, 2.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}, 4);
    dbg.Shutdown();

    // 2. Full Application headless frame stepping & SceneDirector state layer stack
    Application app{};
    ApplicationConfig cfg{};
    cfg.headless = true;
    AppHooks hooks{};
    EN_CHECK(stats, app.Init(cfg, hooks).has_value());

    EntitySpawnDesc eDesc{};
    eDesc.local = {3.0f, 4.0f, 0.0f, 1.0f, 1.0f};
    const EntityId ent = *app.WorldRef().Spawn(eDesc);
    EN_CHECK(stats, app.WorldRef().IsAlive(ent));

    auto stepRes = app.StepHeadless(4);
    EN_CHECK(stats, stepRes.has_value() && stepRes->tickIndex >= 4ULL);

    app.Shutdown();
}

} // namespace en::tests

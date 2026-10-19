#include "framework/Application.h"
#include "framework/SceneDirector.h"
#include "world/WorldExtractor.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace en;
using namespace en::framework;

struct TestAppState {
    u32 enterCount{0};
    u32 exitCount{0};
    u32 baseSimTicks{0};
    u32 overlaySimTicks{0};
    u32 baseRenders{0};
    u32 overlayRenders{0};
    u32 cuesConsumed{0};
    bool sceneActivated{false};
    bool sceneDeactivated{false};
};

void OnBaseSimTick(SimContext& ctx) noexcept {
    auto* st = static_cast<TestAppState*>(ctx.userState);
    ++st->baseSimTicks;
}

void OnBaseRender(RenderContext& ctx) noexcept {
    auto* st = static_cast<TestAppState*>(ctx.userState);
    ++st->baseRenders;
    ctx.renderList.PushRect({0.0f, 0.0f, 10.0f, 10.0f}, renderer::Color{1.0f, 1.0f, 1.0f, 1.0f});
}

void OnOverlaySimTick(SimContext& ctx) noexcept {
    auto* st = static_cast<TestAppState*>(ctx.userState);
    ++st->overlaySimTicks;
}

void OnOverlayRender(RenderContext& ctx) noexcept {
    auto* st = static_cast<TestAppState*>(ctx.userState);
    ++st->overlayRenders;
    ctx.renderList.PushRect({0.0f, 0.0f, 10.0f, 10.0f}, renderer::Color{1.0f, 1.0f, 1.0f, 1.0f});
}

void OnTrackEnter(SceneLoadContext& ctx, void* userState) noexcept {
    (void)ctx;
    auto* st = static_cast<TestAppState*>(userState);
    ++st->enterCount;
}

void OnTrackExit(SceneLoadContext& ctx, void* userState) noexcept {
    (void)ctx;
    auto* st = static_cast<TestAppState*>(userState);
    ++st->exitCount;
}

void OnReentrantEnterPushOverlay(SceneLoadContext& ctx, void* userState) noexcept {
    auto* st = static_cast<TestAppState*>(userState);
    ++st->enterCount;
    assert(ctx.director != nullptr);
    auto r = ctx.director->PushState(StateLayerDesc{
        .onSimTick = &OnOverlaySimTick,
        .onRenderExtract = &OnOverlayRender,
        .onEnter = &OnTrackEnter,
        .onExit = &OnTrackExit,
        .stateData = userState,
        .id = 20,
        .inputActionMask = 0x00F0,
        .flags = StateLayerFlags::BlockLowerSimTicks | StateLayerFlags::BlockLowerInput
    });
    assert(r.has_value());
}

std::vector<u8> BuildPackWithEntries(std::span<const std::pair<assets::PakEntry, std::span<const u8>>> entries) {
    u32 totalPayloadSize = 0;
    for (const auto& [entry, payload] : entries) {
        totalPayloadSize += static_cast<u32>(payload.size());
    }
    const u32 entryCount = static_cast<u32>(entries.size());
    const u64 tocOffset = sizeof(assets::PakHeader) + totalPayloadSize;
    const usize totalPakBytes = static_cast<usize>(tocOffset) + entryCount * sizeof(assets::PakEntry);

    std::vector<u8> pak(totalPakBytes, 0);
    assets::PakHeader hdr{
        .magic = assets::kPakMagic,
        .version = assets::kPakVersion,
        .entryCount = entryCount,
        .flags = 0,
        .tocOffset = tocOffset,
        .contentHash = 0
    };
    std::memcpy(pak.data(), &hdr, sizeof(hdr));

    u64 curOffset = sizeof(assets::PakHeader);
    std::vector<assets::PakEntry> toc(entryCount);
    for (usize i = 0; i < entries.size(); ++i) {
        toc[i] = entries[i].first;
        toc[i].offset = curOffset;
        toc[i].storedSize = static_cast<u32>(entries[i].second.size());
        toc[i].rawSize = static_cast<u32>(entries[i].second.size());
        if (!entries[i].second.empty()) {
            std::memcpy(pak.data() + curOffset, entries[i].second.data(), entries[i].second.size());
        }
        curOffset += entries[i].second.size();
    }
    std::memcpy(pak.data() + tocOffset, toc.data(), entryCount * sizeof(assets::PakEntry));
    return pak;
}

void TestStateStackAndBitmaskOcclusion() {
    Application app{};
    TestAppState state{};
    ApplicationConfig cfg{.headless = true};
    AppHooks hooks{.userAppState = &state};

    auto initRes = app.Init(cfg, hooks);
    assert(initRes.has_value());
    assert(app.Director().PersistentScope() == 0);

    auto pushRes = app.Director().PushState(StateLayerDesc{
        .onSimTick = &OnBaseSimTick,
        .onRenderExtract = &OnBaseRender,
        .onEnter = &OnReentrantEnterPushOverlay,
        .onExit = &OnTrackExit,
        .stateData = &state,
        .id = 10,
        .inputActionMask = 0x000F,
        .flags = StateLayerFlags::None
    });
    assert(pushRes.has_value());
    assert(app.Director().HasPendingWork());

    auto step1 = app.StepHeadless(1, 0);
    assert(step1.has_value());
    assert(app.Director().StateDepth() == 2);
    assert(state.enterCount == 2);

    assert(app.Director().ActiveSimMask() == 0b10u);
    assert(app.Director().ActiveRenderMask() == 0b11u);
    assert(app.Director().TopInputMask() == 0x00F0u);
    assert(app.Input().GetActionMask() == 0x00F0u);
    assert(state.baseSimTicks == 0);
    assert(state.overlaySimTicks == 1);
    assert(state.baseRenders == 1);
    assert(state.overlayRenders == 1);

    assert(app.Director().SwitchState(StateLayerDesc{
        .onSimTick = nullptr,
        .onRenderExtract = &OnOverlayRender,
        .onEnter = &OnTrackEnter,
        .onExit = &OnTrackExit,
        .stateData = &state,
        .id = 30,
        .inputActionMask = 0xFF00,
        .flags = StateLayerFlags::BlockLowerRender | StateLayerFlags::BlockLowerInput | StateLayerFlags::PauseSimClock
    }).has_value());

    auto stepPause = app.StepHeadless(1, 0);
    assert(stepPause.has_value());
    assert(app.Director().IsClockPausedByState());
    assert(app.Time().IsPaused());
    assert((app.Time().FrozenMask() & time::kGameplayChannelsMask) == time::kGameplayChannelsMask);
    assert(app.Director().ActiveRenderMask() == 0b10u);
    assert(app.Director().TopInputMask() == 0xFF00u);

    assert(app.Director().PopState().has_value());
    auto step2 = app.StepHeadless(1, 0);
    assert(step2.has_value());
    assert(app.Director().StateDepth() == 1);
    assert(!app.Time().IsPaused());
    assert(app.Director().ActiveSimMask() == 0b01u);
    assert(app.Director().TopInputMask() == 0x000Fu);
    assert(state.baseSimTicks == 1);

    app.Shutdown();
    assert(state.exitCount == 3);
}

void TestCookedSceneManifestStreamingAndScopeRetirement() {
    Application app{};
    TestAppState state{};
    ApplicationConfig cfg{.headless = true};
    AppHooks hooks{
        .userAppState = &state,
        .OnConsumeSimCue = [](const gameplay::SimCue&, world::World&, const assets::AssetBank&, void* appState) noexcept {
            ++static_cast<TestAppState*>(appState)->cuesConsumed;
        }
    };

    assert(app.Init(cfg, hooks).has_value());

    struct PackedManifest {
        CookedSceneManifestHeader hdr;
        CookedSceneAssetEntry entry;
    } manifest{
        .hdr = CookedSceneManifestHeader{
            .magic = kSceneManifestMagic,
            .version = kSceneManifestVersion,
            .assetCount = 1,
            .collisionMapId = {0},
            .rootPrefabId = {3},
            .cueBankId = {0},
            .musicTrackId = 77,
            .musicCrossfadeBlocks = 48
        },
        .entry = CookedSceneAssetEntry{
            .id = {5},
            .kind = assets::AssetKind::Blob,
            .residency = assets::Residency::Presentation
        }
    };

    struct PackedPrefab {
        world::CookedPrefabHeader hdr;
        world::CookedPrefabEntityRecord rec;
    } rootPrefab{
        .hdr = world::CookedPrefabHeader{
            .magic = world::CookedPrefabHeader::kMagic,
            .version = 1,
            .entityCount = 1,
            .totalBytes = sizeof(PackedPrefab),
            .reserved = 0
        },
        .rec = world::CookedPrefabEntityRecord{
            .localTransform = {.x = 100.0f, .y = 50.0f},
            .tint = {1.0f, 1.0f, 1.0f, 1.0f},
            .flags = 0
        }
    };

    const u8 dummyBlob[16]{1, 2, 3, 4};

    const std::pair<assets::PakEntry, std::span<const u8>> pakEntries[] = {
        {
            assets::PakEntry{
                .assetId = 1,
                .kind = assets::AssetKind::Blob,
                .codec = assets::AssetCodec::Raw
            },
            std::span<const u8>{reinterpret_cast<const u8*>(&manifest), sizeof(manifest)}
        },
        {
            assets::PakEntry{
                .assetId = 3,
                .kind = assets::AssetKind::Blob,
                .codec = assets::AssetCodec::Raw
            },
            std::span<const u8>{reinterpret_cast<const u8*>(&rootPrefab), sizeof(rootPrefab)}
        },
        {
            assets::PakEntry{
                .assetId = 5,
                .kind = assets::AssetKind::Blob,
                .codec = assets::AssetCodec::Raw
            },
            std::span<const u8>{dummyBlob, sizeof(dummyBlob)}
        }
    };

    const std::vector<u8> scenePak = BuildPackWithEntries(pakEntries);
    assert(app.Assets().MountPackMemory(scenePak, 0).has_value());

    auto transRes = app.Director().RequestSceneTransition(SceneTransitionDesc{
        .sceneId = {101},
        .manifestBlobId = {1},
        .onActivated = [](SceneLoadContext& ctx, assets::AssetId id) noexcept -> bool {
            static_cast<TestAppState*>(ctx.userAppState)->sceneActivated = (id.value == 101);
            return true;
        },
        .onDeactivating = [](SceneLoadContext& ctx, assets::AssetId) noexcept {
            static_cast<TestAppState*>(ctx.userAppState)->sceneDeactivated = true;
        },
        .mode = SceneTransitionMode::FullReset,
        .waitPolicy = SceneWaitPolicy::StreamNonBlocking
    }, app.Assets());
    assert(transRes.has_value());
    assert(app.Director().Phase() == TransitionPhase::WaitingManifestBlob);

    assert(app.StepHeadless(1, 0).has_value());
    assert(app.Director().Phase() == TransitionPhase::StreamingSceneAssets);

    assert(app.StepHeadless(1, 0).has_value());
    assert(app.Director().Phase() == TransitionPhase::Idle);
    assert(state.sceneActivated);
    assert(app.World().AliveCount() == 1);
    assert(app.Director().ActiveSceneScope() == 1);

    assert(app.Director().RequestSceneTransition(SceneTransitionDesc{
        .sceneId = {102},
        .mode = SceneTransitionMode::RoomSwap,
        .waitPolicy = SceneWaitPolicy::ImmediateBarrier
    }, app.Assets()).has_value());

    assert(app.StepHeadless(1, 0).has_value());
    assert(state.sceneDeactivated);
    assert(app.Director().ActiveSceneId().value == 102);
    assert(app.Director().ActiveSceneScope() == 2);
    assert(app.World().AliveCount() == 1);

    auto camSpawn = app.World().Spawn(world::EntitySpawnDesc{
        .localTransform = {.x = 320.0f, .y = 180.0f}
    });
    assert(camSpawn.has_value());
    world::EntityId camId = *camSpawn;
    app.World().PropagateTransforms();
    assert(app.World().Emplace<world::CameraRig2D>(camId, world::CameraRig2D{.zoom = 1.0f, .viewSlot = 0, .screenSpace = false, .active = true}).has_value());
    assert(app.World().Emplace<gameplay::CameraController2D>(camId, gameplay::CameraController2D{}).has_value());

    app.Gameplay().EmitCue(gameplay::SimCue{
        .assetId = {99},
        .worldX = 300.0f,
        .worldY = 180.0f,
        .kind = gameplay::SimCueKind::AudioOneShot
    });
    app.Gameplay().EmitCue(gameplay::SimCue{
        .magnitude_q16 = 4096,
        .kind = gameplay::SimCueKind::CameraTraumaPulse
    });
    app.Gameplay().EmitCue(gameplay::SimCue{
        .assetId = {3},
        .worldX = 50.0f,
        .worldY = 75.0f,
        .kind = gameplay::SimCueKind::SpawnPrefabEffect
    });

    auto digestRes = app.StepHeadless(1, 2);
    assert(digestRes.has_value());
    auto* ctrl = app.World().TryGet<gameplay::CameraController2D>(camId);
    assert(ctrl != nullptr);
    assert(ctrl->trauma_q16 == 4096);
    assert(state.cuesConsumed == 3);
    assert(app.World().AliveCount() == 3);
    assert(digestRes->combinedHash != 0);

    app.Shutdown();
}

void TestDeterministicTwinRunAuditParity() {
    auto runSimulation = []() -> FrameAuditDigest {
        Application app{};
        TestAppState state{};
        ApplicationConfig cfg{.headless = true};
        AppHooks hooks{.userAppState = &state};
        assert(app.Init(cfg, hooks).has_value());
        assert(app.Director().PushState(StateLayerDesc{
            .onSimTick = &OnBaseSimTick,
            .onRenderExtract = &OnBaseRender,
            .stateData = &state,
            .id = 7,
            .inputActionMask = 0x0FFF
        }).has_value());
        auto digest = app.StepHeadless(10, 2);
        assert(digest.has_value());
        return *digest;
    };

    const FrameAuditDigest runA = runSimulation();
    const FrameAuditDigest runB = runSimulation();
    assert(runA.tickIndex == 10);
    assert(runA.combinedHash == runB.combinedHash);
}

} // namespace

int main() {
    TestStateStackAndBitmaskOcclusion();
    TestCookedSceneManifestStreamingAndScopeRetirement();
    TestDeterministicTwinRunAuditParity();
    std::printf("All EngineN Phase 10 (framework) tests passed!\n");
    return 0;
}

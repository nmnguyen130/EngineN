#include "framework/Application.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace en::tests {

struct TestSuite {
    u32 passed{0U};
    u32 failed{0U};

    void Expect(bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::printf("[FAIL] Line %d: %s\n", line, expr);
        }
    }
};

#define EXPECT_TRUE(suite, expr) (suite).Expect(static_cast<bool>(expr), #expr, __LINE__)
#define EXPECT_EQ(suite, a, b) (suite).Expect((a) == (b), #a " == " #b, __LINE__)
#define EXPECT_NEAR(suite, a, b, eps) (suite).Expect(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __LINE__)

// Compile-time struct size & cache-line invariants (0B hidden compiler padding across all structs)
static_assert(sizeof(FrameworkError) == 4);
static_assert(sizeof(CookedSceneManifestHeader) == 16);
static_assert(sizeof(CookedSceneAssetEntry) == 16);
static_assert(sizeof(SimContext) == 64);
static_assert(sizeof(RenderContext) == 64);
static_assert(sizeof(SceneLoadContext) == 64);
static_assert(sizeof(StateLayerDesc) == 48);
static_assert(sizeof(SceneTransitionDesc) == 64);
static_assert(sizeof(FrameAuditDigest) == 64);
static_assert(sizeof(AppHooks) == 64);
static_assert(sizeof(SceneDirector) == 576);
static_assert(sizeof(ApplicationConfig) == 96);
static_assert(sizeof(Application) == 64);

struct ClientGameState {
    EntityId hero{kNullEntity};
    EntityId transientEnemy{kNullEntity};
    EntityId transientSpark{kNullEntity};
    SoundCueId slashCue{};
    u32 baseTickCalls{0U};
    u32 overlayTickCalls{0U};
    u32 renderCalls{0U};
    u32 cuesConsumed{0U};
    u32 sceneActivations{0U};
    u32 sceneDeactivations{0U};
};

void TestStateStackRebakeMasksAndIntrospection(TestSuite& suite) noexcept {
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());

    AudioSystem audio;
    EXPECT_TRUE(suite, audio.Init().has_value());

    World world;
    EXPECT_TRUE(suite, world.Init().has_value());

    SceneDirector director;
    EXPECT_TRUE(suite, director.Init(bank).has_value());
    EXPECT_EQ(suite, director.TopStateId(), kInvalidStateId);

    ClientGameState client{};

    StateLayerDesc gameplayLayer{};
    gameplayLayer.id = 1U;
    gameplayLayer.inputActionMask = 0x00FFU;
    gameplayLayer.flags = StateLayerFlags::None;
    gameplayLayer.stateData = &client;
    gameplayLayer.onSimTick = [](SimContext&, const SimStepView&, void* data) noexcept {
        ++static_cast<ClientGameState*>(data)->baseTickCalls;
    };
    gameplayLayer.onRenderExtract = [](RenderContext&, void* data) noexcept {
        ++static_cast<ClientGameState*>(data)->renderCalls;
    };

    EXPECT_TRUE(suite, director.PushState(gameplayLayer).has_value());

    SceneLoadContext loadCtx{};
    loadCtx.world = &world;
    loadCtx.assetBank = &bank;
    loadCtx.audio = &audio;
    loadCtx.director = &director;
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.StateDepth(), 1U);
    EXPECT_EQ(suite, director.TopStateId(), 1U);
    EXPECT_TRUE(suite, director.HasState(1U));
    EXPECT_TRUE(suite, !director.HasState(2U));
    EXPECT_EQ(suite, director.ActiveSimMask(), 0x01U);
    EXPECT_EQ(suite, director.ActiveRenderMask(), 0x01U);
    EXPECT_EQ(suite, director.TopInputMask(), 0x00FFU);
    EXPECT_TRUE(suite, !director.IsClockPausedByState());

    // Push a modal Pause Overlay that blocks lower sim ticks & input and pauses the sim clock, while allowing lower render
    StateLayerDesc pauseLayer{};
    pauseLayer.id = 2U;
    pauseLayer.inputActionMask = 0x0003U;
    pauseLayer.flags = StateLayerFlags::BlockLowerSimTicks |
                       StateLayerFlags::BlockLowerInput |
                       StateLayerFlags::PauseSimClock;
    pauseLayer.stateData = &client;
    pauseLayer.onSimTick = [](SimContext&, const SimStepView&, void* data) noexcept {
        ++static_cast<ClientGameState*>(data)->overlayTickCalls;
    };
    pauseLayer.onRenderExtract = [](RenderContext&, void* data) noexcept {
        ++static_cast<ClientGameState*>(data)->renderCalls;
    };

    EXPECT_TRUE(suite, director.PushState(pauseLayer).has_value());
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.StateDepth(), 2U);
    EXPECT_EQ(suite, director.TopStateId(), 2U);
    EXPECT_TRUE(suite, director.HasState(1U));
    EXPECT_TRUE(suite, director.HasState(2U));
    EXPECT_EQ(suite, director.ActiveSimMask(), 0x02U);    // Lower layer 0 blocked from sim
    EXPECT_EQ(suite, director.ActiveRenderMask(), 0x03U); // Both layer 0 and layer 1 rendered
    EXPECT_EQ(suite, director.TopInputMask(), 0x0003U);   // Input masked to pause overlay
    EXPECT_TRUE(suite, director.IsClockPausedByState());

    SimContext simCtx{};
    SimStepView step{};
    director.DispatchSimTick(simCtx, step);
    EXPECT_EQ(suite, client.baseTickCalls, 0U);
    EXPECT_EQ(suite, client.overlayTickCalls, 1U);

    RenderContext renderCtx{};
    director.DispatchRenderExtract(renderCtx);
    EXPECT_EQ(suite, client.renderCalls, 2U);

    // Pop Pause Overlay -> restores gameplay layer masks
    EXPECT_TRUE(suite, director.PopState().has_value());
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.StateDepth(), 1U);
    EXPECT_EQ(suite, director.TopStateId(), 1U);
    EXPECT_EQ(suite, director.ActiveSimMask(), 0x01U);
    EXPECT_EQ(suite, director.TopInputMask(), 0x00FFU);
    EXPECT_TRUE(suite, !director.IsClockPausedByState());

    // Test SwitchState and ClearAndPushState
    StateLayerDesc cutsceneLayer{};
    cutsceneLayer.id = 3U;
    cutsceneLayer.inputActionMask = 0x0001U;
    EXPECT_TRUE(suite, director.SwitchState(cutsceneLayer).has_value());
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.StateDepth(), 1U);
    EXPECT_EQ(suite, director.TopStateId(), 3U);
    EXPECT_EQ(suite, director.TopInputMask(), 0x0001U);

    EXPECT_TRUE(suite, director.ClearAndPushState(gameplayLayer).has_value());
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.StateDepth(), 1U);
    EXPECT_EQ(suite, director.TopStateId(), 1U);
    EXPECT_EQ(suite, director.TopInputMask(), 0x00FFU);

    director.Shutdown(bank, audio, &loadCtx);
    world.Shutdown();
    audio.Shutdown();
    bank.Shutdown();
}

void TestCookedSceneManifestChecksumAndScreenSpaceTransitionWipes(TestSuite& suite) noexcept {
    AssetBank bank;
    EXPECT_TRUE(suite, bank.Init().has_value());
    AudioSystem audio;
    EXPECT_TRUE(suite, audio.Init().has_value());
    World world;
    EXPECT_TRUE(suite, world.Init().has_value());
    SceneDirector director;
    EXPECT_TRUE(suite, director.Init(bank).has_value());

    // Build a valid binary ESCN v1 manifest referencing one pre-committed Blob asset with verified checksum
    const AssetId depBlobId = AssetId::Hash("level_data.bin");
    bool needsLoad = false;
    const u16 depSlot = bank.AcquireSlot(depBlobId, AssetKind::Blob, director.PersistentScope(), AssetResidency::Sim, needsLoad);
    const u8 dummyBytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    (void)bank.CommitBlob(depSlot, dummyBytes);

    struct ManifestPayload {
        CookedSceneManifestHeader hdr{};
        CookedSceneAssetEntry entries[1]{};
    } manifest{};
    manifest.hdr.magic = CookedSceneManifestHeader::kMagic;
    manifest.hdr.version = CookedSceneManifestHeader::kVersion;
    manifest.hdr.assetEntryCount = 1U;
    manifest.entries[0].id = depBlobId;
    manifest.entries[0].kind = AssetKind::Blob;
    manifest.entries[0].residency = AssetResidency::Sim;
    manifest.hdr.checksum = ComputeSceneManifestChecksum(manifest.entries);

    const AssetId manifestId = AssetId::Hash("boss_room.escn");
    const u16 mSlot = bank.AcquireSlot(manifestId, AssetKind::Blob, director.PersistentScope(), AssetResidency::Sim, needsLoad);
    (void)bank.CommitBlob(mSlot, std::span<const u8>{reinterpret_cast<const u8*>(&manifest), sizeof(manifest)});

    SceneTransitionDesc trans{};
    trans.sceneId = AssetId::Hash("boss_room");
    trans.manifestBlobId = manifestId;
    trans.transitionDurationTicks = 2U;
    trans.wipe = TransitionWipeKind::DiamondIris;
    EXPECT_TRUE(suite, director.RequestSceneTransition(trans, bank).has_value());
    EXPECT_EQ(suite, director.Phase(), TransitionPhase::FadingOut);

    director.StepTransitionWipeTick();
    EXPECT_NEAR(suite, director.TransitionProgress(), 0.5f, 1e-4f);
    director.StepTransitionWipeTick();
    EXPECT_EQ(suite, director.Phase(), TransitionPhase::StreamingSceneAssets);

    SceneLoadContext loadCtx{};
    loadCtx.world = &world;
    loadCtx.assetBank = &bank;
    loadCtx.audio = &audio;
    loadCtx.director = &director;
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.Phase(), TransitionPhase::FadingIn);
    EXPECT_EQ(suite, director.ActiveSceneId(), AssetId::Hash("boss_room"));
    EXPECT_EQ(suite, director.TransitionCount(), 1U);

    director.StepTransitionWipeTick();
    director.StepTransitionWipeTick();
    EXPECT_EQ(suite, director.Phase(), TransitionPhase::Idle);

    // Test ReloadActiveScene()
    EXPECT_TRUE(suite, director.ReloadActiveScene(bank).has_value());
    EXPECT_TRUE(suite, director.CommitSafeFramePoint(loadCtx).value_or(false));
    EXPECT_EQ(suite, director.TransitionCount(), 2U);

    // Verify corrupted manifest checksum rejection
    manifest.hdr.checksum ^= 0xDEADBEEFU;
    const AssetId badManifestId = AssetId::Hash("corrupt.escn");
    const u16 badSlot = bank.AcquireSlot(badManifestId, AssetKind::Blob, director.PersistentScope(), AssetResidency::Sim, needsLoad);
    (void)bank.CommitBlob(badSlot, std::span<const u8>{reinterpret_cast<const u8*>(&manifest), sizeof(manifest)});

    SceneTransitionDesc badTrans{};
    badTrans.sceneId = AssetId::Hash("bad_room");
    badTrans.manifestBlobId = badManifestId;
    badTrans.transitionDurationTicks = 0U;
    badTrans.wipe = TransitionWipeKind::None;
    EXPECT_TRUE(suite, director.RequestSceneTransition(badTrans, bank).has_value());
    const auto badRes = director.CommitSafeFramePoint(loadCtx);
    EXPECT_TRUE(suite, !badRes.has_value());
    EXPECT_EQ(suite, badRes.error(), FrameworkErrc::CorruptedSceneManifest);

    director.Shutdown(bank, audio, &loadCtx);
    world.Shutdown();
    audio.Shutdown();
    bank.Shutdown();
}

void TestFull12SubsystemApplicationLoopModalPauseRoomSwapAndReplayDeterminism(TestSuite& suite) noexcept {
    InputReplayStream recordedStream{};

    const auto runFullEngineSimulation = [&](ReplayMode replayMode) noexcept -> FrameAuditDigest {
        ClientGameState client{};

        AppHooks hooks{};
        hooks.userAppState = &client;
        hooks.onBoot = [](SceneLoadContext& ctx) noexcept -> Result<void, FrameworkErrc> {
            auto* st = static_cast<ClientGameState*>(ctx.userAppState);

            // Commit a procedural sine wave audio clip & register a combat SoundCueDef + MusicTrackDef
            std::vector<f32> pcm(512U);
            for (u32 i = 0U; i < 512U; ++i) {
                pcm[i] = std::sin(2.0f * kPi * static_cast<f32>(i) / 64.0f);
            }
            bool needsLoad = false;
            const u16 aSlot = ctx.assetBank->AcquireSlot(
                AssetId::Hash("slash.wav"), AssetKind::Audio, ctx.persistentScope, AssetResidency::Sim, needsLoad);
            const AudioHandle clipH = ctx.assetBank->CommitAudio(aSlot, pcm, 48000U, 1U, true);

            SoundCueDef cue{};
            cue.id = SoundCueId::FromAssetId(AssetId::Hash("sfx_slash"));
            cue.clipVariants[0] = clipH;
            cue.SetBus(AudioBusId::SfxCombat);
            cue.flags = VoiceFlags::Spatialized2D;
            (void)ctx.audio->RegisterCueDef(cue, ctx.persistentScope);
            st->slashCue = cue.id;

            MusicTrackDef track{};
            track.trackId = 100U;
            track.stemCount = 1U;
            track.stems[0] = clipH;
            track.defaultWeights_q14[0] = 16384U;
            (void)ctx.audio->RegisterMusicTrackDef(track, ctx.persistentScope);

            // Push initial gameplay state layer
            StateLayerDesc mainState{};
            mainState.id = 10U;
            mainState.stateData = st;
            mainState.onSimTick = [](SimContext& sim, const SimStepView& step, void* data) noexcept {
                auto* game = static_cast<ClientGameState*>(data);
                ++game->baseTickCalls;
                if (game->hero.IsValid() && sim.world->IsAlive(game->hero)) {
                    if (auto* ms = sim.world->TryGet<MotionStack2D>(game->hero)) {
                        ms->targetWalkVx = sim.tickInput->Held(0U) ? 6.0f : 3.0f;
                        if (step.tickIndex == 3ULL) {
                            (void)ms->TryExecuteJump(8.0f);
                            SimCue audioCue{};
                            audioCue.kind = SimCueKind::AudioOneShot;
                            audioCue.assetId = AssetId::Hash("sfx_slash");
                            audioCue.worldX = 2.0f;
                            audioCue.worldY = 1.0f;
                            audioCue.emitter = game->hero;
                            sim.gameplay->EmitCue(audioCue);

                            SimCue shakeCue{};
                            shakeCue.kind = SimCueKind::CameraTraumaPulse;
                            shakeCue.SetMagnitude(0.5f);
                            sim.gameplay->EmitCue(shakeCue);
                        }
                    }
                }
            };
            (void)ctx.director->PushState(mainState);

            // Request initial scene transition with FadeToBlack wipe & automatic music track 100U
            SceneTransitionDesc scene1{};
            scene1.sceneId = AssetId::Hash("level_01");
            scene1.musicTrackId = 100U;
            scene1.musicCrossfadeBlocks = 8U;
            scene1.transitionDurationTicks = 2U;
            scene1.wipe = TransitionWipeKind::FadeToBlack;
            scene1.onActivated = [](SceneLoadContext& lctx) noexcept -> Result<void, FrameworkErrc> {
                auto* g = static_cast<ClientGameState*>(lctx.userAppState);
                ++g->sceneActivations;

                TileMaterialFlags floorTiles[16]{};
                for (u32 i = 0U; i < 8U; ++i) floorTiles[i] = TileMaterialFlags::Solid;
                (void)lctx.physics->BuildTileMap(8U, 2U, 1.0f, -2.0f, -1.0f, floorTiles);

                EntitySpawnDesc sp{};
                sp.local = LocalTransform2D{1.0f, 1.0f};
                const EntityId hero = lctx.world->Spawn(sp).value_or(kNullEntity);
                g->hero = hero;

                (void)lctx.world->Emplace<Collider2D>(hero, Collider2D{
                    .localBounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f),
                    .shape = ShapeKind2D::CapsuleY,
                });
                (void)lctx.world->Emplace<CharacterMotor2D>(hero, CharacterMotor2D{
                    .bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f),
                });
                (void)lctx.world->Emplace<MotionStack2D>(hero, MotionStack2D{});
                (void)lctx.world->Emplace<CameraRig2D>(hero, CameraRig2D{});
                CameraController2D camCtrl{};
                camCtrl.primaryTarget = hero;
                (void)lctx.world->Emplace<CameraController2D>(hero, camCtrl);

                // Spawn a room-transient enemy and a temporary particle with Lifetime2D
                g->transientEnemy = lctx.world->Spawn(sp).value_or(kNullEntity);
                (void)lctx.world->Emplace<Tag2D>(g->transientEnemy, Tag2D{
                    .name = StringId::Hash("room_mob"),
                    .categoryMask = kRoomTransientCategoryBit,
                });
                g->transientSpark = lctx.world->Spawn(sp).value_or(kNullEntity);
                (void)lctx.world->Emplace<Lifetime2D>(g->transientSpark, Lifetime2D{.remainingTicks = 200U});
                return {};
            };
            scene1.onDeactivating = [](SceneLoadContext& lctx) noexcept {
                ++static_cast<ClientGameState*>(lctx.userAppState)->sceneDeactivations;
            };
            return ctx.director->RequestSceneTransition(scene1, *ctx.assetBank);
        };
        hooks.onConsumeSimCue = [](const SimCue&, SimContext&, void* userAppState) noexcept {
            ++static_cast<ClientGameState*>(userAppState)->cuesConsumed;
        };

        Application app;
        ApplicationConfig cfg{};
        EXPECT_TRUE(suite, app.Init(cfg, hooks).has_value());
        app.Input().Map().Bind(0U, {InputSource::Key(32U)});
        app.BindReplayStream(&recordedStream, replayMode);

        if (replayMode == ReplayMode::Record) {
            app.Input().FeedButton(InputSource::Key(32U), true);
        }

        // Step 6 ticks: ticks 1..2 complete FadeToBlack out, frame 3 activates scene_01, ticks 3..6 run hero physics/gameplay/audio
        const auto digestRes = app.StepHeadless(6U, 2U);
        EXPECT_TRUE(suite, digestRes.has_value());

        EXPECT_EQ(suite, client.sceneActivations, 1U);
        EXPECT_EQ(suite, client.baseTickCalls, 6U);
        EXPECT_TRUE(suite, client.hero.IsValid());
        EXPECT_TRUE(suite, app.GetWorld().IsAlive(client.hero));
        EXPECT_TRUE(suite, app.GetWorld().IsAlive(client.transientEnemy));
        EXPECT_TRUE(suite, app.GetWorld().IsAlive(client.transientSpark));

        // Push a modal Pause Overlay with PauseSimClock; verify overlay ticks at 120 Hz while world/base layer freeze,
        // and verify popping the overlay automatically resumes world simulation!
        const f32 heroXBeforePause = app.GetWorld().GetLocalTransform(client.hero).x;
        StateLayerDesc modalPause{};
        modalPause.id = 99U;
        modalPause.flags = StateLayerFlags::BlockLowerSimTicks |
                           StateLayerFlags::BlockLowerInput |
                           StateLayerFlags::PauseSimClock;
        modalPause.stateData = &client;
        modalPause.onSimTick = [](SimContext& sim, const SimStepView&, void* data) noexcept {
            auto* g = static_cast<ClientGameState*>(data);
            if (++g->overlayTickCalls == 2U) {
                (void)sim.director->PopState();
            }
        };
        EXPECT_TRUE(suite, app.Director().PushState(modalPause).has_value());

        // Step 2 ticks while paused: overlay runs 2 ticks and queues PopState() on tick 2, while hero does not move!
        EXPECT_TRUE(suite, app.StepHeadless(2U, 2U).has_value());
        EXPECT_EQ(suite, client.overlayTickCalls, 2U);
        EXPECT_EQ(suite, client.baseTickCalls, 6U);
        EXPECT_NEAR(suite, app.GetWorld().GetLocalTransform(client.hero).x, heroXBeforePause, 1e-6f);

        // Trigger a RoomSwap transition to level_02: preserves hero, destroys transientEnemy and transientSpark!
        SceneTransitionDesc scene2{};
        scene2.sceneId = AssetId::Hash("level_02");
        scene2.mode = SceneTransitionMode::RoomSwap;
        scene2.transitionDurationTicks = 0U;
        scene2.wipe = TransitionWipeKind::None;
        scene2.onActivated = [](SceneLoadContext& lctx) noexcept -> Result<void, FrameworkErrc> {
            ++static_cast<ClientGameState*>(lctx.userAppState)->sceneActivations;
            return {};
        };
        EXPECT_TRUE(suite, app.Director().RequestSceneTransition(scene2, app.Assets()).has_value());

        const auto finalDigest = app.StepHeadless(4U, 2U);
        EXPECT_TRUE(suite, finalDigest.has_value());
        EXPECT_EQ(suite, client.sceneDeactivations, 1U);
        EXPECT_EQ(suite, client.sceneActivations, 2U);
        EXPECT_EQ(suite, client.baseTickCalls, 10U); // Automatically resumed after modal pause popped!
        EXPECT_TRUE(suite, app.GetWorld().IsAlive(client.hero));            // Preserved across RoomSwap
        EXPECT_TRUE(suite, !app.GetWorld().IsAlive(client.transientEnemy)); // Destroyed by kRoomTransientCategoryBit
        EXPECT_TRUE(suite, !app.GetWorld().IsAlive(client.transientSpark)); // Destroyed by Lifetime2D

        const FrameAuditDigest out = *finalDigest;
        app.Shutdown();
        return out;
    };

    recordedStream.Clear();
    const FrameAuditDigest dRecord = runFullEngineSimulation(ReplayMode::Record);
    EXPECT_TRUE(suite, recordedStream.SegmentCount() > 0U);

    // Run second simulation with zero hardware input, driven purely by ReplayMode::Playback!
    const FrameAuditDigest dPlayback = runFullEngineSimulation(ReplayMode::Playback);
    EXPECT_EQ(suite, dRecord.tickIndex, 12ULL);
    EXPECT_EQ(suite, dRecord.worldHash, dPlayback.worldHash);
    EXPECT_EQ(suite, dRecord.physicsHash, dPlayback.physicsHash);
    EXPECT_EQ(suite, dRecord.gameplayHash, dPlayback.gameplayHash);
    EXPECT_EQ(suite, dRecord.audioHash, dPlayback.audioHash);
    EXPECT_EQ(suite, dRecord.renderListHash, dPlayback.renderListHash);
    EXPECT_EQ(suite, dRecord.directorHash, dPlayback.directorHash);
    EXPECT_EQ(suite, dRecord.combinedHash, dPlayback.combinedHash);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestSuite suite{};
    TestStateStackRebakeMasksAndIntrospection(suite);
    TestCookedSceneManifestChecksumAndScreenSpaceTransitionWipes(suite);
    TestFull12SubsystemApplicationLoopModalPauseRoomSwapAndReplayDeterminism(suite);

    std::printf("Phase 12 v2 Framework Verification: %u passed, %u failed\n", suite.passed, suite.failed);
    return (suite.failed == 0U) ? 0 : 1;
}

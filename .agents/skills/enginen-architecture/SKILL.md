---
name: enginen-architecture
description: Architect, refactor, and build C++23 client games and subsystems on top of EngineN. Trigger whenever the user mentions EngineN, client game layers, SceneDirector, GameplayPolicyTable, AppHooks, or any EngineN subsystem (platform, input, time, assets, renderer, world, gameplay, audio, debug, framework).
---
# EngineN Architecture & Client Game Guide

Provide architectural continuity, strict two-layer boundary enforcement, and inter-subsystem communication rules for the locked 10-subsystem EngineN runtime and client games built on top of it.

## When to Use

- Building or extending a C++23 client game on top of EngineN (`AppHooks`, `StateLayerDesc`, `GameplayPolicyTable`, `SceneTransitionDesc`).
- Inspecting, integrating, or maintaining any of the 10 locked EngineN subsystems (`time`, `platform`, `input`, `renderer`, `assets`, `world`, `gameplay`, `audio`, `debug`, `framework`).

## Reference Context Files (Load On-Demand Only)

All 10 subsystem specifications are locked in `references/`. To save context tokens, load only the reference files relevant to the current task using `skills:get_skill_resources`:

- `references/Time_Context.md` — **Phase 1 (`core/time/`):** `TimeTypes.h`, `TimePipeline.h`, `SdlClock.h/.cpp` (120 Hz rational accumulator, VSync harmonic snapping, 64B `BakedTickState` at Cache Line 0, 16B `ChannelModSlot`, `RequestStep`, `RequestGameplayFreeze`, O(1) `FrozenMask()` & `EffectiveScales()`).
- `references/Platform_Context.md` — **Phase 2 (`platform/`):** `PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp` (64B `Window`, 4B `PlatformInputEvent`, 256B `PlatformFrameEvents`, single-pass coalesced `Pump`, deferred `RefreshMetrics()`, bitmask gamepad SoA, single-pass `FixedString::Resolve`).
- `references/Input_Context.md` — **Phase 3 (`input/`):** `InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h` (640B header-only L1-partitioned `InputPipeline`, single-call `IngestPlatformFrame`, dual `m_tickTap`/`m_frameTap` latches, 36B `ActionBuffer` with O(1) `PendingPressMask()`, 64B `TickInput` & `FrameInput`).
- `references/Renderer_Context.md` — **Phase 4 (`renderer/`):** `RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp` (zero `platform/` header coupling via `ViewportExtent`, 1.26 MB L2/L3-resident `RenderList` with `kMaxQuads = 16384`, 64B corner-offset `PreparedQuad`, single-pass active-byte radix sort, 16B lazy high-water `TextureSlot`, `BeginFrame(pixelSize)`).
- `references/Assets_Context.md` — **Phase 5 (`assets/`):** `AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp` (single-slab 64B-aligned arenas in `AssetBank` & `AssetSystem`, Cache Line 0 hot table pointers, 8B embedded-generation `AtlasRecord`, intrusive freelists, `constexpr` O(1) `IsScopeSimReady(scope)`, unified single-probe `VfsTocSlot`, 64B FIFO `PendingJob`, unified `Pin(scope, id, kind, residency)`).
- `references/World_Context.md` — **Phase 6 (`world/`):** `WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp` (192B 3-cache-line `World` over 4 MB 64B-aligned arena, single-array `IsAlive`, 64B iterative `TransformSolver` with `m_dirtyWords` & `m_movedWords`, 32B `ComponentPool<T>` with `DirectAtEntityIndex`, stationary sprite bypass in `WorldExtractor`).
- `references/Gameplay_Context.md` — **Phase 7 (`gameplay/`):** `GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp` (64B `SpatialIndex2D` with 24B `GridBox` & stage-specialized `RebuildDynamicGrid`, Swept-AABB `KinematicSolver2D`, single-pass `StateAndCombatSolver`, O(1) Bloom-deduplicated combat hits, 2nd-order spring `CameraController2D`, 704B `GameplayPipeline`).
- `references/Audio_Context.md` — **Phase 8 (`audio/`):** `AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp` (zero `world/`/`gameplay/`/`platform/` header includes, 64B `AudioMixer` over 19.8 KB L1-resident `MixerArenaState`, Hermite4 + Cytomic SVF, in-place stolen-voice tail accumulation, 128B `AudioSystem`, 32B `MainVoiceTracker`, `PollReleasableScopeMask()`, single-call `Update(AudioUpdateDesc)`).
- `references/Debug_Context.md` — **Phase 9 (`debug/`):** `DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp` (zero release overhead when `EN_ENABLE_DEBUG_TOOLS == 0`, 116 KB arena, 16B `DebugBatcher`, 2-level bitmask `DebugShapePool`, 1-hop `DebugInspector`, 64B `FrameProfileSample`, 8B `DebugLoopDirectives`).
- `references/Framework_Context.md` — **Phase 10 (`framework/`):** `FrameworkTypes.h`, `SceneDirector.h/.cpp`, `Application.h/.cpp` (zero Layer 2 header leaks in Layer 1, 832B `SceneDirector` with Cache Line 0 bitmasks & single-pass top-down `RebakeStackMasks()`, 64B Cache Line 0 `Application` coordinator, unified `ExtractRenderFrame()`, 1-pass `CameraRig2D` pool lookup, full `SimCue` & `DebugSystem` orchestration).

## Core Engine Invariants

1. **C++23 Data-Oriented Design:** Zero heap allocation after `Init()`, `-fno-exceptions`, `-fno-rtti`, zero virtual dispatch (function-pointer tables only), strict two-layer separation (Layer 1 pure C++23 testable in headless CI via `ComputeAuditHash()`, Layer 2 thin SDL3/hardware coordinator with `<SDL3/SDL_*.h>` strictly confined to `.cpp`).
2. **Inversion of Control:** Client code never writes a main loop or invokes `World::BeginTick` / `Renderer::Submit` directly. `en::framework::Application` drives the runtime and invokes client callbacks via `AppHooks`, `StateLayerDesc`, `GameplayPolicyTable`, and `SceneTransitionDesc`.
3. **Phase-Scoped Context Access:**
   - **Boot / Scene & State Lifecycle (`SceneLoadContext&`, `64B`):** Mutable access to `world`, `gameplay`, `assets`, `audio`, `renderer`, `sceneScope`, `persistentScope`, `director`.
   - **120 Hz Simulation (`SimContext&`, `128B`):** Mutable access to `world`, `gameplay`, `audio`, `platform`, `actionBuffer`, `director`; read-only `assetBank`, `tickInput`, `step`. (`Renderer` and `AssetSystem` are omitted for determinism).
   - **Render Extraction (`RenderContext&`, `128B`):** Read-only `world`, `gameplay`, `assetBank`, `frameInput`, `windowState`, `timeView`; mutable `renderList`, `director`. `WorldExtractor::Extract()` automatically submits world `Sprite2D` entities; client `onRenderExtract` only emits custom UI/HUD primitives.
4. **Structural ECS & Physics Rules:**
   - Inside `onSimTick` or gameplay policy callbacks, always use `world.DeferDestroy()`, `world.DeferEmplace()`, and `world.DeferRemove()` while iterating pools/views (flushed deterministically at `World::EndTick()`).
   - Colliding actors use `gameplay::KinematicMotor2D` (`kTypeId = 16`) + `MotionStack2D` (`kTypeId = 18`). Never attach `world::Velocity2D` (`kTypeId = 1`) to colliding actors (`Velocity2D` is strictly for non-colliding ambient props/particles).
   - Custom client components must use `kTypeId` in `[24..63]` and register inside `AppHooks::OnRegisterComponents`.
   - Prefer direct dense pool iteration (`world.GetPool<T>()`) + 1-hop sibling lookup (`world.GetComponentMaskByIndex(idx)` + `pool.DirectAtEntityIndex(idx)`) over repeated `world.TryGet<T>()` calls.
   - Never branch on asset readiness in hot paths; `AssetBank` resolves unready handles branchlessly to Slot 0 fallbacks.

## Locked Inter-Subsystem Communication Contracts

- **Time (`core/time/`):** `TimePipeline` exposes O(1) pause-aware `m_time.FrozenMask()` (`u8`, automatically OR-ed with `kGameplayChannelsMask = 0x3Fu` when paused) and `m_time.EffectiveScales()` (`std::span<const f32, 8>`). Never loop over `0..7` channels externally to latch scales or frozen masks.
- **Platform (`platform/`) -> Input (`input/`):** `platform/` never includes headers from `input/` or higher layers. `Platform::Pump()` defers `Window::RefreshMetrics()` until after `SDL_PollEvent`, coalesces high-rate motion/axis events into a 256B `PlatformFrameEvents` batch, and feeds `InputPipeline` via a single `input.IngestPlatformFrame(m_frameEvents)` call.
- **Renderer (`renderer/`):** Zero `platform/` header includes (`ViewportExtent` converts structurally from `Extent2i`). `Application::ExtractRenderFrame()` synchronizes viewport dimensions and resets the command buffer in a single `m_renderer.BeginFrame(viewportSize)` call across both windowed and headless modes.
- **Assets (`assets/`):** Zero `platform/` includes. `SceneDirector` pins manifest entries via unified `m_assets.Pin(scope, id, kind, residency)` and queries `m_assets.Bank().IsScopeSimReady(scope)` in `constexpr` O(1) (`m_scopeSimPending[scope] == 0`).
- **World (`world/`):** Layer 1 has zero `AssetBank.h` or `RenderList.h` includes. `World::BeginTick(step)` snapshots only entities with bits set in `m_movedWords`. `WorldExtractor::Extract()` uses `world.MovedWords()` to bypass `PrevWorldSnapshot2D` and `TimeChannel` loads for stationary sprites.
- **Gameplay (`gameplay/`):** Never calls `AudioSystem`, `Renderer`, or `DebugSystem` directly. Outputs simulation events via `DrainCues()` (`SimCue` `32B`), rebuilds the spatial grid in `KinematicsOnly` mode in Stage 4 and `CombatOnlyIfActive` mode in Stage 5, and propagates camera transforms at the end of Stage 6.
- **Audio (`audio/`):** Zero `world/`, `gameplay/`, `platform/`, `renderer/`, or `input/` header includes. Consumes decoupled `AudioUpdateDesc` (containing `AudioTriggerReq` span, `EffectiveScales()`, `FrozenMask()`, listener coords, and `AudioOcclusionQueryFn` wrapping `SpatialIndex2D::Raycast`) in a single `m_audio.Update(desc)` call. Retires scopes asynchronously via `RequestScopeRelease(scope)` -> `PollReleasableScopeMask()` (`m_assets.EndScope`).
- **Debug (`debug/`):** Zero `platform/`, `audio/`, or `AssetBank.h` header includes in Layer 1. `BeginFrame(DebugFrameContext)` returns an 8B `DebugLoopDirectives` packet applied by `Application`; `RecordSubsystemTelemetry(SubsystemTelemetryDesc)` records frame metrics; `EmitOverlays(DebugFrameContext, list)` renders world/HUD overlays.
- **Framework (`framework/`):** Layer 1 (`FrameworkTypes.h`, `SceneDirector.h/.cpp`) includes only Layer 1 `*Types.h` headers (`0` Layer 2 header leaks). `SceneDirector` packs hot masks/scopes in Cache Line 0, rebakes masks in a single 15-line top-down pass (`RebakeStackMasks()`), and exposes O(1) `HasPendingWork()` so `Application::StepFrame()` skips `CommitSafeFrameBarrier()` on normal frames.

## Canonical Frame Execution Order (`Application::StepFrame`)

1. **OS, Asset & Debug Begin:** `Platform::Pump(m_window, m_input, m_devToolsTap)` -> `AssetSystem::Pump(m_renderer, m_assetPumpBudget)` -> `DebugSystem::BeginFrame(DebugFrameContext)`.
2. **Safe Frame Barrier (`if (m_director.HasPendingWork()) CommitSafeFrameBarrier()`):** Advances 2-stage scene streaming (`WaitingManifestBlob` -> `StreamingSceneAssets`). On activation: pre-validates staged blobs, binds `.eaud` / `.ecol`, invokes active scene's `m_activeSceneDeactivateFn`, optionally clears `World` (`FullReset`), instantiates `.epfb`, transitions music, retires `oldScope` via `AudioSystem::RequestScopeRelease(oldScope)`, resets `TimeManager::OnWindowRefocus` + `ActionBuffer::Clear`, and fires `onActivated`. Commits queued state commands (`PushState`/`PopState`/`SwitchState`/`ClearAndPushState`), rebakes masks in 1 top-down pass, and syncs `InputPipeline::SetActionMask` + `TimeManager::SetPaused` (followed by `DebugLoopDirectives` overrides).
3. **120 Hz Simulation Loop (`ExecuteSimTicks`: `while (step = m_time.PollTick())`):**
   - `InputPipeline::PollTick` -> `ActionBuffer::Feed`
   - `World::BeginTick(step)`
   - `GameplayPipeline::Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer)`
   - `SceneDirector::DispatchSimTick(SimContext&)` -> active `StateLayerDesc::onSimTick`
   - `World::EndTick(step)` -> `DebugSystem::EndSimTick`
4. **Scope Reclaim, Cue Routing & Audio Sync (`RouteCuesAndSyncAudio`):** `AudioSystem::PollReleasableScopeMask()` (`m_assets.EndScope`) -> 1-pass `CameraRig2D` pool listener lookup -> `GameplayPipeline::DrainCues()` (`AudioOneShot`, `CameraTraumaPulse`, `GamepadRumblePulse`, `SpawnPrefabEffect`, `AppHooks::OnConsumeSimCue`) -> single-call `AudioSystem::Update(AudioUpdateDesc)`.
5. **Render Extraction & Submit (`ExtractRenderFrame`):** `Renderer::BeginFrame(m_window.State().pixelSize)` -> `WorldExtractor::Extract` -> `SceneDirector::DispatchRenderExtract` -> `DebugSystem::EmitOverlays(DebugFrameContext, list)` -> `Renderer::Submit(list)`.

## Client Game Development Guide (By Task Area)

When building client game features on top of EngineN, load only the matching context files from `references/`:

- **Client Bootstrap, Component Registration & AppHooks (`main.cpp`, `GameTypes.h`):** Load `Framework_Context.md`, `World_Context.md`, `Input_Context.md`.
- **State Stack Layers & Scene/Room Streaming (`TitleState`, `GameplayState`, `PauseState`):** Load `Framework_Context.md`, `Assets_Context.md`, `Audio_Context.md`.
- **Player/Enemy FSMs, Movement & Combat Policies (`GameplayPolicyTable`):** Load `Gameplay_Context.md`, `World_Context.md`, `Time_Context.md`, `Input_Context.md`.
- **Custom HUD/UI Rendering, Audio Cues & Debug Overlays/CVars:** Load `Renderer_Context.md`, `Audio_Context.md`, `Debug_Context.md`.

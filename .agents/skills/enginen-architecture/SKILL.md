---
name: enginen-architecture
description: Architect, refactor, and build C++23 client games and subsystems on top of EngineN. Trigger whenever the user mentions EngineN, client game layers, SceneDirector, GameplayPolicyTable, AppHooks, or refactoring any EngineN subsystem (platform, input, time, assets, renderer, world, gameplay, audio, debug, framework).
---
# EngineN Architecture & Refactoring Guide

Provide architectural continuity, exact calling hierarchy enforcement, and phase-by-phase refactoring guidance for EngineN and client games built on top of it without bloating conversation context.

## When to Use

- Building or refactoring a client game on top of EngineN (`AppHooks`, `StateLayerDesc`, `GameplayPolicyTable`, `SceneTransitionDesc`).
- Refactoring, reviewing, or extending any EngineN subsystem (`platform`, `input`, `time`, `assets`, `renderer`, `world`, `gameplay`, `audio`, `debug`, `framework`).
- Starting a fresh chat session for a specific refactoring phase to save context tokens.

## Reference Context Files (Load On-Demand Only)

All 10 subsystem specifications are stored in `references/`. To save context tokens, only load the reference files needed for the active refactoring phase using `skills:get_skill_resources`:

- `references/Platform_Context.md` - `platform/` (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`, 64B `Window`, 4B `PlatformInputEvent`, 256B `PlatformFrameEvents`, single-pass coalesced `Pump`, bitmask gamepad SoA, single-pass `FixedString::Resolve`)
- `references/Input_Context.md` - `input/` (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`, 640B L1-partitioned `InputPipeline`, single-call `IngestPlatformFrame`, dual `m_tickTapMask`/`m_frameTapMask`, 36B `ActionBuffer`, 64B `TickInput` & `FrameInput`)
- `references/Time_Context.md` - `core/time/` (`TimeTypes.h`, `TimePipeline.h`, `SdlClock.h/.cpp`, 16B `ChannelModSlot`, `RequestStep`, `RequestGameplayFreeze`, O(1) `FrozenMask()` & `EffectiveScales()`)
- `references/Renderer_Context.md` - `renderer/` (`RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp`, zero `platform/` header coupling via `ViewportExtent`, 1.26 MB L2/L3-sized `RenderList` (`kMaxQuads = 16384`), Cache Line 0 hot control state, 64B corner-offset `PreparedQuad`, single-pass active-byte radix sort, 16B lazy high-water `TextureSlot`, `BeginFrame(pixelSize)`)
- `references/Assets_Context.md` - `assets/` (`AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp`, single-slab 64B-aligned arenas in both `AssetBank` & `AssetSystem`, zero `platform/` includes, Cache Line 0 hot table layout, 8B embedded-generation `AtlasRecord`, intrusive freelists, `constexpr` O(1) `IsScopeSimReady` via `m_scopeSimPending[32]`, unified single-probe VFS Robin Hood table `VfsTocSlot`, 64B intrusive FIFO `PendingJob` queue, direct-to-arena LZ4 decode, unified `Pin(scope, id, kind, residency)`)
- `references/World_Context.md` - `world/` (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp`, 64B-aligned `AlignedArenaLine[]` slab, Cache Line 0 hot state in `World`, single-array `IsAlive`, 64B 1-cache-line `TransformSolver` with unified `m_dirtyWords` & `m_movedWords` invariant, 32B `ComponentPool<T>` & `PoolOps` with `EmplaceRaw`, zero-lookup `WorldView` fast paths, stationary sprite bypass in `WorldExtractor`)
- `references/Gameplay_Context.md` - `gameplay/` (`GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp`, 64B 1-cache-line `SpatialIndex2D` with 24B `GridBox` & 0B stack-alloc 1D scatter, stage-specialized `RebuildDynamicGrid`, single-pass fused `AnimationSubsystem::StepAnimationsAndDirectives`, O(1) Bloom-deduplicated combat overlaps, preserved carrier ground contacts, drift-free 2nd-order spring `CameraController2D`, 704B L1-partitioned `GameplayPipeline` with sparse `m_activeFlagWordsMask`)
- `references/Audio_Context.md` - `audio/` (`AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp`, zero `world/`/`gameplay/`/`platform/` header includes, 64B 1-cache-line `AudioMixer` over `MixerArenaState`, branchless interior Hermite4 + full-range Cytomic SVF, lazy bus clearing, 128B 2-cache-line `AudioSystem`, 32B `MainVoiceTracker`, `m_spatialActiveMask` occlusion, single-call `Update(AudioUpdateDesc)`)
- `references/Debug_Context.md` - `debug/` (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp`, zero `platform/`/`audio/`/`assets/` Layer 1 header coupling, 132 KB arena, 2-level bitmask `DebugShapePool`, zero-`sqrt` circle rotors, 1-hop `DirectAtEntityIndex` `DebugInspector`, O(1) `PendingPressMask` flight recorder, O(1) incremental `m_symbolHash` console, `DebugLoopDirectives`, `SubsystemTelemetryDesc`)
- `references/Framework_Context.md` - `framework/` (`SceneDirector` SoA state stack & 3-phase streaming, `Application` coordinator)

## Core Engine Invariants

1. **C++23 Data-Oriented Design:** Zero heap allocation after `Init()`, `-fno-exceptions`, `-fno-rtti`, zero virtual dispatch (use function-pointer tables), two-layer separation (Layer 1 Pure C++23 testable via `ComputeAuditHash()`, Layer 2 thin SDL3 adapter with `<SDL3/SDL_*.h>` strictly confined to `.cpp`).
2. **Inversion of Control:** Client code never writes a main loop or calls `World::BeginTick` / `Renderer::Submit`. `en::framework::Application` drives the loop and invokes client callbacks via `AppHooks`, `StateLayerDesc`, `GameplayPolicyTable`, and `SceneTransitionDesc`.
3. **Phase-Scoped Context Access:**
   - **Boot / Scene Transition (`SceneLoadContext&`):** Mutable access to `world`, `gameplay`, `assets`, `audio`, `renderer`, `sceneScope`, `persistentScope`.
   - **120 Hz Simulation (`SimContext&`):** Mutable access to `world`, `gameplay`, `audio`, `platform`, `actionBuffer`; read-only `assetBank`, `tickInput`, `step`. (`Renderer` and `AssetSystem` are omitted for determinism).
   - **Render Extraction (`RenderContext&`):** Read-only `world`, `gameplay`, `assetBank`, `frameInput`, `windowState`, `timeView`; mutable `renderList`. `WorldExtractor::Extract()` already draws world `Sprite2D` entities; client `onRenderExtract` only emits custom UI/HUD quads.
4. **Structural ECS & Physics Rules:**
   - Inside `onSimTick` or policy callbacks, always use `world.DeferDestroy()`, `world.DeferEmplace()`, and `world.DeferRemove()` while iterating views (flushed at `World::EndTick()`).
   - Colliding actors use `gameplay::KinematicMotor2D` (`kTypeId = 16`) + `MotionStack2D` (`kTypeId = 18`). Never attach `world::Velocity2D` (`kTypeId = 1`) to colliding actors.
   - Custom client components must use `kTypeId` in `[24..63]`.
   - Never branch on asset readiness in hot paths; `AssetBank` resolves unready handles branchlessly to Slot 0 fallbacks.
5. **Cross-Subsystem Communication Contracts (Zero Rampant Cross-Calls):**
   - **Time (`core/time/`):** Never loop over `0..7` channels in `Application`, `World`, `GameplayPipeline`, `AudioSystem`, or `DebugSystem` to build `frozenMask` or `channelScales`. `TimePipeline` maintains `BakedTickState` at offset `0` (Cache Line 0) and exposes O(1) pause-aware `m_time.FrozenMask()` (`u8`) and `m_time.EffectiveScales()` (`std::span<const f32, 8>`) that persist automatically across 0-tick high-refresh frames.
   - **Platform (`platform/`):** `platform/` never includes headers from `input/` or higher subsystems. `Platform::Pump()` coalesces all window/display events into a single deferred `Window::RefreshMetrics()` call at the end of the poll loop, coalesces high-frequency (1000Hz–8000Hz) mouse motion, wheel deltas, and consecutive gamepad axis motions directly into a 256B L1-aligned `PlatformFrameEvents` batch, and hands off to `InputPipeline` via a single `input.IngestPlatformFrame(m_frameEvents)` call.
   - **Input (`input/`):** `input/` is a 100% pure C++23 header-only Layer 1 subsystem (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`) with zero `<SDL3/SDL_*.h>` or `platform/` includes. `InputPipeline::IngestPlatformFrame(batch)` ingests the coalesced `PlatformFrameEvents` batch in one call. `ActionBuffer::PendingPressMask()` and `PendingReleaseMask()` expose O(1) `u16` bitmasks so `DebugInspector` and `GameplayPipeline` never loop over 16 actions to inspect buffered intents.
   - **Renderer (`renderer/`):** `RendererTypes.h`, `RenderList.h/.cpp`, and `Renderer.h` have zero `platform/` header includes (`ViewportExtent` implicitly converts from `platform::Extent2i` or `{w, h}`; `"platform/Window.h"` and `<SDL3/SDL_*.h>` are confined strictly to `Renderer.cpp`). `Application::StepFrame()` synchronizes viewport dimensions and resets the frame command list in a single `m_renderer.BeginFrame(m_window.State().pixelSize)` call.
   - **Assets (`assets/`):** `assets/` has zero `platform/` includes (`AssetTypes.h` and `AssetBank.h` depend only on `"core/Result.h"`, `"core/Types.h"`, and Layer 1 `"renderer/RendererTypes.h"`; `<SDL3/SDL_*.h>` and `"renderer/Renderer.h"` are confined strictly to `AssetSystem.cpp`). Both `AssetBank` and `AssetSystem` allocate a single 64B-aligned arena slab at `Init()` (`0` heap allocations during `MountPack*`). `SceneDirector` pins manifest entries (`CookedSceneAssetEntry`) directly via `m_assets.Pin(scope, id, kind, residency)` and queries `m_assets.Bank().IsScopeSimReady(scope)` in `constexpr` $O(1)$.
   - **World (`world/`):** Layer 1 (`WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`) has zero `AssetBank.h` or `RenderList.h` includes. `World::BeginTick(step)` consumes `~step.FrozenMask()` in $O(1)$ and snapshots only entities whose bit in `m_movedWords` is set. `WorldExtractor::Extract()` uses `world.MovedWords()` to bypass `PrevWorldSnapshot2D` and `TimeChannel` loads and lerp math for stationary sprites, and populates Phase 4 `renderer::Camera2D` without the removed `rotationRad` field.
   - **Gameplay (`gameplay/`):** `GameplayTypes.h` includes `"core/time/TimePipeline.h"` (never the removed `"core/time/TimeViews.h"`) and defines `SimCueKind`, `SimCue` (`32B`), and `FastSinCos` in Layer 1. `GameplayPipeline` never calls `AudioSystem`, `Renderer`, or `DebugSystem` directly; it communicates strictly via `DrainCues()` and `World` component pools, uses 1-hop `world.GetComponentMaskByIndex(idx)` + `pool.DirectAtEntityIndex(idx)` instead of repeated `world.TryGet<T>()` lookups, rebuilds a kinematic-only spatial grid (`SpatialRebuildMode::KinematicsOnly`) in Stage 4 before conditional combat rebuild (`SpatialRebuildMode::CombatOnlyIfActive`) in Stage 5, and propagates camera transforms at the end of Stage 6.
   - **Audio (`audio/`):** `audio/` has zero `world/`, `gameplay/`, `platform/`, `renderer/`, or `input/` header includes (`<SDL3/SDL_audio.h>` is strictly confined to `AudioSystem.cpp`). Cross-subsystem data flows through decoupled value types in `AudioTypes.h`: `AudioTriggerReq` (`16B`), `AudioEmitterView` (`16B`, passed to `DebugFrameContext::audioEmitters` via `audio.ExtractActiveEmitters()`), `AudioOcclusionQueryFn` (invoking `SpatialIndex2D::Raycast` on the main thread), `RequestScopeRelease(scope)` / `PollReleasableScopeMask()` (`u32` bitmask of unpinned scopes safe for `AssetSystem::EndScope`), and single-call `AudioSystem::Update(const AudioUpdateDesc&)`.
   - **Debug (`debug/`):** Layer 1 (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`) includes `"core/time/TimePipeline.h"` and `renderer::ViewportExtent` with zero `platform/`, `audio/`, or `assets/AssetBank.h` header includes. `DebugSystem` stores zero subsystem pointers: `BeginFrame(const DebugFrameContext&)` returns an 8-byte `DebugLoopDirectives` value packet (`TogglePause`, `ForcePause`, `stepTicks`, `targetTimeChannel`, `channelTimeScale`) applied by `Application`, `RecordSubsystemTelemetry(const SubsystemTelemetryDesc&)` ingests a decoupled POD telemetry struct, and `DebugInspector` resolves entity components in 1 hop via `world.GetComponentMaskByIndex(idx)` + `pool.DirectAtEntityIndex(idx)` and `actionBuffer.PendingPressMask()` in $O(1)$.

## Canonical Frame Execution Order (`Application::StepFrame`)

1. **OS & Asset Sync:** `Platform::Pump(m_window, m_input, m_devToolsTap)` -> `AssetSystem::Pump(m_renderer)` -> `AudioSystem::PollReleasableScopeMask` (`m_assets.EndScope`) -> `DebugSystem::BeginFrame(DebugFrameContext)` (applies `DebugLoopDirectives` to `TimeManager`)
2. **Safe Frame Barrier (`SceneDirector::CommitSafeFramePoint`):** Advances 3-phase scene streaming (`WaitingManifestBlob` -> `StreamingSceneAssets` -> `ReadyToActivate`). On activation: fires `onDeactivating`, binds `.ecol` / `.epfb` / `.eaud`, retires old `ScopeId` via `AudioSystem::RequestScopeRelease(oldScope)`, resets `TimeManager::OnWindowRefocus` + `ActionBuffer::Clear`, fires `onActivated`. Commits queued state stack changes (`PushState`/`PopState`/`SwitchState`) and rebakes 8-bit dispatch masks.
3. **120 Hz Simulation Loop (`while (step = m_time.PollTick())`):**
   - `InputPipeline::PollTick` -> `ActionBuffer::Feed`
   - `World::BeginTick(step)`
   - `SceneDirector::DispatchSimTick(SimContext&)` -> active `StateLayerDesc::onSimTick`
   - `GameplayPipeline::Tick` (Stage 1: `StepCarriersAndRiders` -> Stage 2: single-pass `StepAnimationsAndDirectives` -> Stage 3: `GameplayPolicyTable` FSM -> Stage 4: `StepAmbientVelocities` + `RebuildDynamicGrid(world, KinematicsOnly)` + `StepMotorsAndMotionStacks` + `PropagateTransforms` -> Stage 5: `RebuildDynamicGrid(world, CombatOnlyIfActive)` + `GatherCombatOverlaps` + `ResolveCombatHits` -> Stage 6: `StepCameraControllers` + `PropagateTransforms`)
   - `World::EndTick(step)` -> `DebugSystem::EndSimTick`
4. **Audio & Cue Drain:** `GameplayPipeline::DrainCues()` -> `AppHooks::OnConsumeSimCue` + `AudioTriggerReq` batch -> single-call `AudioSystem::Update(AudioUpdateDesc{...})`
5. **Render Extraction & Submit:** `Renderer::BeginFrame(m_window.State().pixelSize)` -> `WorldExtractor::Extract` -> `SceneDirector::DispatchRenderExtract` -> `DebugSystem::EmitOverlays(DebugFrameContext, list)` -> `Renderer::Submit()`

## Phased Refactoring Roadmap & Required Files

When the user starts a new chat for a specific refactoring phase, load only the matching reference `.md` files from `references/` and inspect only the user-uploaded `.h/.cpp` files for that phase:

- **Engine Core Subsystems:**
  - **Phase 1 (`core/time/` — LOCKED 4-File Footprint):** `TimeTypes.h`, `TimePipeline.h`, `SdlClock.h/.cpp` (+ `tests/TimeTests.cpp`).
  - **Phase 2 (`platform/` — LOCKED 5-File Footprint):** `PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp` (+ `tests/PlatformTests.cpp`).
  - **Phase 3 (`input/` — LOCKED 3-File Header-Only Footprint):** `InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h` (+ `tests/InputTests.cpp`).
  - **Phase 4 (`renderer/` — LOCKED 5-File Footprint):** `RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp` (+ `tests/RendererTests.cpp`).
  - **Phase 5 (`assets/` — LOCKED 5-File Footprint):** `AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp` (+ `tests/AssetsTests.cpp`).
  - **Phase 6 (`world/` — LOCKED 8-File / 5-Module Footprint):** `WorldTypes.h`, `ComponentPool.h`, `TransformSolver.h/.cpp`, `World.h/.cpp`, `WorldExtractor.h/.cpp` (+ `tests/WorldTests.cpp`).
  - **Phase 7 (`gameplay/` — LOCKED 9-File / 5-Module Footprint):** `GameplayTypes.h`, `SpatialIndex2D.h/.cpp`, `KinematicSolver2D.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `GameplayPipeline.h/.cpp` (+ `tests/GameplayTests.cpp`).
  - **Phase 8 (`audio/` — LOCKED 5-File Footprint):** `AudioTypes.h`, `AudioMixer.h/.cpp`, `AudioSystem.h/.cpp` (+ `tests/AudioTests.cpp`).
  - **Phase 9 (`debug/` — LOCKED 9-File / 5-Module Footprint):** `DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp` (+ `tests/DebugTests.cpp`).
- **Client & Framework Phases:**
  - **Phase 1 (Client Bootstrap, AppHooks & Types):** Load `Framework_Context.md`, `Input_Context.md`, `World_Context.md`. User sends: `main.cpp`, `GameTypes.h`, `Application.h/.cpp` (if modifying hooks).
  - **Phase 2 (State Stack & Scene Streaming):** Load `Framework_Context.md`, `Assets_Context.md`, `Audio_Context.md`. User sends: `SceneDirector.h/.cpp`, client state files (`GameplayState`, `PauseState`, `TitleState`).
  - **Phase 3 (Actor Movement, Combat FSM & Gameplay Policies):** Load `Gameplay_Context.md`, `World_Context.md`, `Time_Context.md`, `Input_Context.md`. User sends: `GamePolicies.h/.cpp`, `GameplayPipeline.h/.cpp`, `StateAndCombatSolver.h/.cpp`, `KinematicSolver2D.h/.cpp`.
  - **Phase 4 (Custom UI/HUD, Cues & Presentation):** Load `Renderer_Context.md`, `Audio_Context.md`, `Debug_Context.md`. User sends: client HUD/render extraction files, cue handlers, or `WorldExtractor.h/.cpp`.

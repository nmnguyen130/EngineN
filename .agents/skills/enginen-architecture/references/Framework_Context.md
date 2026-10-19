# EngineN — Game Framework Subsystem Context (`framework/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Application::Init()`, zero virtual dispatch (`AppHooks` & `StateLayerDesc` function-pointer tables), locked 5-file footprint (`FrameworkTypes.h`, `SceneDirector.h/.cpp`, `Application.h/.cpp`) + `tests/FrameworkTests.cpp`, two-layer pure C++23 separation (`namespace en::framework`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Leaks:**
  - **Layer 1 (`FrameworkTypes.h`, `SceneDirector.h/.cpp`):** Pure C++23 data-oriented Structure-of-Arrays (SoA) state stack machine (`kMaxStateStackDepth = 8`), cooked binary `'ESCN'` scene manifest parser, non-blocking 2-stage scene/room transition pipeline (`WaitingManifestBlob` -> `StreamingSceneAssets`), and `constexpr` 64-bit SplitMix deterministic state audit (`SceneDirector::ComputeAuditHash()`, `SceneDirector::CombineAuditHashes()`). `FrameworkTypes.h` and `SceneDirector.h` include only Layer 1 `*Types.h` headers (`AssetTypes.h`, `TimePipeline.h`, `GameplayTypes.h`, `InputTypes.h`, `ActionBuffer.h`, `Platform.h`, `WorldTypes.h`) and forward-declare Layer 2 subsystem classes — zero `World.h`, `Renderer.h`, `RenderList.h`, `AssetSystem.h`, `AudioSystem.h`, `GameplayPipeline.h`, `DebugSystem.h`, `Window.h`, or `<SDL3/SDL_*.h>` header pollution.
  - **Layer 2 (`Application.h/.cpp`):** 64B-aligned move-only RAII runtime coordinator owning all concrete subsystem instances (`Platform`, `Window`, `InputPipeline`, `ActionBuffer`, `TimeManager`, `Renderer`, `AssetSystem`, `AudioSystem`, `World`, `GameplayPipeline`, `DebugSystem`, `SceneDirector`) in strict dependency order. Drives the canonical 120 Hz fixed-step simulation loop, routes OS window/focus events, drains gameplay `SimCue` records into consolidated `AudioUpdateDesc` / camera trauma / rumble / prefab spawn actions, integrates `DebugSystem` (`DebugLoopDirectives`, `EndSimTick`, `RecordSubsystemTelemetry`, `EmitOverlays`), and orchestrates interpolated render extraction.
- **L1 Cache Line 0 Partitioning & Eliminated Memory Bloat:**
  - **`SceneDirector` (`sizeof == 832`, `alignas(64)`, 13 Cache Lines Exact):**
    - **Cache Line 0 (`offset 0..63`, `64B` exact):** Packs all hot 120 Hz/240 Hz bitmasks (`m_activeSimMask`, `m_activeRenderMask`, `m_pausedClockMask`), `m_stackDepth`, `m_cmdCount`, `m_phase`, `m_persistentScope`, `m_activeScope`, `m_stagingScope`, `m_topInputMask`, `m_stagedManifestBlob`, `m_layerFlags[8]` (`8B`), `m_stateIds[8]` (`16B`), `m_inputMasks[8]` (`16B`), and `m_activeSceneId` (`8B`) with zero padding.
    - **Cache Lines 1..3 (`offset 64..255`, `192B` exact):** Hot SoA dispatch tables (`m_simFns[8]`, `m_renderFns[8]`, `m_userStates[8]`).
    - **Cache Line 4 (`offset 256..319`, `64B` exact):** Cold exit lifecycle table (`m_exitFns[8]`). The write-only dead `m_enterFns[8]` (`64B`) array and `PendingStateCmd` (`15B/cmd` padding) are completely eliminated (`StateLayerDesc::cmdKind` embeds the command tag directly at offset 45).
  - **`Application` Cache Line 0 (`offset 0..63`, `64B` exact) & `-1.26 MB` Footprint Reduction:**
    - Packs all hot runtime coordinator state (`m_userAppState`, `m_devToolsTap`, `m_onConsumeSimCue`, `m_onPreShutdown`, `m_lastRenderListAuditHash`, `m_assetPumpBudget`, `m_frameIndex`, `m_initialized`, `m_headless`) into Cache Line 0 (`64B` exact), eliminating the dead runtime `ApplicationConfig m_cfg` copy, `m_cachedCameraEntity`, `m_latchedFrozenMask`, `m_latchedChannelScales[8]`, and the duplicate `1.26 MB` `m_headlessRenderList` (`StepHeadless()` reuses `m_renderer.BeginFrame()` via shared `ExtractRenderFrame()`).
- **Single-Pass Top-Down Bitmask Rebaking & Re-Entrant State Command Queueing:**
  - `RebakeStackMasks()` sweeps top-down (`i = m_stackDepth` down to `1`) in a single 15-line pass to compute `m_activeSimMask`, `m_activeRenderMask`, `m_pausedClockMask`, and `m_topInputMask` simultaneously without `std::bit_width` shifts or second loops.
  - `PushState`, `PopState`, `SwitchState`, and `ClearAndPushState` delegate to a single 6-line `EnqueueStateCmd` helper, and `CommitSafeFramePoint()` supports re-entrant state/scene commands triggered from inside `onEnter` callbacks.
- **Non-Blocking Scene Streaming, `O(1)` `IsScopeSimReady` & Symmetric Scene Teardown:**
  - Manifest entries (`CookedSceneAssetEntry`) are pinned via unified `assets.Pin(m_stagingScope, entry.id, entry.kind, entry.residency)` (eliminating 5-way `switch (entry.kind)`), and `StepSceneTransition()` checks `bank.IsScopeSimReady(m_stagingScope)` in `constexpr` $O(1)$ (`m_scopeSimPending[scope] == 0`) via a single shared `ensureSimReady` gate.
  - `ActivatePendingScene()` pre-validates staged simulation blobs before invoking `m_activeSceneDeactivateFn` or `world.Clear()`, latches `m_activeSceneDeactivateFn = m_pendingScene.onDeactivating` so the active scene's teardown hook is preserved across subsequent transitions and `Shutdown()`, and retires `oldScope` via single-arg `audio.RequestScopeRelease(oldScope)` (drained in `Application::RouteCuesAndSyncAudio` via `m_audio.PollReleasableScopeMask()` -> `m_assets.EndScope()`).

## 2. Core Types (`framework/FrameworkTypes.h`)
- **Primitives, Enums & Errors (`Zero Padding`):**
  - `using StateId = u16;` (`kInvalidStateId = 0xFFFF`, `kMaxStateStackDepth = 8`, `kMaxPendingStateCmds = 8`).
  - `enum class FrameworkErrc : u8`: `None = 0`, `InvalidConfig`, `PlatformInitFailed`, `WindowCreateFailed`, `RendererInitFailed`, `AssetInitFailed`, `AudioInitFailed`, `WorldInitFailed`, `GameplayInitFailed`, `DebugInitFailed`, `ClientBootFailed`, `StateStackOverflow`, `StateStackUnderflow`, `SceneTransitionBusy`, `ScopeAllocationFailed`, `CorruptedSceneManifest`, `SimAssetWaitFailed`.
  - `enum class SubSystemStage : u8`: `None = 0`, `Platform`, `Window`, `Renderer`, `Assets`, `Audio`, `World`, `Gameplay`, `Debug`, `SceneDirector`, `ClientApp`.
  - `struct alignas(4) FrameworkError` (`sizeof == 4`): `code`, `stage`, `subSystemErrc`, `reserved`.
  - `enum class StateLayerFlags : u8`: `None`, `BlockLowerSimTicks`, `BlockLowerInput`, `BlockLowerRender`, `PauseSimClock`. Queried via `constexpr bool HasStateFlag(StateLayerFlags val, StateLayerFlags mask) noexcept`.
  - `enum class SceneTransitionMode : u8`: `FullReset = 0` (`World::Clear()`), `RoomSwap = 1` (retains surviving entities).
  - `enum class SceneWaitPolicy : u8`: `StreamNonBlocking = 0`, `ImmediateBarrier = 1`.
  - `enum class TransitionPhase : u8`: `Idle = 0`, `WaitingManifestBlob`, `StreamingSceneAssets`.
- **Non-Owning Execution Contexts (`alignas(64)`, Zero Implicit Padding):**
  - `struct alignas(64) SimContext` (`sizeof == 128`): `world`, `gameplay`, `assetBank`, `audio`, `platform`, `actionBuffer`, `tickInput` (`const TickInput*`), `userState` (`void*`), `step` (`SimStepView`), `director` (`SceneDirector*`). Hoisted outside the `PollTick()` loop.
  - `struct alignas(64) RenderContext` (`sizeof == 128`, 0 implicit padding): `world`, `gameplay`, `assetBank`, `renderList`, `frameInput`, `windowState`, `userState` (`void*`), `director` (`SceneDirector*`), `timeView` (`RenderTimeView`).
  - `struct alignas(64) SceneLoadContext` (`sizeof == 64`, 1 cache line): `world`, `gameplay`, `assets`, `audio`, `renderer` (`Renderer*`), `userAppState` (`void*`), `frameIndex`, `sceneScope`, `persistentScope`, `reserved`, `director` (`SceneDirector*`).
- **State, Scene & Determinism Descriptors (`Zero Padding`):**
  - `struct alignas(16) StateLayerDesc` (`sizeof == 48`): `onSimTick`, `onRenderExtract`, `onEnter`, `onExit`, `stateData`, `id`, `inputActionMask`, `flags`, `cmdKind`, `reserved`.
  - `struct alignas(64) SceneTransitionDesc` (`sizeof == 64`): `sceneId`, `manifestBlobId` (`'ESCN'`), `collisionMapId` (`'ECOL'`), `rootPrefabId` (`'EPFB'`), `cueBankId` (`'EAUD'`), `onActivated`, `onDeactivating`, `musicTrackId`, `musicCrossfadeBlocks`, `mode`, `waitPolicy`.
  - `struct alignas(16) CookedSceneManifestHeader` (`sizeof == 48`, magic `'ESCN'`) & `struct alignas(16) CookedSceneAssetEntry` (`sizeof == 16`).
  - `struct alignas(64) FrameAuditDigest` (`sizeof == 64`): `tickIndex`, `worldHash`, `gameplayHash`, `assetBankHash`, `audioHash`, `renderListHash`, `directorHash`, `combinedHash`.
  - `struct alignas(64) AppHooks`: `userAppState`, `devToolsTap`, `gameplayPolicies`, `OnRegisterComponents`, `OnBoot`, `OnPreShutdown`, `OnConsumeSimCue`.
  
## 3. Public Interfaces & Signatures
### `SceneDirector` (`framework/SceneDirector.h` — `sizeof == 832`, `alignas(64)`, Pure C++23 SoA State Stack & Scene Coordinator)
- Lifecycle & Queued State Stack Operations:
  - `[[nodiscard]] Result<void, FrameworkErrc> Init(assets::AssetSystem& assets) noexcept`
  - `void Shutdown(assets::AssetSystem& assets, audio::AudioSystem& audio, SceneLoadContext* optionalLoadCtx = nullptr) noexcept`
  - `[[nodiscard]] constexpr Result<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept`
  - `[[nodiscard]] constexpr Result<void, FrameworkErrc> PopState() noexcept`
  - `[[nodiscard]] constexpr Result<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept`
  - `[[nodiscard]] constexpr Result<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept`
- Scene Streaming, Frame Barrier & Hot Dispatch:
  - `[[nodiscard]] Result<void, FrameworkErrc> RequestSceneTransition(const SceneTransitionDesc& desc, assets::AssetSystem& assets) noexcept`
  - `[[nodiscard]] Result<bool, FrameworkErrc> CommitSafeFramePoint(SceneLoadContext& loadCtx) noexcept`
  - `void DispatchSimTick(SimContext& simCtx) const noexcept`
  - `void DispatchRenderExtract(RenderContext& renderCtx) const noexcept`
- Queries & Determinism Audit (`constexpr` O(1) Cache Line 0):
  - `HasPendingWork()`, `Phase()`, `ActiveSceneScope()`, `PersistentScope()`, `ActiveSceneId()`, `StateDepth()`, `ActiveSimMask()`, `ActiveRenderMask()`, `TopInputMask()`, `IsClockPausedByState()`
  - `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`
  - `[[nodiscard]] static constexpr u64 CombineAuditHashes(u64 tickIndex, u64 worldHash, u64 gameplayHash, u64 assetHash, u64 audioHash, u64 renderHash, u64 directorHash) noexcept`

### `ApplicationConfig` & `Application` (`framework/Application.h` — 64B-Aligned Layer 2 Runtime Coordinator)
- `struct ApplicationConfig` (Layer 2): `appName`, `orgName`, `bootPakPath`, `headless`, `assetPumpBudget`, `window`, `renderer`, `assets`, `audio`, `world`, `gameplay`, `debug`.
- Lifecycle & Execution Entry Points:
  - `[[nodiscard]] Result<void, FrameworkError> Init(const ApplicationConfig& cfg, const AppHooks& hooks = {}) noexcept`
  - `void Shutdown() noexcept`
  - `[[nodiscard]] Result<void, FrameworkError> Run() noexcept`
  - `[[nodiscard]] Result<void, FrameworkError> StepFrame() noexcept`
  - `[[nodiscard]] Result<FrameAuditDigest, FrameworkError> StepHeadless(u32 tickCount = 1, u32 audioBlocksPerTick = 2) noexcept`
  - `[[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept`
- Deduced-`this` Subsystem Accessors:
  - `Director()`, `World()`, `Gameplay()`, `Assets()`, `Audio()`, `Renderer()`, `Platform()`, `Window()`, `Input()`, `Time()`, `ActionBuffer()`, `Debug()`
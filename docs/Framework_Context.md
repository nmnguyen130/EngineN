# EngineN — Game Framework Subsystem Context (`framework/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Application::Init()`, zero virtual dispatch (`AppHooks` & `StateLayerDesc` function-pointer tables), minimal 5-file footprint (`FrameworkTypes.h`, `SceneDirector.h/.cpp`, `Application.h/.cpp`), two-layer pure C++23 separation (`namespace en::framework`).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`FrameworkTypes.h`, `SceneDirector.h/.cpp`):** Pure C++23 data-oriented Structure-of-Arrays (SoA) state stack machine (`kMaxStateStackDepth = 8`), cooked binary `'ESCN'` scene manifest parser, non-blocking 3-phase scene/room transition state machine (`WaitingManifestBlob` $\rightarrow$ `StreamingSceneAssets` $\rightarrow$ `ReadyToActivate`), and 64-bit SplitMix cross-subsystem audit hash combiner (`SceneDirector::ComputeAuditHash()`). Zero OS or `<SDL3/SDL_*.h>` dependencies; 100% deterministic in headless CI.
  - **Layer 2 (`Application.h/.cpp`):** Move-only RAII runtime coordinator owning all concrete subsystem instances (`Platform`, `Window`, `InputPipeline`, `ActionBuffer`, `TimeManager`, `Renderer`, `AssetSystem`, `AudioSystem`, `World`, `GameplayPipeline`, `SceneDirector`) in strict dependency order. Drives the canonical 120 Hz fixed-step simulation loop, routes OS window/focus events, synchronizes lock-free audio snapshots, and orchestrates interpolated render extraction.
- **Hot/Cold SoA State Stack & Pre-Baked Bitmask Intrinsic Dispatch:**
  - `SceneDirector` (`alignas(64)`) separates hot 120 Hz/240 Hz dispatch tables (`m_simFns[8]`, `m_renderFns[8]`, `m_userStates[8]`, `192 B` across Cache Lines 0..2) from cold lifecycle callbacks (`m_enterFns[8]`, `m_exitFns[8]`, `m_cmdQueue[8]`).
  - Whenever state commands commit at `CommitSafeFramePoint()`, `RebakeStackMasks()` computes 8-bit execution masks (`m_activeSimMask`, `m_activeRenderMask`, `m_pausedClockMask`) in $O(1)$ without loops using `std::bit_width(blockBits)` and bitwise masking.
  - Hot-path dispatch (`DispatchSimTick`, `DispatchRenderExtract`) iterates exclusively over active set bits via `for (u32 m = mask; m != 0; m &= (m - 1u))` and `std::countr_zero(m)`, skipping occluded or null-callback layers in hardware with zero branch mispredictions.
- **Non-Blocking 3-Phase Scene Streaming & Safe Frame Mutation Barrier:**
  - Scene transitions allocate a staging `assets::ScopeId` (`m_stagingScope`) and stream cooked `'ESCN'` manifests and `Residency::Sim` payloads (`'ECOL'` collision, `'EPFB'` root prefab, `'EAUD'` cue bank) across frames (`SceneWaitPolicy::StreamNonBlocking`) while the current scene or loading overlay continues rendering at full frame rate.
  - Activation executes strictly at `CommitSafeFramePoint()` *before* `TimeManager::BeginFrame()`, enforcing `AssetSystem::WaitSimReady(m_stagingScope)`, retiring `oldScope` safely through `AudioSystem::RequestScopeRelease(oldScope, m_assets)`, and resetting `TimeManager::OnWindowRefocus()` + `ActionBuffer::Clear()` to prevent post-load catch-up tick spikes.
- **O(1) Cached Camera Listener & Zero-Tick High-Hz Audio Latching:**
  - `Application` caches `m_cachedCameraEntity` (`world::EntityId`) and validates it in $O(1)$ via `World::IsAlive()` + `World::TryGet<CameraRig2D>()`, avoiding linear ECS view scans during `AudioSystem::SetListenerPosition()` updates.
  - `m_latchedFrozenMask` and `m_latchedChannelScales` persist across 0-tick high-refresh render frames (e.g., 240 Hz display running 120 Hz simulation), and automatically freeze gameplay audio channels (`0x3Fu`, keeping `TimeChannel::Ui` bit 6 active) when `TimeManager::IsPaused()` is true.

## 2. Core Types (`framework/FrameworkTypes.h`)
- **Primitives, Enums & Errors (`Zero Padding`):**
  - `using StateId = u16;` (`kInvalidStateId = 0xFFFF`, `kMaxStateStackDepth = 8`, `kMaxPendingStateCmds = 8`).
  - `enum class FrameworkErrc : u8`: `None = 0`, `InvalidConfig`, `PlatformInitFailed`, `WindowCreateFailed`, `RendererInitFailed`, `AssetInitFailed`, `AudioInitFailed`, `WorldInitFailed`, `GameplayInitFailed`, `ClientBootFailed`, `StateStackOverflow`, `StateStackUnderflow`, `SceneTransitionBusy`, `ScopeAllocationFailed`, `CorruptedSceneManifest`, `SimAssetWaitFailed`.
  - `enum class SubSystemStage : u8`: `None = 0`, `Platform`, `Window`, `Renderer`, `Assets`, `Audio`, `World`, `Gameplay`, `SceneDirector`, `ClientApp`.
  - `struct alignas(4) FrameworkError` (`sizeof == 4`): `code` (`FrameworkErrc`), `stage` (`SubSystemStage`), `subSystemErrc` (`u8`), `reserved` (`u8`).
  - `enum class StateLayerFlags : u8`: `None`, `BlockLowerSimTicks`, `BlockLowerInput`, `BlockLowerRender`, `PauseSimClock`. Queried via `constexpr bool HasStateFlag(StateLayerFlags val, StateLayerFlags mask) noexcept`.
  - `enum class SceneTransitionMode : u8`: `FullReset = 0` (`World::Clear()`), `RoomSwap = 1` (retains surviving entities).
  - `enum class SceneWaitPolicy : u8`: `StreamNonBlocking = 0`, `ImmediateBarrier = 1`.
  - `enum class TransitionPhase : u8`: `Idle = 0`, `WaitingManifestBlob`, `StreamingSceneAssets`, `ReadyToActivate`.
- **Non-Owning Execution Contexts (`alignas(64)`):**
  - `struct alignas(64) SimContext`: `world`, `gameplay`, `assetBank`, `audio`, `platform`, `actionBuffer`, `tickInput` (`const TickInput*`), `userState` (`void*`), `step` (`SimStepView`). Hoisted outside the `PollTick()` loop.
  - `struct alignas(64) RenderContext`: `world`, `gameplay`, `assetBank`, `renderList`, `frameInput`, `windowState`, `userState` (`void*`), `timeView` (`RenderTimeView`).
  - `struct alignas(64) SceneLoadContext` (`sizeof == 64`): `world`, `gameplay`, `assets`, `audio`, `renderer` (`Renderer*`), `userAppState` (`void*`), `frameIndex`, `sceneScope`, `persistentScope`.
- **State, Scene & Determinism Descriptors (`Zero Padding`):**
  - `struct alignas(16) StateLayerDesc` (`sizeof == 48`): `onSimTick`, `onRenderExtract`, `onEnter`, `onExit`, `stateData`, `id`, `inputActionMask`, `flags`.
  - `struct alignas(64) SceneTransitionDesc` (`sizeof == 64`): `sceneId`, `manifestBlobId` (`'ESCN'`), `collisionMapId` (`'ECOL'`), `rootPrefabId` (`'EPFB'`), `cueBankId` (`'EAUD'`), `onActivated`, `onDeactivating`, `musicTrackId`, `musicCrossfadeBlocks`, `mode`, `waitPolicy`.
  - `struct alignas(16) CookedSceneManifestHeader` (`sizeof == 48`, magic `'ESCN'`) & `struct alignas(16) CookedSceneAssetEntry` (`sizeof == 16`).
  - `struct alignas(64) FrameAuditDigest` (`sizeof == 64`): `tickIndex`, `worldHash`, `gameplayHash`, `assetBankHash`, `audioHash`, `renderListHash`, `directorHash`, `combinedHash`.
  - `struct alignas(64) AppHooks`: `userAppState`, `devToolsTap`, `gameplayPolicies`, `OnRegisterComponents`, `OnBoot`, `OnPreShutdown`, `OnConsumeSimCue`.

## 3. Public Interfaces & Signatures
### `SceneDirector` (`framework/SceneDirector.h` — Pure C++23 SoA State Stack & Scene Coordinator)
- Lifecycle & Queued State Stack Operations:
  - `[[nodiscard]] std::expected<void, FrameworkErrc> Init(assets::AssetSystem& assets) noexcept`
  - `void Shutdown(assets::AssetSystem& assets, audio::AudioSystem& audio) noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkErrc> PushState(const StateLayerDesc& desc) noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkErrc> PopState() noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkErrc> SwitchState(const StateLayerDesc& desc) noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkErrc> ClearAndPushState(const StateLayerDesc& desc) noexcept`
- Scene Streaming, Frame Barrier & Hot Dispatch:
  - `[[nodiscard]] std::expected<void, FrameworkErrc> RequestSceneTransition(const SceneTransitionDesc& desc, assets::AssetSystem& assets) noexcept`
  - `[[nodiscard]] std::expected<bool, FrameworkErrc> CommitSafeFramePoint(SceneLoadContext& loadCtx, platform::input::InputPipeline& input, core::time::TimeManager& time) noexcept`
  - `void DispatchSimTick(SimContext& simCtx) const noexcept`
  - `void DispatchRenderExtract(RenderContext& renderCtx) const noexcept`
- Queries & Determinism Audit:
  - `[[nodiscard]] constexpr TransitionPhase Phase() const noexcept`, `ActiveSceneScope() const noexcept`, `PersistentScope() const noexcept`, `ActiveSceneId() const noexcept`, `StateDepth() const noexcept`, `IsClockPausedByState() const noexcept`
  - `[[nodiscard]] u64 ComputeAuditHash() const noexcept`
  - `[[nodiscard]] static constexpr u64 CombineAuditHashes(u64 tickIndex, u64 worldHash, u64 gameplayHash, u64 assetHash, u64 audioHash, u64 renderHash, u64 directorHash) noexcept`

### `Application` (`framework/Application.h` — Move-Only RAII Engine Runtime Coordinator)
- Lifecycle & Execution Entry Points:
  - `[[nodiscard]] std::expected<void, FrameworkError> Init(const ApplicationConfig& cfg, const AppHooks& hooks) noexcept`
  - `void Shutdown() noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkError> Run() noexcept`
  - `[[nodiscard]] std::expected<void, FrameworkError> StepFrame() noexcept`
  - `[[nodiscard]] std::expected<FrameAuditDigest, FrameworkError> StepHeadless(u32 tickCount = 1, u32 audioBlocksPerTick = 2) noexcept`
  - `[[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept`
- Deduced-`this` Subsystem Accessors:
  - `Director()`, `World()`, `Gameplay()`, `Assets()`, `Audio()`, `Renderer()`, `Platform()`, `Window()`, `Input()`, `Time()`

## 4. Canonical Main Loop & Client Bootstrap Usage
```cpp
en::framework::Application app{};
const en::framework::ApplicationConfig cfg{
    .appName = "Metroidvania2D",
    .bootPakPath = "base.enpak"
};
const en::framework::AppHooks hooks{
    .userAppState = &gameState,
    .gameplayPolicies = MakeGameplayPolicies(),
    .OnRegisterComponents = &RegisterClientComponents,
    .OnBoot = [](en::framework::SceneLoadContext& ctx, en::framework::SceneDirector& director) noexcept
        -> std::expected<void, en::u8> {
        (void)director.RequestSceneTransition({
            .sceneId = "scenes/room_01"_aid,
            .manifestBlobId = "scenes/room_01.escn"_aid,
            .mode = en::framework::SceneTransitionMode::FullReset,
            .waitPolicy = en::framework::SceneWaitPolicy::ImmediateBarrier
        }, ctx.assets);
        (void)director.PushState({
            .onSimTick = &GameplayState::OnSimTick,
            .onRenderExtract = &GameplayState::OnRenderExtract,
            .stateData = ctx.userAppState,
            .id = 1,
            .inputActionMask = 0xFFFF
        });
        return {};
    }
};

if (auto initRes = app.Init(cfg, hooks)) {
    (void)app.Run(); // Drives StepFrame(): Platform::Pump -> AssetSystem::Pump -> CommitSafeFramePoint -> 120Hz PollTick -> AudioSystem::Update -> WorldExtractor::Extract -> Renderer::Submit
}
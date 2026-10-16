# EngineN — In-Game Debug & Tooling Subsystem Context (`debug/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single `192 KB` 64-byte aligned arena slab), zero virtual dispatch, minimal 5-file footprint (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp`), two-layer pure C++23 separation (`namespace en::debug`).
- **Zero Release Overhead (`EN_ENABLE_DEBUG_TOOLS`):** When `EN_ENABLE_DEBUG_TOOLS == 0` (automatically `0` under `NDEBUG`), `DebugSystem` compiles down to a stateless no-op struct with `constexpr` inline empty methods (`0` bytes runtime allocation, `0` branch overhead).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`):** Pure C++23 unified single-atlas command batcher (`DebugBatcher`), 64-bit word-bitset transient shape pool (`DebugShapePool`), stateless read-only `World` / `Gameplay` / `Audio` spatial introspector & screen-to-world mouse entity picker (`DebugInspector`), 64-tick (`533 ms`) combat/input flight recorder (`ActorTickRecord`), $O(1)$ 64-bucket quantized frame-time percentile profiler (`DebugProfiler`), and Fibonacci open-addressed Command + direct-memory `CVar` console (`DebugConsole`). Zero OS or `<SDL3/SDL_*.h>` dependencies; 100% deterministic in headless CI.
  - **Layer 2 (`DebugSystem.h/.cpp`):** Move-only RAII coordinator owning the `192 KB` arena slab, the unified `128x64` RGBA8 debug atlas texture (`m_debugAtlas`), built-in engine commands, deterministic pre-tick command flush queue, and platform keyboard/text input tap (`OnPlatformKeyEvent`, `OnPlatformTextInput`).
- **Unified Single-Atlas Batching (`0` Radix Sort Passes):**
  - The 96 printable `8x8` ASCII glyphs (`kEmbeddedFont8x8`) and an `8x8` solid opaque white block (`[120..127, 56..63]`, `kSolidWhiteUv`) are unpacked at `Init()` into a single `128x64` texture (`m_debugAtlas`).
  - All world-space debug primitives (`kDebugWorldPassId = 14`, `viewSlot = 0`) and screen-space HUD/Console widgets (`kDebugHudPassId = 15`, `kDebugHudViewSlot = 15`) emit `renderer::QuadCmd` records sharing identical upper 48-bit `SortKey` fields (`Pass`, `Layer = 0`, `Depth = 32768`, `Blend = PremultipliedAlpha`, `View`, `TextureIdx`), allowing `renderer::RenderList::Compile()` to bypass all 6 Radix Sort passes in $O(1)$ and submit each pass in a single draw call.
- **Word-Bitset Transient Shape Pool & Inline Text Payload:**
  - `DebugShapePool` manages `2048` slots via `std::array<u64, 32> m_activeWords` (`std::countr_one` for $O(1)$ allocation, `std::countr_zero` for 64-slot block scanning). Shapes expire deterministically by simulation tick (`expireTick < currentTickLow`) in place with zero memory moves, and embed up to `12` ASCII characters inline inside `DebugShapeCmd` (`32 B`) to eliminate external string buffer fragmentation.
- **Zero Stuck-Key Input Masking & Deterministic Command Flush:**
  - Opening the console (`` ` `` / `SDL_SCANCODE_GRAVE = 53`) saves `input.GetActionMask()` and sets `input.SetActionMask(0)`, cleanly releasing held gameplay actions on the next tick without stuck keys. Interactive commands are queued during OS event polling and flushed strictly inside `DebugSystem::BeginFrame()` prior to `m_time.BeginFrame()`.

## 2. Core Types (`debug/DebugTypes.h` & `debug/DebugProfilerAndConsole.h`)
- **Constants, Error Codes & Bitmasks:**
  - `kDebugWorldPassId = 14`, `kDebugHudPassId = 15`, `kDebugHudViewSlot = 15`, `kDebugFixedDepth = 32768`, `kDebugAtlasWidth = 128`, `kDebugAtlasHeight = 64`, `kSolidWhiteUv`.
  - `enum class DebugErrc : u8`: `InvalidConfig`, `OutOfMemory`, `AtlasCreateFailed`, `SymbolTableFull`, `DuplicateSymbol`, `SymbolNotFound`, `InvalidArguments`, `ValueOutOfRange`.
  - `enum class OverlayFlags : u32`: `None`, `SpatialTransforms`, `KinematicBounds`, `VelocityVectors`, `CombatHitboxes`, `TileCollisionMap`, `DynamicSpatialGrid`, `SafeBreadcrumbs`, `CameraRigOverlay`, `AudioEmitters`, `EntityInspector`, `FlightScrubber`, `ProfilerHud`, `TimeChannelHud`, `InputViewerHud`, `AllWorld`, `AllHud`. Queried via `constexpr bool HasOverlay(OverlayFlags mask, OverlayFlags bit) noexcept`.
  - `enum class DebugShapeKind : u8`: `Line = 0`, `BoxOutline`, `BoxFilled`, `CircleOutline`, `Arrow`, `TextWorld`, `TextScreen`.
  - `enum class ProfileScopeId : u8`: `FrameTotal = 0`, `PlatformPump`, `AssetsPump`, `SimTickTotal`, `GameplaySpatial`, `GameplayKinematics`, `GameplayCombatFsm`, `WorldTransforms`, `AudioUpdate`, `WorldExtract`, `DebugOverlayEmit`, `RenderSubmit`, `Count` (`kProfileScopeCount = 12`).
  - `enum class SymbolKind : u8`: `Empty = 0`, `Command`, `CVarF32`, `CVarI32`, `CVarU32`, `CVarBool`.
  - `enum class LogSeverity : u8`: `Info = 0`, `Success`, `Warning`, `Error`, `Echo`.
- **Cache-Aligned Arena Records (Zero Padding):**
  - `struct alignas(32) DebugShapeCmd` (`sizeof == 32`, 2 shapes per 64B cache line): `x0`, `y0`, `rgba8`, `expireTick`, `kind`, `auxParam`, `thicknessQ8`, and 12B union (`geom { x1, y1, headSize }` or `inlineText[12]`).
  - `struct alignas(64) FrameProfileSample` (`sizeof == 128`, 2 cache lines): `frameIndex`, `simTickIndex`, `scopeUs[12]` (`u16`), `aliveEntities`, `submittedQuads`, `culledQuads`, `drawBatches`, `channelScaleQ8[8]`, `audioCpuPermille`, `audioPeakL_Q10`, `audioPeakR_Q10`, `catchupTicks`, `activePhysVoices`, `activeVirtVoices`, `frozenChannelMask`, `audioUnderruns`, `radixPassesExecuted`.
  - `struct alignas(64) ActorTickRecord` (`sizeof == 64`, 1 cache line): `tickIndex` (`u32`), `entityId`, `inputHeld/Pressed/Released`, `actionBufferMask`, `axisX/Y`, `worldX/Y`, `velX/Y`, `cancelMask`, `stateTicks`, `stateId`, `animClipId`, `animFrame`, `contactFlags`, `activeSliceIdx`, `iframeTicks`, `hp`, `hurtboxQ8[4]`, `hitboxQ8[4]`.
  - `struct alignas(32) ConsoleSymbol` (`sizeof == 32`, 2 symbols per 64B cache line): `nameHash` (32-bit FNV-1a with Fibonacci slot index), `namePtr`, `targetPtr`, 8B union (`cmdFn`, `f32Bounds`, `i32Bounds`), `kind`, `nameLen`.
  - `struct LogEntryMeta` (`sizeof == 8`): `textOffset`, `tickLow16`, `textLength`, `severity`.

## 3. Public Interfaces & Signatures
### `DebugBatcher` & `DebugShapePool` (`debug/DebugBatcher.h`)
- `DebugBatcher`:
  - `void PushSolidRect(renderer::FRect rect, renderer::Color color) noexcept`, `PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept`
  - `void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept`, `PushArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, f32 thickness, renderer::Color color) noexcept`
  - `void PushCircleOutline(f32 cx, f32 cy, f32 radius, u8 segments, f32 thickness, renderer::Color color) noexcept`, `PushText(f32 x, f32 y, std::string_view text, renderer::Color color, f32 scale = 1.0f) noexcept`
  - `static char* FormatI64(char* out, char* end, i64 val) noexcept`, `FormatU64Hex(char* out, char* end, u64 val, u8 digits = 16) noexcept`, `FormatF32Fixed2(char* out, char* end, f32 val) noexcept`
- `DebugShapePool` (`kMaxShapes = 2048`):
  - `void AddLine(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8 = 0xFFFFFFFFu, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept`
  - `void AddBox(f32 cx, f32 cy, f32 halfW, f32 halfH, u32 rgba8 = 0x00FF00FFu, bool filled = false, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept`
  - `void AddArrow(f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba8 = 0xFFFF00FFu, f32 headSize = 4.0f, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept`
  - `void AddCircle(f32 cx, f32 cy, f32 radius, u32 rgba8 = 0x00FFFFFFu, u8 segments = 16, f32 thickness = 1.0f, u32 durationTicks = 1) noexcept`
  - `void AddTextWorld(f32 wx, f32 wy, std::string_view text12, u32 rgba8 = 0xFFFFFFFFu, u32 durationTicks = 1) noexcept`, `AddTextScreen(f32 sx, f32 sy, std::string_view text12, u32 rgba8 = 0xFFFFFFFFu, u32 durationTicks = 1) noexcept`
  - `void EmitAndExpire(u64 currentTickIndex, renderer::TextureHandle debugAtlas, renderer::RenderList& outList) noexcept`, `[[nodiscard]] constexpr u32 ActiveCount() const noexcept`

### `DebugInspector` (`debug/DebugInspector.h` — Read-Only Spatial Extractor, Picker & Flight Scrubber)
- `void RecordActorTick(const world::World& world, u64 tickIndex, const input::TickInput& tickInput, const input::ActionBuffer& actionBuffer) noexcept`
- `world::EntityId PickEntityAtScreenPos(const world::World& world, time::RenderTimeView timeView, input::Vec2 mousePos, platform::Extent2i viewportSize) noexcept`
- `constexpr void SetPinnedEntity(world::EntityId id) noexcept`, `[[nodiscard]] constexpr world::EntityId PinnedEntity() const noexcept`
- `constexpr void StepScrubOffset(i32 deltaTicks) noexcept`, `constexpr void ResetScrubOffset() noexcept`, `[[nodiscard]] constexpr u8 ScrubOffset() const noexcept`
- `void EmitWorldAndInspectorOverlays(OverlayFlags flags, const world::World& world, const gameplay::GameplayPipeline& gameplay, const audio::AudioSystem& audio, const assets::AssetBank& assetBank, time::RenderTimeView timeView, const input::FrameInput& frameInput, platform::Extent2i viewportSize, renderer::TextureHandle debugAtlas, renderer::RenderList& outList) const noexcept`

### `DebugProfiler` & `DebugConsole` (`debug/DebugProfilerAndConsole.h`)
- `DebugProfiler` (`kHistoryFrames = 256`, `kBucketCount = 64`, + `ScopedProfileTimer<ClockT>`):
  - `void BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept`, `void EndFrame() noexcept`
  - `void RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept`
  - `void RecordSubsystemTelemetry(u8 catchupTicks, u16 aliveEntities, u16 submittedQuads, u16 culledQuads, u16 drawBatches, u8 radixPasses, const audio::AudioTelemetrySnapshot& audioSnap, std::span<const f32, 8> channelScales, u8 frozenChannelMask) noexcept`
  - `[[nodiscard]] u32 ComputePercentileUs(u32 targetRankOutOf256) const noexcept`, `[[nodiscard]] constexpr const FrameProfileSample& Latest() const noexcept`
- `DebugConsole` (`kMaxSymbols = 256`, `kMaxLogEntries = 512`, `kLogTextBytes = 16384`):
  - `[[nodiscard]] Result<void, DebugErrc> RegisterCommand(std::string_view staticName, ConsoleCmdCallback fn, void* userCtx = nullptr) noexcept`
  - `[[nodiscard]] Result<void, DebugErrc> RegisterCVarF32(std::string_view staticName, f32* target, f32 minVal, f32 maxVal) noexcept` / `RegisterCVarI32` / `RegisterCVarBool`
  - `void Print(LogSeverity severity, std::string_view msg, u64 tickIndex = 0) noexcept`, `void EnqueueCommandLine(std::string_view line) noexcept`, `void ExecuteImmediate(std::string_view line, u64 currentTickIndex = 0) noexcept`

### `DebugSystem` (`debug/DebugSystem.h` — Move-Only RAII Coordinator)
- `[[nodiscard]] Result<void, DebugErrc> Init(renderer::Renderer& renderer, const DebugSystemConfig& cfg = {}) noexcept`
- `void Shutdown(renderer::Renderer* renderer = nullptr) noexcept`
- `bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl, input::InputPipeline& input) noexcept`, `bool OnPlatformTextInput(std::string_view textUtf8) noexcept`
- `template <time::ClockSource ClockT> void BeginFrame(time::TimePipeline<ClockT>& time, world::World& world, gameplay::GameplayPipeline& gameplay, audio::AudioSystem& audio, const input::FrameInput& frameInput, platform::Extent2i viewportSize) noexcept`
- `void EndSimTick(const world::World& world, time::SimStepView step, const input::TickInput& tickInput, const input::ActionBuffer& actionBuffer) noexcept`
- `void EmitOverlays(const world::World& world, const gameplay::GameplayPipeline& gameplay, const audio::AudioSystem& audio, const assets::AssetBank& assetBank, time::RenderTimeView timeView, const input::FrameInput& frameInput, platform::Extent2i viewportSize, renderer::RenderList& outList) noexcept`
- Accessors: `Shapes()`, `Inspector()`, `Profiler()`, `Console()`, `ActiveOverlays()`, `SetActiveOverlays(OverlayFlags)`.

## 4. Canonical Main Loop Usage
```cpp
const auto winChanges = m_platform.Pump(m_window, m_input, m_debug);
m_audio.OnWindowEvent(winChanges, m_window.State());
m_assets.Pump(m_renderer);
const auto& frameInput = m_input.EndFrame();

m_debug.BeginFrame(m_time, m_world, m_gameplay, m_audio, frameInput, m_window.State().pixelSize);

u8 frozenMask = 0;
std::array<f32, 8> channelScales{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::core::time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);

    m_debug.EndSimTick(m_world, step, tickInput, m_actionBuffer);

    for (u8 ch = 0; ch < 8; ++ch) {
        if (step.IsFrozen(ch)) frozenMask |= static_cast<u8>(1u << ch);
        channelScales[ch] = step.EffectiveScale(ch);
    }
}

m_audio.ConsumeSimCues(m_gameplay.DrainCues(), m_world, m_assets.Bank());
m_audio.Update(m_world, m_gameplay.Spatial(), m_assets.Bank(), m_time.GetRenderView(), frozenMask, channelScales);

en::renderer::RenderList& list = m_renderer.BeginFrame();
en::world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), list);
m_debug.EmitOverlays(m_world, m_gameplay, m_audio, m_assets.Bank(), m_time.GetRenderView(), frameInput, m_window.State().pixelSize, list);
m_renderer.Submit(list);
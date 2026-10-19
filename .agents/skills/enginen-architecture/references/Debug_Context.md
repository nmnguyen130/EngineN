# EngineN — In-Game Debug & Tooling Subsystem Context (`debug/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()` (single right-sized `116 KB` 64-byte aligned arena slab, `kTotalUsedArena == kArenaSizeBytes`), zero virtual dispatch, locked 5-module / 9-file footprint (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp`), two-layer pure C++23 separation (`namespace en::debug`).
- **Zero Release Overhead (`EN_ENABLE_DEBUG_TOOLS`):** When `EN_ENABLE_DEBUG_TOOLS == 0` (automatically `0` under `NDEBUG`), `DebugSystem` and `ScopedProfileTimer<ClockT>` compile down to stateless no-op structs with `constexpr` inline empty methods (`0` bytes runtime allocation, `0` branch overhead).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Leaks (DRY + SOLID):**
  - **Layer 1 (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`):**
    - `DebugTypes.h`: Pure POD types, bitmasks, cache-aligned records, and `UnpackDebugAtlasRGBA8`. Includes `"core/time/TimePipeline.h"` and uses `renderer::ViewportExtent` — zero `platform/`, `audio/`, `assets/`, or `<SDL3/SDL_*.h>` header includes.
    - `DebugBatcher.h/.cpp`: 16-byte immediate-mode primitive & glyph quad emitter (`DebugBatcher`), stack string builder (`DebugTextLine`), and 2-level bitmask transient shape pool (`DebugShapePool`). Depends only on `RenderList` (`0` ECS or Gameplay includes).
    - `DebugInspector.h/.cpp`: 32-byte stateless 1-hop read-only `World` / `Gameplay` spatial introspector (`TryGetDirect<T>`), screen-to-world mouse entity picker, and 64-tick (`533 ms`) combat/input flight recorder (`ActorTickRecord`). `DebugInspector.cpp` does not even include `RenderList.h`.
    - `DebugProfilerAndConsole.h/.cpp`: 192-byte $O(1)$ 64-bucket quantized frame-time percentile profiler (`DebugProfiler`) and Fibonacci open-addressed Command + direct-memory `CVar` console (`DebugConsole`) with zero side-table length arrays (`0` ECS or Gameplay includes).
  - **Layer 2 (`DebugSystem.h/.cpp`):** 64B-aligned move-only RAII coordinator owning the `116 KB` arena slab, the unified `128x64` RGBA8 debug atlas texture (`m_debugAtlas`), built-in engine commands, deterministic pre-tick command flush queue, `DebugLoopDirectives` (`8B`) return packet, and platform keyboard/text input tap (`OnPlatformKeyEvent`, `OnPlatformTextInput`) with automatic `InputPipeline` action-mask save/restore.
- **L1 Cache Line Partitioning, Byte-Tight Structs & Zero Redundant Initialization:**
  - **`DebugSystem` Cache Line 0 (`offset 0..63`, `alignas(64)`):** Packs `m_arenaSlab`, `m_debugAtlas`, `m_overlays`, `m_pendingDirectives` (`8B`), `m_toggleConsoleScancode`, `m_savedActionMask`, and `DebugInspector m_inspector` (`32B`, `alignas(32)`) into the first 64B cache line.
  - **`FrameProfileSample` (`sizeof == 64`, `alignas(64)`, 1 Cache Line Exact):** Shrunk by 50% (from `128B` to `64B` with zero padding) by storing `u32 frameIndex`, `u32 simTickIndex`, `scopeUs[12]` (`24B`), `aliveEntities`, `submittedQuads`, `culledQuads`, `drawBatches`, `channelScaleQ8[8]` (`16B`), `audioCpuPermille`, `catchupTicks`, `activePhysVoices`, `activeVirtVoices`, `frozenChannelMask`, `audioUnderruns`, and `radixPassesExecuted` in a single 64B cache line (cutting the 256-frame profiler ring from `32 KB` to `16 KB`).
  - **`DebugBatcher` (`sizeof == 16`, `alignas(16)`):** Shrunk by 5x (from `80B` to `16B`) by storing only `{m_list, m_atlas, m_passId, m_viewSlot}` and constructing `QuadCmd` in registers via `constexpr MakeQuad(...)`, while `FormatI64` uses standard C++23 `std::to_chars`.
  - **`DebugShapePool` (`sizeof == 320`, `alignas(64)`, 5 Cache Lines Exact):** Cache Line 0 holds `m_pool`, `m_nonEmptyWordMask` (`u32`), `m_nonFullWordMask` (`u32`), `m_currentTickLow`, and `m_activeCount` (`m_allocCursor` eliminated); Cache Lines 1..4 hold `alignas(64) std::array<u64, 32> m_activeWords`. `AllocateSlot` packs active shapes into the lowest available word/bit (`std::countr_zero(m_nonFullWordMask)` + `std::countr_one`) to keep active shapes concentrated in the first cache lines of the pool.
  - **`DebugProfiler` (`sizeof == 192`, `alignas(64)`, 3 Cache Lines Exact):** Cache Line 0 holds `m_samples`, `m_activeBucketMask`, `m_currentIdx`, and `m_validFrames`; Cache Lines 1..2 hold `alignas(64) std::array<u16, 64> m_buckets` (`128B`).
  - **`DebugConsole` (`alignas(64)`):** Cache Line 0 (`64B` exact) holds `m_symbols`, `m_logMeta`, `m_logText`, incremental `m_symbolHash` (`u64`), `m_logWriteIdx`, and `m_logCount`. History and pending `LineBuffer` (`std::array<char, 128>`) entries pack their length in byte `127` (`PackLineBuffer` / `UnpackLineBuffer`), eliminating `m_historyLens[32]` and `m_pendingLens[4]`.

## 2. Core Types (`debug/DebugTypes.h` & `debug/DebugProfilerAndConsole.h`)
- **Constants, Error Codes & Bitmasks:**
  - `kDebugWorldPassId = 14`, `kDebugHudPassId = 15`, `kDebugHudViewSlot = 15`, `kDebugFixedDepth = 32768`, `kDebugAtlasWidth = 128`, `kDebugAtlasHeight = 64`, `kSolidWhiteUv`.
  - `enum class DebugErrc : u8`: `InvalidConfig`, `OutOfMemory`, `AtlasCreateFailed`, `SymbolTableFull`, `DuplicateSymbol`, `SymbolNotFound`, `InvalidArguments`, `ValueOutOfRange`.
  - `enum class OverlayFlags : u32`: `None`, `SpatialTransforms`, `KinematicBounds`, `VelocityVectors`, `CombatHitboxes`, `TileCollisionMap`, `DynamicSpatialGrid`, `SafeBreadcrumbs`, `CameraRigOverlay`, `AudioEmitters`, `EntityInspector`, `FlightScrubber`, `ProfilerHud`, `TimeChannelHud`, `InputViewerHud`, `AllWorld`, `AllHud`. Supports `|`, `&`, `^`, `~`, `|=`, `&=`, `^=`, and `HasOverlay(mask, bit)`.
  - `struct alignas(8) DebugLoopDirectives` (`sizeof == 8`): `channelTimeScale`, `stepTicks`, `targetTimeChannel`, `controlFlags` (`TogglePause()`, `ForcePause()`).
  - `struct alignas(16) DebugAudioEmitter` (`sizeof == 16`): `worldX`, `worldY`, `maxDist`, `occlusionFactor` (layout-compatible with `audio::AudioEmitterView`).
  - `struct DebugFrameContext` (aliased as `DebugBeginFrameContext` and `DebugEmitOverlayContext`): `world`, `timeView`, `frameInput`, `viewportSize` (`renderer::ViewportExtent`), `audioEmitters` (`std::span<const DebugAudioEmitter>`), `isPaused`.
  - `struct SubsystemTelemetryDesc`: `channelScales`, `audioCpuLoadRatio`, `audioPeakL`, `audioPeakR`, `aliveEntities`, `submittedQuads`, `culledQuads`, `drawBatches`, `catchupTicks`, `radixPasses`, `frozenChannelMask`, `activePhysVoices`, `activeVirtVoices`, `audioUnderruns`.
- **Cache-Aligned Arena Records (Zero Padding):**
  - `struct alignas(32) DebugShapeCmd` (`sizeof == 32`, 2 shapes per 64B cache line): `x0`, `y0`, `rgba8`, `expireTick`, `kind`, `auxParam`, `thicknessQ8`, and 12B union (`geom { x1, y1, headSize }` or `inlineText[12]`).
  - `struct alignas(64) FrameProfileSample` (`sizeof == 64`, 1 cache line): `frameIndex` (`u32`), `simTickIndex` (`u32`), `scopeUs[12]` (`u16`), `aliveEntities`, `submittedQuads`, `culledQuads`, `drawBatches`, `channelScaleQ8[8]`, `audioCpuPermille`, `catchupTicks`, `activePhysVoices`, `activeVirtVoices`, `frozenChannelMask`, `audioUnderruns`, `radixPassesExecuted`.
  - `struct alignas(64) ActorTickRecord` (`sizeof == 64`, 1 cache line): `tickIndex` (`u32`), `entityId`, `worldX/Y`, `velX/Y`, `inputHeld/Pressed/Released`, `actionBufferMask` (populated in O(1) via `actionBuffer.PendingPressMask()`), `axisX/Y`, `cancelMask`, `stateTicks`, `hp`, `hurtboxQ8[4]`, `hitboxQ8[4]`, `stateId`, `animClipId`, `animFrame`, `contactFlags`, `activeSliceIdx`, `iframeTicks`.
  - `struct alignas(32) ConsoleSymbol` (`sizeof == 32`, 2 symbols per 64B cache line): `namePtr`, `targetPtr`, 8B union (`cmdFn`, `f32Bounds`, `i32Bounds`, `u32Bounds`), `nameHash`, `kind`, `nameLen`, `Name()`.
  - `struct alignas(8) LogEntryMeta` (`sizeof == 8`): `textOffset`, `tickLow16`, `textLength`, `severity`.

## 3. Public Interfaces & Signatures
### `DebugBatcher` & `DebugShapePool` (`debug/DebugBatcher.h`)
- `DebugBatcher` (`sizeof == 16`, `alignas(16)`):
  - `constexpr DebugBatcher(renderer::RenderList& list, renderer::TextureHandle debugAtlas, u8 passId, u8 viewSlot) noexcept`
  - `void PushSolidRect(renderer::FRect rect, renderer::Color color) noexcept`, `PushBoxOutline(renderer::FRect rect, f32 thickness, renderer::Color color) noexcept`
  - `void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, renderer::Color color) noexcept`, `PushArrow(f32 x0, f32 y0, f32 x1, f32 y1, f32 headSize, f32 thickness, renderer::Color color) noexcept`
  - `void PushCircleOutline(f32 cx, f32 cy, f32 radius, u8 segments, f32 thickness, renderer::Color color) noexcept`, `PushText(f32 x, f32 y, std::string_view text, renderer::Color color, f32 scale = 1.0f) noexcept`
  - `static char* FormatI64(char* out, char* end, i64 val) noexcept`, `FormatU64Hex(char* out, char* end, u64 val, u8 digits = 16) noexcept`, `FormatF32Fixed2(char* out, char* end, f32 val) noexcept`
- `DebugShapePool` (`sizeof == 320`, `alignas(64)`, `kMaxShapes = 2048`):
  - `void AddLine(...) noexcept`, `AddBox(...) noexcept`, `AddArrow(...) noexcept`, `AddCircle(...) noexcept`, `AddTextWorld(...) noexcept`, `AddTextScreen(...) noexcept`
  - `void EmitAndExpire(u64 currentTickIndex, DebugBatcher& worldBatcher, DebugBatcher& hudBatcher) noexcept` (plus inline `TextureHandle, RenderList&` overload)
  - `[[nodiscard]] constexpr u32 ActiveCount() const noexcept`, `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `DebugInspector` (`debug/DebugInspector.h` — `sizeof == 32`, `alignas(32)`, Read-Only 1-Hop Spatial Extractor, Picker & Flight Scrubber)
- `void RecordActorTick(const world::World& world, u64 tickIndex, const input::TickInput& tickInput, const input::ActionBuffer& actionBuffer) noexcept`
- `world::EntityId PickEntityAtScreenPos(const world::World& world, input::Vec2 mousePos, renderer::ViewportExtent viewportSize) noexcept` (plus inline 4-arg overload accepting `const time::RenderTimeView&`)
- `constexpr void SetPinnedEntity(world::EntityId id) noexcept`, `[[nodiscard]] constexpr world::EntityId PinnedEntity() const noexcept`
- `constexpr void StepScrubOffset(i32 deltaTicks) noexcept`, `constexpr void ResetScrubOffset() noexcept`, `[[nodiscard]] constexpr u8 ScrubOffset() const noexcept`
- `void EmitWorldAndInspectorOverlays(OverlayFlags flags, const DebugEmitOverlayContext& ctx, DebugBatcher& worldBatcher, DebugBatcher& hudBatcher) const noexcept` (plus inline `TextureHandle, RenderList&` overload)
- `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `DebugProfiler` & `DebugConsole` (`debug/DebugProfilerAndConsole.h`)
- `DebugProfiler` (`sizeof == 192`, `alignas(64)`, `kHistoryFrames = 256`, `kBucketCount = 64`, + `ScopedProfileTimer<ClockT>`):
  - `void BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept`, `void EndFrame() noexcept`
  - `void RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept`
  - `void RecordSubsystemTelemetry(const SubsystemTelemetryDesc& desc) noexcept`
  - `[[nodiscard]] u32 ComputePercentileUs(u32 targetRankOutOf256) const noexcept`, `[[nodiscard]] const FrameProfileSample& Latest() const noexcept`
  - `void EmitProfilerAndTimeHud(OverlayFlags flags, renderer::ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept`
- `DebugConsole` (`alignas(64)`, `kMaxSymbols = 256`, `kMaxLogEntries = 512`, `kLogTextBytes = 16384`):
  - `RegisterCommand`, `RegisterCVarF32`, `RegisterCVarI32`, `RegisterCVarU32`, `RegisterCVarBool`
  - `void Print(LogSeverity severity, std::string_view msg, u64 tickIndex = 0) noexcept`, `void EnqueueCommandLine(std::string_view line) noexcept`, `void FlushPendingCommands(u64 currentTickIndex) noexcept`, `void ExecuteImmediate(std::string_view line, u64 currentTickIndex = 0) noexcept`
  - `void EmitConsoleOverlay(renderer::ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept`, `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `DebugSystem` (`debug/DebugSystem.h` — 64B-Aligned Move-Only RAII Coordinator, `kArenaSizeBytes = 116 * 1024`)
- `[[nodiscard]] Result<void, DebugErrc> Init(renderer::Renderer& renderer, const DebugSystemConfig& cfg = {}) noexcept`
- `void Shutdown(renderer::Renderer* renderer = nullptr) noexcept`
- `[[nodiscard]] constexpr platform::EventCapture GetEventCapture() const noexcept`
- `bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl) noexcept`, `bool OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl, input::InputPipeline& input) noexcept`, `bool OnPlatformTextInput(std::string_view textUtf8) noexcept`
- `[[nodiscard]] DebugLoopDirectives BeginFrame(const DebugBeginFrameContext& ctx) noexcept`
- `void EndSimTick(const world::World& world, time::SimStepView step, const input::TickInput& tickInput, const input::ActionBuffer& actionBuffer) noexcept`
- `void EmitOverlays(const DebugEmitOverlayContext& ctx, renderer::RenderList& outList) noexcept`
- Deduced-`this` Accessors: `Shapes()`, `Inspector()`, `Profiler()`, `Console()`, `ActiveOverlays()`, `SetActiveOverlays(OverlayFlags)`.
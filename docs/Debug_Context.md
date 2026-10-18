# EngineN — Debug Subsystem Context (`engine/debug/`)

## 1. Architecture & Invariants
- **Core:** C++23 (`-fno-exceptions`, `-fno-rtti`), zero heap allocation after `Init()` (single 192 KB 64B-aligned arena slab), zero virtual dispatch, 5-component modular design (`DebugTypes.h`, `DebugBatcher.h/.cpp`, `DebugInspector.h/.cpp`, `DebugProfilerAndConsole.h/.cpp`, `DebugSystem.h/.cpp`).
- **Zero Release Overhead (`EN_ENABLE_DEBUG_TOOLS`):** When `EN_ENABLE_DEBUG_TOOLS == 0` (default under `NDEBUG`), `DebugSystem` compiles down to a stateless no-op struct with inline `constexpr` empty methods (0 bytes RAM, 0 CPU cycles).
- **Two-Layer Decoupled Design:**
  - **Layer 1 (Introspectors & Batchers):** `DebugBatcher` (shared 128x64 atlas font + white box, 0 radix passes), `DebugShapePool` (2048 transient shapes, 64-bit word bitsets), `DebugInspector` (read-only entity picker, 64-tick flight recorder), `DebugProfiler` (64-bucket percentile frame profiler), `DebugConsole` (Robin Hood symbol hash, CVars). Zero OS/SDL dependencies.
  - **Layer 2 (`DebugSystem`):** Move-only RAII coordinator owning the 192 KB arena, debug atlas texture, console input tap, and returning 8-byte `DebugLoopDirectives` to control outer simulation pause/stepping.
- **Unified 128x64 Atlas & 0 Radix Passes:** Embedded 8x8 ASCII font (`kEmbeddedFont8x8`) and 8x8 solid white block (`kSolidWhiteUv`) share one texture (`m_debugAtlas`). World debug (`kDebugWorldPassId = 14`, `view = 0`) and HUD (`kDebugHudPassId = 15`, `view = 15`) bypass all 6 radix passes and submit in single batches.
- **Console Capture & Zero Stuck-Key Guarantee:** When console is open, `GetEventCapture()` returns `EventCapture::Keyboard | Mouse` to `Platform::Pump`'s `EventTap`. Release events still pass through, preventing stuck keys.

## 2. Core Layouts & Types (`DebugTypes.h`)
- Constants: `kDebugWorldPassId = 14`, `kDebugHudPassId = 15`, `kDebugHudViewSlot = 15`, `kDebugFixedDepth = 32768`, `kSolidWhiteUv`.
- `DebugErrc`: `InvalidConfig`, `OutOfMemory`, `AtlasCreateFailed`, `SymbolTableFull`, `DuplicateSymbol`, `SymbolNotFound`, `InvalidArguments`, `ValueOutOfRange`.
- `OverlayFlags` (32-bit bitmask): `None`, `SpatialTransforms`, `KinematicBounds`, `VelocityVectors`, `CombatHitboxes`, `TileCollisionMap`, `DynamicSpatialGrid`, `SafeBreadcrumbs`, `CameraRigOverlay`, `AudioEmitters`, `EntityInspector`, `FlightScrubber`, `ProfilerHud`, `TimeChannelHud`, `InputViewerHud`, `AllWorld`, `AllHud`.
- `DebugShapeCmd` (`alignas(32)`, 32B): `{x0, y0, rgba8, expireTick, kind, auxParam, thicknessQ8, union { geom{x1,y1,headSize}, inlineText[12] }}`.
- `FrameProfileSample` (`alignas(64)`, 128B): `frameIndex`, `simTickIndex`, `scopeUs[12]`, telemetry stats, audio peaks/load.
- `ActorTickRecord` (`alignas(64)`, 64B): Per-tick flight recording for actor inputs, motors, FSM, and hurtbox/hitbox bounds.
- `DebugLoopDirectives` (8B POD): `{channelTimeScale, stepTicks, targetTimeChannel, controlFlags}` (`TogglePause()`, `ForcePause()`).
- `DebugFrameContext`: `{const World& world, RenderTimeView timeView, const FrameInput& frameInput, Extent2i viewportSize, bool isPaused}`.

## 3. Subsystem APIs
### `DebugShapePool` & `DebugBatcher` (`engine/debug/DebugBatcher.h`)
- `AddLine`, `AddBox`, `AddArrow`, `AddCircle`, `AddTextWorld`, `AddTextScreen` (transient shapes expired by simulation tick).
- `EmitAndExpire(currentTick, debugAtlas, renderList)`: Emits accumulated shapes into `RenderList`.

### `DebugInspector` (`engine/debug/DebugInspector.h`)
- `RecordActorTick(world, tickIndex, tickInput, actionBuffer)`: Records 64-tick ring buffer.
- `PickEntityAtScreenPos(world, timeView, mousePos, viewportSize)` $\to$ `EntityId`
- `SetPinnedEntity(id)`, `PinnedEntity()`, `StepScrubOffset(delta)`, `ResetScrubOffset()`.

### `DebugProfiler` & `DebugConsole` (`engine/debug/DebugProfilerAndConsole.h`)
- `DebugProfiler`: `BeginFrame(frameIdx, tickIdx)`, `EndFrame()`, `RecordScopeUs(scope, us)`, `RecordSubsystemTelemetry(SubsystemTelemetryDesc)`, `ComputePercentileUs(rank)`.
- `DebugConsole`: `RegisterCommand(name, fn, ctx)`, `RegisterCVarF32 / I32 / Bool`, `Print(severity, msg)`, `EnqueueCommandLine(line)`.

### `DebugSystem` (`engine/debug/DebugSystem.h` — Coordinator)
- **Lifecycle & Configuration:**
  - `Init(Renderer&, DebugSystemConfig)` $\to$ `Result<void, DebugErrc>`, `Shutdown(Renderer*)`
- **Platform Input & Directives:**
  - `GetEventCapture()` $\to$ `EventCapture`: Captures keyboard/mouse when console is open.
  - `OnPlatformKeyEvent(scancode, down, shift, ctrl)` $\to$ `bool`, `OnPlatformTextInput(textUtf8)` $\to$ `bool`
  - `BeginFrame(DebugBeginFrameContext)` $\to$ `DebugLoopDirectives`: Flushes console commands, updates profiler, returns loop directives.
  - `EndSimTick(world, step, tickInput, actionBuffer)`: Records flight data for pinned actor.
  - `EmitOverlays(DebugEmitOverlayContext, RenderList&)`: Emits world overlays, HUD, and console.
  - `Shapes()`, `Inspector()`, `Profiler()`, `Console()`, `ActiveOverlays()`, `SetActiveOverlays()`.

## 4. Main Loop Integration
```cpp
// 1. OS & Platform pump with Debug EventTap
const auto winChanges = m_platform.Pump(m_window, m_input, platform::EventTap::Bind<&DebugSystem::GetEventCapture>(m_debug));

// 2. Begin debug frame and apply loop directives (pause/step/timescale)
const auto directives = m_debug.BeginFrame({
    .world = m_world, .timeView = m_time.GetRenderView(),
    .frameInput = m_input.GetFrameView(), .viewportSize = m_window.State().pixelSize,
    .isPaused = m_time.IsPaused()
});
if (directives.TogglePause()) m_time.TogglePause();

// 3. 120Hz deterministic simulation loop
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);

    m_debug.EndSimTick(m_world, step, tickInput, m_actionBuffer);
}

// 4. Render extraction & debug overlay pass
auto& renderList = m_renderer.BeginFrame();
world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), renderList);
m_debug.EmitOverlays({.world = m_world, .timeView = m_time.GetRenderView(),
                      .frameInput = m_input.GetFrameView(), .viewportSize = m_window.State().pixelSize}, renderList);
m_renderer.Submit(renderList);
```
# EngineN Phase 11: `debug/` (v2) Subsystem Architecture & Reference

## 1. Zero-Coupling Boundary & Zero Release Overhead
- **Zero SDL/OS/Renderer Header Leaks:** `debug/` uses `kWhiteTexture{0, 1}` and `kBuiltinFontTexture{1, 1}` from `"renderer/RenderList.h"`, eliminating duplicate bitmap font tables and requiring zero `Renderer.h` or `<SDL3/SDL_*.h>` includes anywhere in `debug/`.
- **Zero Release Overhead (`EN_ENABLE_DEBUG_TOOLS == 0`):** Under `-DEN_ENABLE_DEBUG_TOOLS=0` (or `NDEBUG`), `DebugSystem` and `ScopedProfileTimer<ClockT>` compile to `constexpr` inline no-ops (`0` arena bytes allocated, `0` runtime branches).
- **Zero Type Aliases & Zero Hidden Padding:** `0` `using` type aliases across `debug/*.h` and `debug/*.cpp`; `0` dummy `reserved` padding fields; `0B` hidden compiler padding verified via `static_assert`.

## 2. Exact Struct Layouts (`debug/DebugTypes.h`, `debug/DebugBatcher.h`, `debug/DebugInspector.h`, `debug/DebugProfilerAndConsole.h`, `debug/DebugSystem.h`)
- `DebugLoopDirectives` (`8B`, `alignas(8)`): `f32 channelTimeScale{-1.0f}`, `u16 stepTicks{0}`, `u8 targetTimeChannel{0}`, `u8 controlFlags{0}` (`TogglePause()`, `ForcePause()`, `ResetRequested()`).
- `LogEntryMeta` (`8B`, `alignas(8)`): `u32 textOffset`, `u16 tickLow16`, `u8 textLength`, `LogSeverity severity`.
- `DebugBatcher` (`16B`, `alignas(16)`): `RenderList* m_list`, `TextureHandle m_fontTex`, `u16 m_depth`, `u8 m_passId`, `u8 m_viewSlot`.
- `DebugInspector` (`16B`, `alignas(16)`): `ActorTickRecord* m_history`, `EntityId m_pinnedEntity`, `u16 m_writeIdx`, `u8 m_validCount`, `u8 m_scrubOffset`.
- `DebugShapeCmd` (`32B`, `alignas(32)`): `f32 x0`, `f32 y0`, `12B` `payload` union (`geom{x1, y1, auxFloat}` or `text[12]`), `u32 rgba8`, `u32 expireTick`, `u16 thicknessQ8`, `DebugShapeKind kind`, `u8 auxByte`.
- `ConsoleSymbol` (`32B`, `alignas(32)`) & `SubsystemTelemetryDesc` (`32B`, `alignas(32)`).
- `FrameProfileSample` (`64B`, `alignas(64)`, 1 cache line exact) & `ActorTickRecord` (`64B`, `alignas(64)`, 1 cache line exact) & `DebugFrameContext` (`64B`, `alignas(64)`, 1 cache line exact).
- `DebugSystem` (`64B`, `alignas(64)`, **1 cache line exact**): `AlignedArena m_arena` (`32B`), `DebugInspector m_inspector` (`16B`), `DebugArenaState* m_state` (`8B`), `DebugLoopDirectives m_pendingDirectives` (`8B`).
- `DebugProfiler` (`160B`, `alignas(32)`): `u16 m_buckets[64]` (`128B`), `FrameProfileSample* m_samples`, `WatchPlotTrack* m_plots`, `u64 m_activeBucketMask`, `u16 m_currentIdx`, `u16 m_validFrames`, `u16 m_spikeCount`, `u16 m_spikeThresholdUs`.
- `WatchPlotTrack` (`288B`, `alignas(32)`) & `DebugShapePool` (`288B`, `alignas(32)`).
- `DebugConsole` (`2752B`, `alignas(64)`, 43 cache lines exact).

## 3. Core 2026 Mechanisms (`debug/` v2)
- **State-Colored Physics Colliders, `PhysicsJoints` (`DistanceJoint2D`), & `SpatialBroadphase`:** Color-codes colliders by `Static`, `Kinematic`, `Dynamic Awake`, `Sleeping`, `Sensor`, and `OneWay`; visualizes `DistanceJoint2D` springs/rods, broadphase AABB proxies, and solid/one-way tilemap cells.
- **2D Arcade Stick Gate & Input Button LED Viewer (`OverlayFlags::InputViewerHud`):** Renders live stick position inside a 2D gate box alongside 8 action button LEDs (`pressed`, `held`, `buffered` from `ActorTickRecord`).
- **Real-Time Variable Watch & Sparkline Plotter (`DebugProfiler::PlotValue` & `OverlayFlags::WatchPlotHud`):** Tracks up to 4 named 64-sample rolling time-series (`WatchPlotTrack`) with auto-ranged `[minVal, maxVal]` waveform rendering.
- **Console Commands & History Navigation (`DebugConsole`):** Built-in `help`, `clear` (`ClearLogs()`), `cvarlist` (`ListAllCVars()`), `reset` (`RequestStateReset()`), `pause`, `step [N]`, `timescale <s [ch]>`, `overlay <name>`, and `inspect <id>`.
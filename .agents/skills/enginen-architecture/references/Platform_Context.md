# EngineN — Platform Subsystem Context (`platform/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()`, 5 production files (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`) + `tests/PlatformTests.cpp`, two-layer pure C++23 separation (`namespace en`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Includes:**
  - **Layer 1 (`PlatformTypes.h`):** 100% `constexpr` C++23 geometry/window types (`Extent2i` `8B`, `WindowMode`, `GraphicsApi`, `ScaleMode`, `CursorShape`, `WindowFlashOp`, `PowerSupplyState`, `PowerInfo` `4B`, `WindowFlags`, `WindowConfig` `40B`, `WindowState` `32B` with high-DPI coordinate transforms, 2D virtual canvas viewport scaler `ComputeViewport` / `PixelToVirtual` / `VirtualToPixel`, `ApplyMetrics`, and mutually-exclusive net `ApplyLifecycleEvent`), branchless 16-byte `EventTap` delegate (`FixedMaskThunk` non-null invariant), 4-byte packed discrete input event (`PlatformInputEvent`), 256-byte coalesced frame batch (`PlatformFrameEvents` with `constexpr` key-repeat filter, duplicate button-edge suppression, anti-sticking capture filter, immediate high-DPI 8000 Hz mouse scaling, UTF-8 `textInput[32]`, `INT16_MIN` axis clamping, interleaved multi-axis coalescing, and mid-frame gamepad disconnect purge), `FilePath = FixedString<256>` (`256B` from `core/Types.h`), and 64-bit SplitMix deterministic state audit (`WindowState::ComputeAuditHash()`, `PlatformFrameEvents::ComputeAuditHash()`). Depends solely on `"core/Math.h"` — zero OS, `<SDL3/SDL_*.h>`, `<filesystem>`, or `input/` dependencies.
  - **Layer 2 (`Window.h/.cpp`, `Platform.h/.cpp`):** Move-only RAII `Window` (`sizeof == 64`, `alignas(64)`) and single-instance `Platform` coordinator (`sizeof == 1856`, `alignas(64)`, 29 cache lines exact, 0B padding). `<SDL3/SDL.h>` is strictly confined to `Window.cpp` and `Platform.cpp` (with automatic `#if __has_include(<SDL3/SDL.h>)` fallback for standalone headless CI builds). `platform/` never includes headers from `input/` or higher engine layers.
- **2D Virtual Canvas & Aspect-Ratio Viewport Scaler (`ScaleMode`):**
  - `WindowState::ComputeViewport(Extent2i virtualSize, ScaleMode)` computes the exact destination viewport `Rect` in physical backbuffer pixels for `Stretch`, `Letterbox` (pillarbox/letterbox bars on 21:9 or 16:10 displays), `IntegerScale` (pixel-perfect integer multiples for 2D pixel art with automatic `Letterbox` fallback below `1x`), and `Expand` (aspect-preserving overscan).
  - `WindowState::PixelToVirtual` and `VirtualToPixel` unproject/project physical backbuffer pixel coordinates (`mouseX/Y`) to/from 2D virtual canvas coordinates with optional clamping to the active canvas bounds.
- **Deferred Single-Pass Window Metric Synchronization & Live Modal Resize Watch:**
  - `Platform::Pump()` tracks mutually-exclusive net `WindowFlags` via `window.ApplyLifecycleEvent()` and invokes `window.RefreshMetrics()` **at most once per frame** in `FinalizePump()` after the `SDL_PollEvent` loop finishes.
  - `Platform::SetLiveResizeWatch(Window*)` registers an `SDL_AddEventWatch` hook (`OnModalResizeEventWatch`) so `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` and `SDL_EVENT_WINDOW_EXPOSED` refresh window metrics immediately inside Windows/macOS modal border-drag resize loops.
- **High-Frequency Input Coalescing & UTF-8 Text Stream (8000 Hz Mouse & 1000 Hz Hall-Effect Stick Ready):**
  - Mouse motion and button coordinates are scaled immediately on ingestion by `pixelDensity` so `mouseX/Y` and `mouseDx/Dy` are **always in physical pixel space** (eliminating `hasMouseMotion` and deferred scaling bugs).
  - `GamepadAxis` values clamp `-32768` (`INT16_MIN`) to `-32767` and coalesce across **interleaved multi-axis samples** (e.g. alternating `Axis 0 (X)` and `Axis 1 (Y)` at 1000 Hz) in-place so each `(padSlot, axis)` occupies at most 1 slot per frame.
  - Discrete button/key transitions suppress redundant duplicate edges (`Down` followed by `Down` without an intervening `Up`) while preserving `Down -> Up` pairs for sub-frame tap latches.
  - `IngestTextInput` accumulates up to 31 UTF-8 bytes per frame in `PlatformFrameEvents::textInput` (`TextView()`) when `window.SetTextInput(true)` is active.
- **L1 Cache Line Layout & Bitmask Dispatch (Zero Padding):**
  - `Window` (`alignas(64)`, `sizeof == 64`, 0B padding): Places `WindowState m_state` (`32B`) at offset `0`, `SDL_Window*`, DPI callback/context, `m_windowId`, `m_dpiMigrationCount`, `CursorShape m_cursorShape` (`1B`), and packed `m_optionBits` (`1B`: `cursorVisible`, `textInputActive`, `aspectLocked`).
  - `Platform` (`alignas(64)`, `sizeof == 1856`, 29 cache lines exact, 0B padding):
    - **Cache Line 0 (`offset 0..63`, `64B` exact):** `m_gamepadHandles[4]` (`32B`), `m_gamepadIds[4]` (`16B`), `m_watchedWindow` (`8B`), `m_activeGamepadMask` (`1B`), `m_rumbleGamepadMask` (`1B`), `m_droppedFileCount` (`1B`), `m_initialized` (`1B`), `m_quit` (`1B`), `m_headless` (`1B`), `m_watchRegistered` (`1B`), `m_screenSleepDisabled` (`1B`).
    - **Cache Lines 1..4 (`offset 64..319`, `256B` exact):** `PlatformFrameEvents m_frameEvents` (`alignas(64)`).
    - **Cache Lines 5..28 (`offset 320..1855`, `1536B` exact):** `m_basePath` (`256B`), `m_prefPath` (`256B`), `m_droppedFiles[4]` (`1024B`).
  - `FileWatcher` (`kMaxWatched = 32`): Tracks occupied slots via `u32 m_activeMask` (`std::countr_zero`, `std::countr_one`, `std::popcount`) over 272-byte zero-padding `WatchSlot` records (`0` loop iterations in `PollModified` when idle).

## 2. Core Types (`platform/PlatformTypes.h`)
- **Primitives, Enums, Power & Paths (`Zero Padding`):**
  - `using FilePath = FixedString<256>;` (`sizeof == 256`, `alignof == 2`).
  - `enum class PlatformErrc : u8`: `None = 0`, `SdlInitFailed`, `WindowCreateFailed`, `FullscreenFailed`, `PathRejected`, `FileIoFailed`, `ClipboardFailed`, `InvalidState`.
  - `struct alignas(8) Extent2i` (`sizeof == 8`): `w`, `h`, `IsValid()`, `AspectRatio()`.
  - `enum class WindowMode : u8`: `Windowed`, `Borderless`, `ExclusiveFullscreen`.
  - `enum class GraphicsApi : u8`: `Auto`, `OpenGL`, `Vulkan`, `Metal`.
  - `enum class ScaleMode : u8`: `Stretch`, `Letterbox`, `IntegerScale`, `Expand`.
  - `enum class CursorShape : u8`: `Arrow`, `IBeam`, `Crosshair`, `Hand`, `ResizeEW`, `ResizeNS`, `NotAllowed`.
  - `enum class WindowFlashOp : u8`: `Cancel`, `Briefly`, `UntilFocused`.
  - `enum class PowerSupplyState : u8`: `Unknown`, `OnBattery`, `NoBattery`, `Charging`, `Charged`.
  - `struct alignas(4) PowerInfo` (`sizeof == 4`): `remainingSeconds`, `batteryPercent`, `state`, `IsLowBattery(thresholdPercent = 20)`.
- **Window Configuration, Flags & State (`Zero Padding`):**
  - `enum class WindowFlags : u8`: `None`, `Resized`, `DisplayChanged`, `FocusGained`, `FocusLost`, `Minimized`, `Restored`, `CloseRequested`.
  - `struct alignas(8) WindowConfig` (`sizeof == 40`): `title`, `size{1280, 720}`, `minSize{640, 360}`, `targetRefreshHz{0.0f}`, `mode`, `api`, `resizable{true}`, `highDpi{true}`.
  - `struct alignas(32) WindowState` (`sizeof == 32`): `logicalSize`, `pixelSize`, `pixelDensity{1.0f}`, `displayScale{1.0f}`, `refreshHz{60.0f}`, `mode`, `focused{true}`, `suspended{false}`, `relativeMouse{false}`.
    - Methods: `IsPresentable()`, `AspectRatio()`, `WindowToPixel(Vec2)`, `PixelToWindow(Vec2)`, `PixelToUiLogical(Vec2)`, `UiLogicalToPixel(Vec2)`, `ComputeViewport(Extent2i, ScaleMode)`, `PixelToVirtual(...)`, `VirtualToPixel(...)`, `ApplyMetrics(...)`, `ApplyLifecycleEvent(...)`, `ComputeAuditHash()`.
- **Coalesced Input, Text & Event Capture Types (`Zero Padding`):**
  - `enum class EventCapture : u8`: `None`, `Keyboard`, `Mouse`, `Gamepad`, `All`.
  - `struct alignas(16) EventTap` (`sizeof == 16`): Branchless delegate (`EventTap::Bind<&T::Method>(instance)` or implicit `constexpr` construction from `EventCapture`), `Evaluate()`.
  - `enum class InputEventKind : u8`: `Key = 0`, `MouseButton = 1`, `GamepadButton = 2`, `GamepadAxis = 3`.
  - `struct alignas(4) PlatformInputEvent` (`sizeof == 4`): `packedHeader` (`u16`), `value` (`i16`), `Make()`, `Code()`, `PadSlot()`, `Kind()`, `Down()`.
  - `struct alignas(64) PlatformFrameEvents` (`sizeof == 256`, `kMaxTextBytes = 31`, `kMaxEvents = 49`): `ClearFrameDeltas()`, `ClearInput(bool)`, `PushEvent(...)`, `IngestKey(...)`, `IngestMouseButton(...)`, `IngestMouseMotion(...)`, `IngestMouseWheel(...)`, `IngestTextInput(...)`, `IngestGamepadButton(...)`, `IngestGamepadAxis(...)`, `IngestGamepadDisconnect(u8)`, `Events()`, `TextView()`, `HasMouseWheel()`, `ComputeAuditHash()`.

## 3. Public Interfaces & Signatures
### `Window` (`platform/Window.h` — `sizeof == 64`, `alignas(64)`)
- `Create(cfg, headless)`, `Destroy()`, `IsValid()`, `Native()`, `Id()`, `State()`
- `SetMode(mode, targetRefreshHz)`, `SetTitle(title)`, `SetRelativeMouse(enabled)`, `SetAspectRatioLock(minAspect, maxAspect)`
- `SetCursorVisible(visible)`, `SetCursorShape(shape)`, `SetTextInput(enabled)`, `WarpMouse(pixelPos)`, `Flash(op)`, `SafeAreaPixels()`
- `IsCursorVisible()`, `IsTextInputActive()`, `IsAspectRatioLocked()`, `GetCursorShape()`
- `SetDpiChangeCallback(fn, userCtx)`, `SimulateDpiMigration(...)`, `DpiMigrationCount()`, `ApplyLifecycleEvent(...)`, `RefreshMetrics()`, `ComputeAuditHash()`

### `Platform` & `FileWatcher` (`platform/Platform.h` — `sizeof(Platform) == 1856`, `alignas(64)`)
- `Init(appName, orgName, headless)`, `Shutdown()`, `IsInitialized()`, `IsHeadless()`, `ShouldQuit()`, `RequestQuit()`
- `Pump(window, tap)`, `Pump(window, input, tap)`, `FinalizePump(window, winFlags, needRefresh)`
- `SetLiveResizeWatch(window)`, `SetScreenSleepEnabled(enabled)`, `IsScreenSleepEnabled()`, `QueryPowerInfo()`
- `OpenGamepad(instanceId)`, `CloseGamepad(instanceId)`, `FindGamepadSlot(instanceId)`, `RumbleGamepad(...)`, `StopAllRumble()`, `ActiveGamepadMask()`, `RumbleGamepadMask()`
- `ResolveAsset(rel)`, `ResolveUser(rel)`, `BasePath()`, `PrefPath()`, `ReadFileBuffer(path, outBuf)`, `WriteFileAtomic(path, data)`
- `PushDroppedFile(filePath)`, `PollDroppedFiles(outFiles)`, `DroppedFileCount()`, `SetClipboardText(text)`, `GetClipboardText(outBuf)`, `ShowErrorBox(...)`
- `FileWatcher`: `Watch(path, assetId)`, `Unwatch(assetId)`, `Clear()`, `ActiveCount()`, `PollModified(outChangedIds)`
# `engine/platform/` — Contracts & Invariants

## Files
- `engine/platform/PlatformTypes.h` (Layer 1: `constexpr` window state, viewport math, and coalesced input event batch)
- `engine/platform/Window.h`, `engine/platform/Window.cpp` (Layer 2: move-only SDL3 window wrapper)
- `engine/platform/Platform.h`, `engine/platform/Platform.cpp` (Layer 2: SDL3 lifecycle, event pump, gamepad, and atomic file I/O)

## Contracts & Invariants

### 1. Window State & Viewport Scaling (`PlatformTypes.h`, `Window.h/.cpp`)
- **`WindowState` (`32B`, `alignas(8)`):** Tracks `logicalSize`, `pixelSize`, `pixelDensity`, `displayScale`, `refreshHz`, `mode`, `focused`, `suspended`, and `relativeMouse`.
  - **Invariant:** `pixelDensity > 0.0f`, `displayScale > 0.0f`, `refreshHz > 0.0f`, and `IsPresentable()` is true iff `!suspended && pixelSize.IsValid()`.
  - Supports `Stretch`, `Letterbox`, `IntegerScale`, and `Expand` 2D virtual canvas viewport mapping (`ComputeViewport`, `PixelToVirtual`, `VirtualToPixel`).
- **`Window` (`48B`, `alignas(16)`):** Move-only owner of `SDL_Window*`.
  - **State Synchronization Invariant:** `SetMode`, `SetRelativeMouse`, `SetCursorVisible`, and `SetTextInput` only update cached state (`m_state.mode`, `m_state.relativeMouse`, `m_cursorVisible`, `m_textInputActive`) after the underlying SDL3 call succeeds (or in headless mode when `m_window == nullptr`).

### 2. Coalesced Per-Frame Event Batch (`PlatformFrameEvents`)
- **`PlatformInputEvent` (`4B`, `alignas(4)`):** Packed 16-bit header (`code` in bits `[0..13]`, `InputEventKind` in bits `[14..15]`) and `i16 value` (`GamepadAxis` clamped from `-32768` to `-32767`).
- **`PlatformFrameEvents` (`256B`, `alignas(16)`):** Holds physical-pixel mouse state (`mouseX/Y/Dx/Dy`), wheel deltas, up to 31 UTF-8 text bytes (`textInput`), and up to `kMaxEvents = 49` discrete/axis events.
  - **Coalescing & Anti-Sticking Invariant:**
    - `GamepadAxis` events coalesce in-place so each axis occupies at most 1 slot per frame.
    - Duplicate consecutive button/key state edges are suppressed.
    - Release events (`down == false`) bypass focus and UI capture filters so held keys/buttons never stick when focus is lost or an overlay captures input.
    - **Full-Buffer Release Guarantee:** If `eventCount == kMaxEvents` when a discrete release event (`value == 0`) arrives, `PushEvent` never drops the release: it folds into an existing event for the same key/button if present in the frame batch, or overwrites the latest axis/press slot so physical state never sticks.

### 3. Event Pumping & Atomic File Persistence (`Platform.h/.cpp`)
- **Deferred Metrics Refresh Invariant:** During `Platform::Pump()`, window resize and display scale events set `needRefresh = true` via `ApplyLifecycleEvent`. `window.RefreshMetrics()` is executed once after the `SDL_PollEvent` loop completes, avoiding redundant SDL display/window queries when multiple resize events queue in the same frame.
- **Collision-Free Atomic Write Invariant:** `Platform::WriteFileAtomic` formats a timestamped temporary filename (`%s.%llx.tmp` via `SDL_GetTicksNS()`), writes and flushes the payload, and renames it over the target path (cleaning up the temp file on failure).

### 4. Struct Sizes (`static_assert` Locked)
- `Extent2i`: `8B` (`alignas(8)`)
- `WindowConfig`: `32B` (`alignas(8)`)
- `WindowState`: `32B` (`alignas(8)`)
- `PlatformInputEvent`: `4B` (`alignas(4)`)
- `PlatformFrameEvents`: `256B` (`alignas(16)`)
- `Window`: `48B` (`alignas(16)`)
- `Platform`: `784B` (`alignas(16)`)

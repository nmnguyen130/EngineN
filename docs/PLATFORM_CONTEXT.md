# EngineN — Platform Subsystem Context (`engine/platform/`)

## 1. Architecture & Invariants
- **Core:** C++23, zero heap allocation, 5-file footprint (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`), SDL3 encapsulation confined to `.cpp` files.
- **Single-Pass Pump (`Platform::Pump`):** Drives the per-frame OS lifecycle in one pass: `input.BeginFrame()` $\to$ `SDL_PollEvent` loop (`window.ProcessEvent` $\to$ system/gamepad/display $\to$ `EventTap` filter $\to$ High-DPI mouse scaling $\to$ `DispatchInputEvent`) $\to$ `input.EndFrame()`.
- **Anti-Sticking Capture Filter (`ShouldSuppressInput`):** When UI/overlays capture input via `EventTap` (`EventCapture::Keyboard | Mouse | Gamepad`), only initiating/continuous events (`KEY_DOWN`, `MOUSE_BUTTON_DOWN`, `MOUSE_MOTION`, `GAMEPAD_BUTTON_DOWN`) are suppressed. Release events (`KEY_UP`, `MOUSE_BUTTON_UP`, `GAMEPAD_BUTTON_UP`) always pass through to prevent stuck physical bits.
- **Focus & Suspend Safety:** `WindowFlags::FocusLost` (Alt-Tab) or `SDL_EVENT_WILL_ENTER_BACKGROUND` automatically triggers `StopAllRumble()` and `input.ClearPhysicalState()`.
- **Sandboxed Paths:** `ResolveAsset` and `ResolveUser` strictly validate paths (`IsSafeRelativePath`), blocking `..`, leading slashes, drive letters, and null bytes, returning normalized `FilePath`.

## 2. Core Layouts & Types (`PlatformTypes.h`, `Window.h`)
- `PlatformErrc`: `SdlInitFailed`, `WindowCreateFailed`, `FullscreenFailed`, `PathUnavailable`, `PathRejected`, `ClipboardFailed`, `InvalidState`.
- `Extent2i`: `{i32 w, h}`.
- `FixedString<N>`: Stack-allocated NUL-terminated buffer (`FilePath = FixedString<1024>`).
- `WindowMode`: `Windowed`, `Borderless`, `ExclusiveFullscreen`.
- `WindowFlags` (`u8` bitmask): `None`, `Resized`, `DisplayChanged`, `FocusGained`, `FocusLost`.
- `EventCapture` (`u8` bitmask): `None`, `Keyboard`, `Mouse`, `Gamepad`.
- `WindowState`: `logicalSize`, `pixelSize`, `pixelDensity`, `refreshHz`, `mode`, `focused`, `suspended`. Helpers: `IsPresentable()`, `AspectRatio()`.

## 3. Subsystem APIs
### `Window` (`engine/platform/Window.h` — Move-Only RAII)
- `Create(WindowConfig)` $\to$ `Result<void, Error>`, `Destroy()`, `IsValid()`, `Native()`, `Id()`, `State()`
- `SetMode(WindowMode)` $\to$ `Result<void, Error>`: Auto-queries closest fullscreen display mode when entering `ExclusiveFullscreen`.
- `SetTitle(title)`, `SetRelativeMouse(enabled)`, `SetCursorVisible(visible)`, `SetTextInput(enabled)`
- `ProcessEvent(SDL_Event)` $\to$ `WindowFlags`, `RefreshMetrics()`: Synchronously queries size, DPI density, and refresh rate.

### `Platform` (`engine/platform/Platform.h` — OS Coordinator)
- **Lifecycle & Frame Pump:**
  - `Init(appName, orgName)` $\to$ `Result<void, Error>`, `Shutdown()`, `ShouldQuit()`, `RequestQuit()`
  - `Pump(Window&, InputPipeline&, EventTap)` $\to$ `WindowFlags`: Dispatches events, applies DPI scaling, and finalizes input frame.
- **Hardware & OS Services:**
  - `RumbleGamepad(slot, lowFreq, highFreq, durationMs)`, `StopAllRumble()`: Manages hotplugged gamepad handles (up to 4).
  - `ResolveAsset(rel)`, `ResolveUser(rel)` $\to$ `Result<FilePath, Error>`: Resolves against base path and pref path.
  - `GetClipboard(span<char>)`, `SetClipboard(string_view)`, `ShowErrorBox(title, message, parent)`.

## 4. Main Loop Integration
```cpp
while (!m_platform.ShouldQuit()) {
    const WindowFlags winChanges = m_platform.Pump(m_window, m_input, m_devToolsTap);

    if (HasFlag(winChanges, WindowFlags::Resized)) {
        m_renderer.ResizeViewport(m_window.State().pixelSize);
    }

    m_time.BeginFrame();
    while (const auto step = m_time.PollTick()) {
        const TickInput tickInput = m_input.PollTick(step.TickIndex());
        m_actionBuffer.Feed(tickInput, step.IsFrozen(time::TimeChannel::Player));

        m_world.BeginTick(step);
        m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
        m_world.EndTick(step);
    }

    if (m_window.State().IsPresentable()) {
        auto& renderList = m_renderer.BeginFrame();
        world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), renderList);
        m_renderer.Submit(renderList);
    }
}
```
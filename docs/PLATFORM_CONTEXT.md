# EngineN — Platform Subsystem Context (`platform/`)

## 1. Architecture & Invariants
- **Standard:** C++23, zero heap allocation, minimal 5-file footprint (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`), SOLID/DOD compliant.
- **Deterministic Single-Pass Pump:** `Platform::Pump()` drives the entire per-frame OS lifecycle in a single pass: `input.BeginFrame()` $\rightarrow$ `SDL_PollEvent` loop (`EventTap` $\rightarrow$ System/Gamepad/Display $\rightarrow$ `Window::ProcessEvent` $\rightarrow$ High-DPI Mouse Scaling $\rightarrow$ `IngestSdlEvent`) $\rightarrow$ `input.EndFrame()`.
- **Anti-Sticking Capture Filter (`ShouldSuppressInput`):** DevTools/UI overlays (`EventTap`) return an `EventCapture` bitmask (`Keyboard | Mouse | Gamepad`). Only initiating/continuous events (`KEY_DOWN`, `MOUSE_BUTTON_DOWN`, `MOUSE_MOTION`, `MOUSE_WHEEL`, `GAMEPAD_BUTTON_DOWN`) are suppressed when captured; release edges (`KEY_UP`, `MOUSE_BUTTON_UP`, `GAMEPAD_BUTTON_UP`) and axis motions always pass through to `InputPipeline`, guaranteeing zero stuck physical bits or ghost latched taps.
- **Focus & Suspend Safety:** On `WindowFlags::FocusLost` (Alt-Tab) or `SDL_EVENT_WILL_ENTER_BACKGROUND`, `Platform::Pump()` automatically invokes `StopAllRumble()` and `input.ClearPhysicalState()`.
- **High-DPI Pixel Normalization:** Mouse coordinates and deltas (`motion.x/y/xrel/yrel`, `button.x/y`) are automatically scaled by `WindowState::pixelDensity` prior to `IngestSdlEvent`, ensuring `FrameInput::mousePos` maps 1:1 to the physical render viewport.
- **Encapsulation:** `<SDL3/SDL_*.h>` headers are strictly confined to `.cpp` implementation files; public headers expose only forward-declared `SDL_Window`, `SDL_Gamepad`, and `SDL_Event`.

## 2. Core Types (`platform/PlatformTypes.h` & `platform/Window.h`)
- **Primitives & Errors:**
  - `enum class PlatformErrc : u8`: `SdlInitFailed`, `WindowCreateFailed`, `FullscreenFailed`, `PathUnavailable`, `PathRejected`, `ClipboardFailed`, `InvalidState`.
  - `struct Extent2i { i32 w{0}; i32 h{0}; };`
  - `FixedString<N>`: Non-allocating, NUL-terminated inline string buffer (`FilePath = FixedString<1024>`).
- **Window Configuration & Modes:**
  - `enum class WindowMode : u8 { Windowed, Borderless, ExclusiveFullscreen };`
  - `enum class GraphicsApi : u8 { None, OpenGL, Vulkan, Metal };`
  - `struct WindowConfig`: `title`, `size{1280, 720}`, `minSize{640, 360}`, `mode`, `api`, `resizable{true}`, `highDpi{true}`.
- **Frame Event Bitmasks (`sizeof == 1`):**
  - `enum class WindowFlags : u8`: `None`, `Resized` (logical, pixel, or DPI scale changed), `DisplayChanged` (monitor moved or refresh rate changed), `FocusGained`, `FocusLost`. Queried via `constexpr bool HasFlag(WindowFlags val, WindowFlags mask) noexcept`.
  - `enum class EventCapture : u8`: `None`, `Keyboard`, `Mouse`, `Gamepad`. Queried via `constexpr bool HasCapture(EventCapture val, EventCapture mask) noexcept`.
- **Synchronous Window Metrics (`WindowState`):**
  - `Extent2i logicalSize`, `Extent2i pixelSize`, `f32 pixelDensity{1.0f}`, `f32 refreshHz{60.0f}`, `WindowMode mode`, `bool focused{true}`, `bool suspended{false}` (`minimized || occluded || hidden`).
  - Helpers: `constexpr bool IsPresentable() const noexcept` (`!suspended && pixelSize.w > 0 && pixelSize.h > 0`), `constexpr f32 AspectRatio() const noexcept`.

## 3. Public Interfaces & Signatures
### `Window` (`platform/Window.h` — Move-Only RAII Handle)
- Lifecycle & State:
  - `[[nodiscard]] Result<void, EngineError> Create(const WindowConfig& cfg = {}) noexcept`
  - `void Destroy() noexcept`
  - `[[nodiscard]] constexpr bool IsValid() const noexcept`, `SDL_Window* Native() const noexcept`, `u32 Id() const noexcept`
  - `[[nodiscard]] constexpr const WindowState& State() const noexcept`
- Runtime Mutators:
  - `[[nodiscard]] Result<void, EngineError> SetMode(WindowMode mode) noexcept` (auto-queries native desktop mode for `ExclusiveFullscreen` and syncs metrics)
  - `[[nodiscard]] Result<void, EngineError> SetTitle(std::string_view title) noexcept`
  - `[[nodiscard]] Result<void, EngineError> SetRelativeMouse(bool enabled) noexcept`
  - `void SetCursorVisible(bool visible) noexcept`, `void SetTextInput(bool enabled) noexcept`
  - `[[nodiscard]] WindowFlags ProcessEvent(const SDL_Event& ev) noexcept`, `void RefreshMetrics() noexcept`

### `Platform` & `EventTap` (`platform/Platform.h` — OS Coordinator)
- `struct EventTap`: Zero-allocation delegate (`EventTap::Bind<&T::Method>(instance)`) returning `EventCapture`.
- Lifecycle & Frame Pump:
  - `[[nodiscard]] Result<void, EngineError> Init(std::string_view appName = "EngineN", std::string_view orgName = "EngineN") noexcept`
  - `void Shutdown() noexcept`
  - `WindowFlags Pump(Window& window, InputPipeline& input, EventTap tap = {}) noexcept`
  - `[[nodiscard]] constexpr bool ShouldQuit() const noexcept`, `constexpr void RequestQuit() noexcept`
- Hardware & OS Services:
  - `bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept`, `void StopAllRumble() noexcept` (manages up to `kMaxGamepads = 4` hot-plugged `SDL_Gamepad*` handles)
  - `[[nodiscard]] Result<FilePath, EngineError> ResolveAsset(std::string_view relative) const noexcept` (sandboxed to `SDL_GetBasePath()`, rejects `..`, leading slashes, and drive letters)
  - `[[nodiscard]] Result<FilePath, EngineError> ResolveUser(std::string_view relative) const noexcept` (sandboxed to `SDL_GetPrefPath()`)
  - `[[nodiscard]] static std::string_view GetClipboard(std::span<char> outBuf) noexcept`, `static Result<void, EngineError> SetClipboard(std::string_view utf8) noexcept`
  - `static void ShowErrorBox(std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept`

## 4. Canonical Main Loop Usage
```cpp
while (!m_platform.ShouldQuit()) {
    const WindowFlags winChanges = m_platform.Pump(m_window, m_input, m_devToolsTap);

    if (HasFlag(winChanges, WindowFlags::Resized)) {
        m_renderer.ResizeViewport(m_window.State().pixelSize);
    }
    if (HasFlag(winChanges, WindowFlags::DisplayChanged)) {
        // Display refresh rate changes are dynamically accommodated by TimePipeline clock deltas
    }

    m_time.BeginFrame();
    while (const auto step = m_time.PollTick()) {
        const TickInput tickInput = m_input.PollTick(step.TickIndex());
        m_actionBuffer.Feed(tickInput, step.IsFrozen(TimeChannel::Player));
        m_world.Tick(step, tickInput, m_actionBuffer);
    }

    if (m_window.State().IsPresentable()) {
        m_renderer.Render(m_world, m_time.GetRenderView(), m_input.GetFrameView());
    }
}
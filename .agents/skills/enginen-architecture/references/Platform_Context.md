# EngineN — Platform Subsystem Context (`platform/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()`, minimal 5-file footprint (`PlatformTypes.h`, `Window.h/.cpp`, `Platform.h/.cpp`) + `tests/PlatformTests.cpp`, two-layer pure C++23 separation (`namespace en::platform`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Includes:**
  - **Layer 1 (`PlatformTypes.h`):** 100% `constexpr` C++23 geometry/window types (`Extent2i`, `WindowMode`, `GraphicsApi`, `WindowFlags`, `WindowConfig`, `WindowState`), 4-byte packed discrete input transition (`PlatformInputEvent`), 256-byte coalesced frame batch (`PlatformFrameEvents`), cache-line-aligned fixed-capacity string buffer (`FixedString<N>`, `FilePath`), single-pass path validator/joiner (`FixedString::Resolve`), and 64-bit SplitMix deterministic state audit (`WindowState::ComputeAuditHash()`, `PlatformFrameEvents::ComputeAuditHash()`). Zero OS, `<SDL3/SDL_*.h>`, or `input/` dependencies.
  - **Layer 2 (`Window.h/.cpp`, `Platform.h/.cpp`):** Move-only RAII `Window` (`sizeof == 64`) and single-instance `Platform` coordinator (`sizeof == 2368`). `<SDL3/SDL_*.h>` headers are strictly confined to `Window.cpp` and `Platform.cpp`. `platform/` never includes headers from `input/` or higher engine layers.
- **Deferred Single-Pass Window Metric Synchronization (Zero Poll-Loop OS Query Storms):**
  - `Window::ProcessEvent(ev, needMetricRefresh)` accumulates `WindowFlags` in registers and marks `needMetricRefresh = true` without invoking `SDL_GetWindow*` queries inside the `SDL_PollEvent` loop.
  - `Platform::Pump()` invokes `window.RefreshMetrics()` **at most once per frame** after the `SDL_PollEvent` loop finishes, collapsing multi-event resize/DPI/fullscreen bursts into a single OS query pass.
- **High-Frequency Input Coalescing (8000 Hz Mouse & Gamepad Axis Ready):**
  - Mouse motion (`mouseX/Y`, `mouseDx/Dy`) and mouse wheel (`wheelX/Y`) events are accumulated directly in `m_frameEvents` (Cache Line 1 of `Platform`) during `SDL_PollEvent` and scaled once by the post-refresh `WindowState::pixelDensity` at the end of `Pump()`.
  - Consecutive `GamepadAxis` events on the same `(padSlot, axis)` within a single frame are coalesced in-place via a single 16-bit comparison (`events[eventCount - 1].packedHeader == ev.packedHeader`).
- **Anti-Sticking Capture Filter & Immediate Focus-Loss Purge:**
  - Unified `DOWN`/`UP` event handlers enforce `!down || (isFocused && !HasCapture(capture, ...))`: release edges (`KEY_UP`, `MOUSE_BUTTON_UP`, `GAMEPAD_BUTTON_UP`) always pass through even when unfocused or captured by `EventTap`, whereas press/motion events require window focus and no capture.
  - On `WindowFlags::FocusLost` or `SDL_EVENT_WILL_ENTER_BACKGROUND`, `Platform::Pump()` immediately invokes `StopAllRumble()` and `m_frameEvents.ClearInput(true)`, purging any key-downs queued earlier in the same frame prior to Alt-Tab.
- **L1 Cache Line Layout & Bitmask Gamepad Dispatch:**
  - `Window` (`alignas(64)`, `sizeof == 64`): Places `WindowState m_state` (`32B`, `alignas(32)`) at offset `0` so `window.State()` resolves with zero pointer offset addition.
  - `Platform` (`alignas(64)`, `sizeof == 2368`): Places all hot runtime state in **Cache Line 0 (`offset 0..51`)** using Structure-of-Arrays (`m_gamepadHandles[4]`, `m_gamepadIds[4]`, `m_activeGamepadMask`, `m_rumbleGamepadMask`, `m_initialized`, `m_quit`). `StopAllRumble()`, `OpenGamepad()`, `CloseGamepad()`, `FindGamepadSlot()`, and `Shutdown()` use `std::countr_zero` / `std::countr_one` on 4-bit masks (`0` loop iterations when idle).
  - `PlatformFrameEvents` (`alignas(64)`, `sizeof == 256`, 4 cache lines): Packs the 28-byte continuous/status header AND the first 9 discrete `PlatformInputEvent` (`4B`) records inside **Cache Line 0 (`offset 0..63`)**, so typical frames (`<= 9` button/axis transitions) touch only a single 64B cache line.
  - `FixedString<N>` (`alignas(64)` for `N >= 64`, `sizeof == N`): Places `u16 m_len` at offset `0` alongside `m_buf[0..61]` in Cache Line 0 with zero cache-line tail spill. `m_basePath` and `m_prefPath` are normalized once at `Init()` (`AssignNormalizedDir`), allowing `ResolveAsset()` and `ResolveUser()` to validate and append relative paths in a single pass (`Resolve()`).

## 2. Core Types (`platform/PlatformTypes.h`)
- **Primitives, Errors & Strings (Zero Padding):**
  - `enum class PlatformErrc : u8`: `None = 0`, `SdlInitFailed`, `WindowCreateFailed`, `FullscreenFailed`, `PathUnavailable`, `PathRejected`, `ClipboardFailed`, `InvalidState`.
  - `struct alignas(8) Extent2i` (`sizeof == 8`): `w`, `h`, `IsValid()`, `AspectRatio()`.
  - `template <usize N> class FixedString` (`sizeof == N`, `kMaxLength = N - 3`): `Assign`, `AssignNormalizedDir`, `Resolve` (single-pass validation + append; rejects empty, leading/trailing slashes, `.`, `..`, mixed-slash traversal `subdir/..\`, `:`, and control chars `< 0x20`), `View`, `CStr`, `Size`, `Empty`, `Capacity`. `using FilePath = FixedString<1024>;` (`sizeof == 1024`, `alignof == 64`).
- **Window Configuration, Flags & State:**
  - `enum class WindowMode : u8 { Windowed = 0, Borderless = 1, ExclusiveFullscreen = 2 };`
  - `enum class GraphicsApi : u8 { None = 0, OpenGL = 1, Vulkan = 2, Metal = 3 };`
  - `enum class WindowFlags : u8`: `None`, `Resized`, `DisplayChanged`, `FocusGained`, `FocusLost`, `CloseRequested`. Queried via `constexpr bool HasFlag(WindowFlags val, WindowFlags mask) noexcept`.
  - `struct WindowConfig`: `title`, `size{1280, 720}`, `minSize{640, 360}`, `mode`, `api`, `resizable{true}`, `highDpi{true}`.
  - `struct alignas(32) WindowState` (`sizeof == 32`): `logicalSize`, `pixelSize`, `pixelDensity{1.0f}`, `displayScale{1.0f}`, `refreshHz{60.0f}`, `mode`, `focused{true}`, `suspended{false}`, `relativeMouse{false}`, `IsPresentable()`, `AspectRatio()`, `ComputeAuditHash()`.
- **Coalesced Input & Event Capture Types:**
  - `enum class EventCapture : u8`: `None`, `Keyboard`, `Mouse`, `Gamepad`. Queried via `constexpr bool HasCapture(EventCapture val, EventCapture mask) noexcept`.
  - `enum class InputEventKind : u8`: `Key = 0`, `MouseButton = 1`, `GamepadButton = 2`, `GamepadAxis = 3`.
  - `struct alignas(4) PlatformInputEvent` (`sizeof == 4`, 16 events per 64B cache line): `packedHeader` (`u16`: bits `[0..11]` code, bits `[12..13]` padSlot `0..3`, bits `[14..15]` `InputEventKind`), `value` (`i16`). Accessors: `Make()`, `Code()`, `PadSlot()`, `Kind()`, `Down()`.
  - `struct alignas(64) PlatformFrameEvents` (`sizeof == 256`, 4 cache lines, zero padding):
    - Cache Line 0 (`offset 0..63`): 28B header (`mouseX`, `mouseY`, `mouseDx`, `mouseDy`, `wheelX`, `wheelY`, `eventCount`, `disconnectedPadMask`, `focusCleared`, `hasMouseMotion`) + `events[0..8]` (`36B`).
    - Cache Lines 1..3 (`offset 64..255`): `events[9..56]` (`kMaxFrameInputEvents = 57`).
    - Methods: `ClearInput(bool markFocusLost = false)`, `PushEvent(InputEventKind, u8 padSlot, u16 code, i16 value)`, `Events()`, `HasMouseWheel()`, `ComputeAuditHash()`.

## 3. Public Interfaces & Signatures
### `Window` (`platform/Window.h` — 64B L1-Aligned Move-Only RAII Handle)
- Lifecycle & State:
  - `[[nodiscard]] Result<void, PlatformErrc> Create(const WindowConfig& cfg = {}) noexcept`
  - `void Destroy() noexcept`
  - `[[nodiscard]] constexpr bool IsValid() const noexcept`, `SDL_Window* Native() const noexcept`, `u32 Id() const noexcept`
  - `[[nodiscard]] constexpr const WindowState& State() const noexcept` (zero-offset reference to Cache Line 0)
- Runtime Mutators & Audit:
  - `[[nodiscard]] Result<void, PlatformErrc> SetMode(WindowMode mode) noexcept` (O(1) early-out if unchanged; clears fullscreen mode when leaving `ExclusiveFullscreen`)
  - `[[nodiscard]] Result<void, PlatformErrc> SetTitle(std::string_view title) noexcept`
  - `[[nodiscard]] Result<void, PlatformErrc> SetRelativeMouse(bool enabled) noexcept` (O(1) early-out via `m_state.relativeMouse`)
  - `void SetCursorVisible(bool visible) noexcept`, `void SetTextInput(bool enabled) noexcept`
  - `[[nodiscard]] WindowFlags ProcessEvent(const SDL_Event& ev, bool& outNeedRefresh) noexcept`, `void RefreshMetrics() noexcept`
  - `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`

### `Platform` & `EventTap` (`platform/Platform.h` — 64B-Aligned Single-Instance OS Coordinator)
- `struct EventTap`: Zero-allocation delegate (`EventTap::Bind<&T::Method>(instance)`) returning `EventCapture`.
- Lifecycle & Coalesced Frame Pump:
  - `[[nodiscard]] Result<void, PlatformErrc> Init(std::string_view appName = "EngineN", std::string_view orgName = "EngineN") noexcept`
  - `void Shutdown() noexcept`
  - `WindowFlags Pump(Window& window, EventTap tap = {}) noexcept`
  - `template <typename InputSinkT> WindowFlags Pump(Window& window, InputSinkT& input, EventTap tap = {}) noexcept` (invokes `Pump(window, tap)` followed by single-call `input.IngestPlatformFrame(m_frameEvents)`)
  - `[[nodiscard]] constexpr const PlatformFrameEvents& FrameEvents() const noexcept`
  - `[[nodiscard]] constexpr bool ShouldQuit() const noexcept`, `constexpr void RequestQuit() noexcept`
- Hardware, Sandboxed Paths, Clipboard & Audit:
  - `bool RumbleGamepad(u8 padSlot, u16 lowFreq, u16 highFreq, u32 durationMs) noexcept`, `void StopAllRumble() noexcept`, `[[nodiscard]] constexpr u8 ActiveGamepadMask() const noexcept`
  - `[[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveAsset(std::string_view relative) const noexcept`
  - `[[nodiscard]] constexpr Result<FilePath, PlatformErrc> ResolveUser(std::string_view relative) const noexcept`
  - `[[nodiscard]] constexpr std::string_view BasePath() const noexcept`, `[[nodiscard]] constexpr std::string_view PrefPath() const noexcept`
  - `[[nodiscard]] static std::string_view GetClipboard(std::span<char> outBuf) noexcept`, `[[nodiscard]] static Result<void, PlatformErrc> SetClipboard(std::string_view utf8) noexcept`
  - `static void ShowErrorBox(std::string_view title, std::string_view message, const Window* parent = nullptr) noexcept`
  - `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
while (!m_platform.ShouldQuit()) {
    const WindowFlags winChanges = m_platform.Pump(m_window, m_input, m_devToolsTap);

    if (HasFlag(winChanges, WindowFlags::FocusGained)) {
        m_time.OnWindowRefocus();
    }
    m_audio.OnWindowEvent(winChanges, m_window.State());

    m_time.BeginFrame();
    while (const auto step = m_time.PollTick()) {
        const TickInput tickInput = m_input.PollTick(step.TickIndex());
        m_actionBuffer.Feed(tickInput, step.IsFrozen(TimeChannel::Player));
        m_world.Tick(step, tickInput, m_actionBuffer);
    }

    if (m_window.State().IsPresentable()) {
        en::renderer::RenderList& list = m_renderer.BeginFrame(m_window.State().pixelSize);
        en::world::WorldExtractor::Extract(m_world, m_assets.Bank(), m_time.GetRenderView(), list);
        m_renderer.Submit();
    }
}
```
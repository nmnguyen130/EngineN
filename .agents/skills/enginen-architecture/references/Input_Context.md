# EngineN — Input Subsystem Context (`input/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation, 100% `constexpr` header-only 3-file footprint (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`) + `tests/InputTests.cpp` (`namespace en::input`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Includes:**
  - **Layer 1 (`InputTypes.h`, `ActionBuffer.h`, `InputPipeline.h`):** 100% self-contained pure C++23 pull-evaluated input pipeline, direct-indexed action/axis binding table (`InputMap`), deterministic tick-counted intent buffer (`ActionBuffer`), and 64-bit FNV-1a deterministic state audit (`TickInput::ComputeAuditHash()`, `ActionBuffer::ComputeAuditHash()`, `InputPipeline::ComputeAuditHash()`). Depends solely on `"core/Types.h"` and `"math/Vec2.h"` — zero OS, `<SDL3/SDL_*.h>`, or `platform/` includes.
  - **Layer 2 (`platform::Platform::Pump` -> `InputPipeline::IngestPlatformFrame`):** All OS event polling (`SDL_PollEvent`) and high-frequency event coalescing are handled in `platform::Platform::Pump()`, which populates a 256-byte `PlatformFrameEvents` batch and invokes `input.IngestPlatformFrame(m_frameEvents)` once per frame.
- **L1 Cache Line Partitioning (`sizeof(InputPipeline) == 640`, `10` Cache Lines Exact):**
  - **Cache Line 0 (`offset 0..63`, `64B` exact):** Packs `m_heldTicks[16]` (`32B`), `m_anyPadButtons` (`4B` OR-mask across all 4 gamepad slots), `m_actionMask`, `m_tickTap`, `m_frameTap`, `m_prevTickHeld`, `m_prevFrameHeld`, `m_keyWordMask` (`1B` 8-bit summary of non-zero `u64` words in `m_keyBits`), `m_mouseBits` (`1B`), and `m_lastAxisDir[4]`. On idle ticks/frames, `PollTick()` and `EndFrame()` touch **only Cache Line 0** (`0` L1 misses, `0` loop iterations).
  - **Cache Line 1 (`offset 64..127`, `64B` exact):** `FrameInput m_frame` (`alignas(64)`).
  - **Cache Line 2 (`offset 128..191`, `64B` exact):** `std::array<u64, 8> m_keyBits` (`512` scancodes).
  - **Cache Line 3 (`offset 192..255`, `64B` exact):** `std::array<GamepadState, 4> m_pads` (`4 x 16B`, `alignas(16)`, zero padding, indexed directly by `padSlot [0..3]` with zero `instanceId` or `connected` flags).
  - **Cache Lines 4..9 (`offset 256..591`, `336B`):** `InputMap m_map` (`alignas(64)`).
- **Decoupled Dual Tap Latches (`m_tickTap` & `m_frameTap`):**
  - Sub-frame taps (`Down` + `Up` within `< 8.33ms`) are latched into `m_tickTap` (consumed by `PollTick()`) and `m_frameTap` (consumed by `EndFrame()`).
  - Prevents 0-tick high-refresh render frames (`240Hz`/`480Hz`) from phantom-repeating UI presses on subsequent frames while still preserving the tap for the next 120Hz simulation tick (`pressed = 1, held = 1, released = 0` on Tick $N$, `released = 1, held = 0` on Tick $N+1$).
  - Calling `SetActionMask(mask)` immediately masks both `m_tickTap` and `m_frameTap` (and `RecordTapAndSocd` gates by `m_actionMask`), preventing keystrokes typed in an overlay/console from firing as stale gameplay taps after unpausing.
- **Zero-Bloat, Zero-Idle-Loop Evaluation:**
  - `InputMap::Bind`, `BindSlot`, and `BindAxis` validate `Source::IsValid()` on insertion so stored slots are guaranteed valid or `Device::None` (eliminating redundant `s.IsValid()` switches inside `EvaluateLiveHeld()` and `RecordTapAndSocd()`).
  - `EvaluateLiveHeld()` exits in $O(1)$ when `(m_keyWordMask | m_mouseBits | m_anyPadButtons) == 0`.
  - `PollTick()` updates `m_heldTicks` by iterating only over `released` (`~tickHeld & m_prevTickHeld`) and `tickHeld` via `std::countr_zero` (`0` loop iterations when idle).
  - `ActionBuffer::StepChannel()` exits immediately when `(ch.pending | edges) == 0`, hashes only active pending bits in `ComputeAuditHash()`, and exposes $O(1)$ `PendingPressMask()` / `PendingReleaseMask()` (`u16`).

## 2. Core Types (`input/InputTypes.h`)
- **Constants & Concepts:**
  - `kMaxGamepads = 4`, `kMaxActions = 16`, `kMaxAxes = 4`, `kMaxBindingsPerSlot = 4`, `kInvAxisMax = 1.0f / 32767.0f`.
  - `concept InputId<T>`: Accepts any unsigned integral or `u8`/`u16`/`u32`-backed scoped enum. Converted via `constexpr u8 ToInputIndex(InputId auto id) noexcept` and `constexpr u16 MakeActionMask(InputId auto... actions) noexcept`.
- **Physical Descriptor (`sizeof(Source) == 4`, `alignas(4)`):**
  - `struct Source`: `device` (`Device`), `gamepadIndex` (`u8`, `kAnyGamepad = 0xFF`), `code` (`u16`).
  - Factories & Queries: `Source::Key(u16)`, `Source::Mouse(u16)`, `Source::Pad(u16, u8 = kAnyGamepad)`, `IsValid()`, `Matches(Source physical)`.
- **Direct-Indexed Binding Table (`InputMap`, `sizeof == 336`):**
  - `actions[16][4]` (`256B`), `axes[4]` (`80B`, `sizeof(AxisBinding) == 20`).
  - Fluent builders: `Bind(action, {sources...})`, `BindSlot(action, slot, src)`, `BindAxis(axis, AxisBinding{...})`.
- **L1 Cache-Aligned Snapshots (`sizeof == 64`, `alignas(64)`):**
  - `struct alignas(64) TickInput` (`sizeof == 64`): `tick`, `held`, `pressed`, `released`, `axis[4]`, `heldTicks[16]`. Queries: `Held(a)`, `Pressed(a)`, `Released(a)`, `HeldTicks(a)`, `Axis(ax)`, `Stick(axX, axY, radialDeadzone)`, `ComputeAuditHash()`.
  - `struct alignas(64) FrameInput` (`sizeof == 64`): `frameIndex`, `held`, `pressed`, `released`, `activeDevice`, `lastPressed`, `mousePos`, `mouseDelta`, `mouseWheel`. Queries: `Held(a)`, `Pressed(a)`, `Released(a)`.

## 3. Public Interfaces & Signatures
### `ActionBuffer` (`input/ActionBuffer.h` — `sizeof == 36`, Deterministic Intent Forgiveness)
- `static constexpr u8 kMaxBufferTicks = 60;` (500ms at 120Hz)
- `constexpr void Feed(const TickInput& in, bool channelFrozen = false) noexcept`
- `constexpr void Clear() noexcept`
- `[[nodiscard]] constexpr u16 PendingPressMask() const noexcept`, `PendingReleaseMask() const noexcept`
- `[[nodiscard]] constexpr bool Peek(InputId auto a, u8 windowTicks) const noexcept`, `PeekRelease(a, windowTicks)`
- `[[nodiscard]] constexpr bool Consume(InputId auto a, u8 windowTicks) noexcept`, `ConsumeRelease(a, windowTicks)`
- `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`

### `InputPipeline` (`input/InputPipeline.h` — `sizeof == 640`, `alignof == 64`)
- `using InputSystem = InputPipeline;`
- Coalesced Frame Ingestion & Simulation Tick Polling:
  - `template <typename FrameBatchT> constexpr const FrameInput& IngestPlatformFrame(const FrameBatchT& batch) noexcept`
  - `constexpr void BeginFrame() noexcept`, `[[nodiscard]] constexpr const FrameInput& EndFrame() noexcept`
  - `[[nodiscard]] constexpr const FrameInput& GetFrameView() const noexcept`
  - `[[nodiscard]] constexpr TickInput PollTick(u64 tickIndex) noexcept`
- Configuration, Direct Feed & Audit:
  - `[[nodiscard]] constexpr InputMap& Map() noexcept` (`const` & mutable)
  - `constexpr void SetActionMask(u16 enabledActionsMask) noexcept`, `[[nodiscard]] constexpr u16 GetActionMask() const noexcept`
  - `constexpr void FeedButton(Source src, bool down) noexcept`
  - `constexpr void FeedMouseMotion(Vec2 pos, Vec2 delta) noexcept`, `FeedMouseWheel(Vec2 wheel) noexcept`
  - `constexpr void FeedGamepadButton(u8 padSlot, u16 button, bool down) noexcept`, `FeedGamepadAxis(u8 padSlot, u8 axisIdx, i16 value) noexcept`
  - `constexpr void DisconnectGamepad(u8 padSlot) noexcept`, `constexpr void ClearPhysicalState() noexcept`
  - `[[nodiscard]] constexpr bool IsPhysicallyDown(Source src) const noexcept`
  - `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
const auto winChanges = m_platform.Pump(m_window, m_input, m_devToolsTap); // Calls m_input.IngestPlatformFrame(m_frameEvents)
const auto& frameInput = m_input.GetFrameView();

m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const TickInput tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::time::TimeChannel::Player));
    m_world.Tick(step, tickInput, m_actionBuffer);
}
m_renderer.Render(m_world, m_time.GetRenderView(), frameInput);
```
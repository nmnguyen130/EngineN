# EngineN — Core Time Subsystem Context (`core/time/`)

## 1. Architecture & Invariants
- **Standard:** C++23, zero heap allocation, header-only core + SDL3 clock adapter, SOLID/ISP/DOD compliant.
- **Deterministic Baseline:** `kSimHz = 120` (`kFixedDtNs = 8,333,333`, `kFixedDtSec = 1.0f / 120.0f`).
- **Accumulator:** Exact integer rational units (`ns * kSimHz`, where `10^9 units == 1 tick`) for zero floating-point drift. Includes VSync harmonic snapping (240/120/60/30Hz within ±150µs) and spiral-of-death clamping (`maxCatchupTicks = 8`, `maxFrameDeltaNs = 250ms`).
- **Literals:** `consteval operator""_f60` (converts 60FPS design frames to 120Hz ticks, e.g., `6_f60 == 12`) and `operator""_ticks`.
- **Encapsulation:** Gameplay events (`SimEvents`) are strictly decoupled from `core/time/`.

## 2. Core Types (`core/Types.h` & `core/time/TimeTypes.h`)
- **Primitives:** `u8, u16, u32, u64, i8, i16, i32, i64, usize, isize, f32, f64` in `namespace EngineN`.
- **Channels (`kMaxTimeChannels = 8`):**
  `enum class TimeChannel : u8 { World = 0, Player, Enemy, Projectile, Vfx, Camera, Ui, Custom };`
- **Concepts:**
  - `ChannelId<T>`: Accepts `TimeChannel`, any `u8`-backed scoped enum, or unsigned integral. Converted via `u8 ToChannelIndex(ChannelId auto ch) noexcept`.
  - `ClockSource<T>`: Requires `{ clock.NowNs() } noexcept -> std::same_as<u64>`. Implemented by `SdlClock` (`SDL_GetTicksNS()`) and `ManualClock` (unit testing).
- **L1 Cache-Aligned Snapshots:**
  - `struct alignas(64) BakedTickState { std::array<f32, 8> scaledDt; std::array<f32, 8> effectiveScale; };`
  - `struct alignas(32) BakedRenderState { std::array<f32, 8> alpha; };` (locks channel alpha to `1.0f` when frozen by hitstop to prevent interpolation jitter).

## 3. Public Interfaces & Signatures
### `SimStepView` (`core/time/TimeViews.h` — Passed to all `Tick()` functions)
- `explicit operator bool() const noexcept` (true when tick is active)
- `f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept` (O(1) read from `BakedTickState`)
- `f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept`
- `bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept`
- `u64 TickIndex() const noexcept` (monotonic 120Hz simulation tick counter)
- `u32 CatchupIndex() const noexcept` (sub-step index `[0..maxCatchupTicks)` within current frame)
- `void RequestHitstop(ChannelId auto ch, u32 ticks, f32 intensity = 0.0f) const noexcept` (duration-dominant merge; `0.0f` = hard freeze)
- `void RequestMultiHitstop(u32 ticks, f32 intensity, Channels... chs) const noexcept`
- `void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) const noexcept` (smoothstep ease-out; pauses while frozen)
- `void SetBaseScale(ChannelId auto ch, f32 scale) const noexcept`

### `RenderTimeView` (`core/time/TimeViews.h` — Passed to `Render()`)
- `f32 Alpha(ChannelId auto ch = TimeChannel::World) const noexcept`
- Fields: `BakedRenderState channelAlpha`, `f32 rawAlpha`, `f32 realDt`, `u64 tickIndex`.

### `TimePipeline<ClockT>` (`core/time/TimePipeline.h` — `using TimeManager = TimePipeline<SdlClock>`)
- Frame Lifecycle:
  - `void BeginFrame() noexcept` / `constexpr void BeginFrameManual(u64 rawDeltaNs) noexcept`
  - `[[nodiscard]] constexpr SimStepView PollTick() noexcept` (auto-closes previous tick, advances hitstop/ramps, bakes `BakedTickState`, finalizes frame alpha on exit)
  - `[[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept`
- Control & State:
  - `void SetPaused(bool paused) noexcept`, `constexpr void RequestSingleStep() noexcept` (advances 1 tick while paused), `void OnWindowRefocus() noexcept`
  - `bool IsPaused() const noexcept`, `const TimeTelemetry& Telemetry() const noexcept`, `const ChannelModulator& Modulator() const noexcept`, `const ClockT& Clock() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    m_world.Tick(step); // Reads step.TickIndex(), step.Dt(ch), step.IsFrozen(ch)
}
m_renderer.Render(m_world, m_time.GetRenderView());
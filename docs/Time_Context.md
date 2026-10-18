# EngineN — Core Time Subsystem Context (`core/time/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation, minimal 5-unit footprint (`TimeTypes.h`, `ChannelModulator.h`, `TimeViews.h`, `TimePipeline.h`, `SdlClock.h/.cpp`), two-layer pure C++23 separation (`namespace en::time`).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`TimeTypes.h`, `ChannelModulator.h`, `TimeViews.h`, `TimePipeline.h`):** 100% `constexpr` C++23 rational accumulator, O(1) bitmask-driven 8-channel modulator, `ManualClock`, and 64-bit SplitMix deterministic state audit (`ChannelModulator::ComputeAuditHash()`, `TimePipeline::ComputeAuditHash()`). Zero OS or `<SDL3/SDL_*.h>` dependencies.
  - **Layer 2 (`SdlClock.h/.cpp`):** Thin SDL3 wall-clock adapter (`SdlClock::NowNs()` + `using TimeManager = TimePipeline<SdlClock>;`). `<SDL3/SDL_timer.h>` is strictly confined to `SdlClock.cpp`.
- **Deterministic Baseline & Rational Accumulator:**
  - `kSimHz = 120` (`kFixedDtNs = 8,333,333`, `kFixedDtSec = 1.0f / 120.0f`, `kInvNsPerSecond = 1e-9`).
  - Accumulates in exact integer rational units (`ns * kSimHz`, where `kNsPerSecond == 10^9 units == 1 exact tick`) for zero floating-point drift. Exact multiples of `kFixedDtNs` scale losslessly to `N * kNsPerSecond`.
  - **O(1) Power-of-Two VSync Harmonic Snapping (`SnapToRefreshHarmonicUnits`):** Branchlessly snaps frame deltas within `±vsyncSnapMarginNs` (`150µs`) to power-of-two multiples `{1, 2, 4, 8, 16}` of a quarter-tick (`250M` units), supporting `480Hz`, `240Hz`, `120Hz`, `60Hz`, and `30Hz` with zero loop iterations and zero integer division truncation.
  - **Spiral-of-Death Protection:** Clamps frame delta (`maxFrameDeltaNs = 250ms`) and substeps (`maxCatchupTicks = 8`, sanitized `>= 1`).
- **L1 Cache Line Layout & Bitmask-Driven Zero-Loop Modulation (`ChannelModulator`):**
  - **Cache Line 0 (`offset 0..63`, `alignas(64)`):** `BakedTickState m_bakedTick` sits at offset `0` of both `ChannelModulator` and `TimePipeline`, placing `scaledDt[8]` and `effectiveScale[8]` in the first 64B L1 cache line.
  - **Cache Line 1 (`offset 64..127`):** `m_baseScales[8]` + three 8-bit active state masks (`m_frozenMask`, `m_activeHitstopMask`, `m_activeRampMask`).
  - **Zero-Loop Idle Fast Path & Single-Channel Immediate Rebake:** `SetBaseScale`, `RequestHitstop`, `RequestMaskHitstop`, `RequestTimeRamp`, and `ClearChannelEffects` immediately rebake only the modified channel (`BakeChannel(idx)`), keeping `m_bakedTick` and `m_frozenMask` 100% synchronized at all times. `AdvanceTick()` exits immediately when `(m_activeHitstopMask | m_activeRampMask) == 0` and iterates exclusively over set bits via `std::countr_zero(mask)`.
- **Zero-Loop Cross-Subsystem Communication Contract:**
  - `TimePipeline::FrozenMask()` (`u8`, automatically OR-ed with `kGameplayChannelsMask = 0x3Fu` when paused, leaving `Ui` bit 6 and `Custom` bit 7 active) and `TimePipeline::EffectiveScales()` (`std::span<const f32, 8>`) persist across 0-tick high-refresh frames (e.g., 240Hz display running 120Hz simulation).
  - `Application`, `AudioSystem::Update`, and `DebugProfiler::RecordSubsystemTelemetry` query `m_time.FrozenMask()` and `m_time.EffectiveScales()` directly in O(1) without per-tick 8-channel loops or external latched arrays.

## 2. Core Types (`core/time/TimeTypes.h`)
- **Constants & Literals (`inline namespace literals`):**
  - `kSimHz = 120`, `kNsPerSecond = 1'000'000'000ULL`, `kFixedDtNs = 8'333'333ULL`, `kFixedDtSec = 1.0f / 120.0f`, `kMaxTimeChannels = 8`, `kGameplayChannelsMask = 0x3Fu`.
  - `consteval u32 operator""_f60(unsigned long long frames) noexcept` (converts 60FPS frames to 120Hz ticks, e.g. `6_f60 == 12`) and `consteval u32 operator""_ticks(unsigned long long ticks) noexcept`.
- **Channels & Concepts:**
  - `enum class TimeChannel : u8 { World = 0, Player, Enemy, Projectile, Vfx, Camera, Ui, Custom };`
  - `concept ChannelId<T>`: Accepts `TimeChannel`, any `u8`-backed scoped enum, or unsigned integral. Converted via `constexpr u8 ToChannelIndex(ChannelId auto ch) noexcept` and `constexpr u8 MakeChannelMask(Channels... chs) noexcept`.
  - `concept ClockSource<T>`: Requires `{ clock.NowNs() } noexcept -> std::same_as<u64>`. Implemented by Layer 1 `ManualClock` and Layer 2 `SdlClock`.
- **L1 Cache-Aligned Snapshots:**
  - `struct alignas(64) BakedTickState`: `std::array<f32, 8> scaledDt`, `std::array<f32, 8> effectiveScale` (`sizeof == 64`).
  - `struct alignas(32) BakedRenderState`: `std::array<f32, 8> alpha` (`sizeof == 32`).
  - `struct alignas(64) RenderTimeView`: `BakedRenderState channelAlpha`, `f32 rawAlpha`, `f32 realDt`, `u64 tickIndex`, `u8 frozenMask` (`sizeof == 64`).

## 3. Public Interfaces & Signatures
### `SimStepView` (`core/time/TimeViews.h` — Passed by Value to `Tick()` Functions)
- `explicit operator bool() const noexcept`
- `f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept`, `f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept`, `bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept`
- `u8 FrozenMask() const noexcept`, `std::span<const f32, 8> EffectiveScales() const noexcept`, `std::span<const f32, 8> ScaledDts() const noexcept`, `const BakedTickState& BakedTick() const noexcept`
- `u64 TickIndex() const noexcept`, `u32 CatchupIndex() const noexcept`
- `void RequestHitstop(ChannelId auto ch, u32 ticks, f32 intensity = 0.0f) const noexcept`
- `void RequestMultiHitstop(u32 ticks, f32 intensity, Channels... chs) const noexcept`, `void RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity = 0.0f) const noexcept`
- `void RequestTimeRamp(ChannelId auto ch, f32 targetScale, u32 holdTicks, u32 blendOutTicks) const noexcept`
- `void SetBaseScale(ChannelId auto ch, f32 scale) const noexcept`, `void ClearChannelEffects(ChannelId auto ch) const noexcept`

### `RenderTimeView` (`core/time/TimeViews.h` — 64B Snapshot Passed to `Extract()` / `Update()`)
- `f32 Alpha(ChannelId auto ch = TimeChannel::World) const noexcept`
- `bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept`

### `TimePipeline<ClockT>` (`core/time/TimePipeline.h` & `core/time/SdlClock.h`)
- Frame Lifecycle:
  - `constexpr void BeginFrame() noexcept` / `constexpr void BeginFrameManual(u64 rawDeltaNs) noexcept`
  - `[[nodiscard]] constexpr SimStepView PollTick() noexcept`
  - `[[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept`
- Control, Direct Queries & Audit:
  - `constexpr void SetPaused(bool paused) noexcept`, `constexpr void RequestSingleStep() noexcept`, `constexpr void OnWindowRefocus() noexcept`
  - `[[nodiscard]] constexpr bool IsPaused() const noexcept`, `[[nodiscard]] constexpr u8 FrozenMask() const noexcept`, `[[nodiscard]] constexpr bool IsFrozen(ChannelId auto ch = TimeChannel::World) const noexcept`
  - `[[nodiscard]] constexpr f32 Dt(ChannelId auto ch = TimeChannel::World) const noexcept`, `[[nodiscard]] constexpr f32 EffectiveScale(ChannelId auto ch = TimeChannel::World) const noexcept`
  - `[[nodiscard]] constexpr std::span<const f32, 8> EffectiveScales() const noexcept`, `[[nodiscard]] constexpr std::span<const f32, 8> ScaledDts() const noexcept`
  - `[[nodiscard]] constexpr ChannelModulator& Modulator() noexcept` (`const` & mutable), `[[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept`
  - `[[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
m_time.BeginFrame();
while (const auto step = m_time.PollTick()) {
    const auto tickInput = m_input.PollTick(step.TickIndex());
    m_actionBuffer.Feed(tickInput, step.IsFrozen(en::time::TimeChannel::Player));

    m_world.BeginTick(step);
    m_gameplay.Tick(m_world, m_assets.Bank(), step, tickInput, m_actionBuffer);
    m_world.EndTick(step);

    m_debug.EndSimTick(m_world, step, tickInput, m_actionBuffer);
}

const auto renderTime = m_time.GetRenderView();
m_audio.ConsumeSimCues(m_gameplay.DrainCues(), m_world, m_assets.Bank());
m_audio.Update(m_world, m_gameplay.Spatial(), m_assets.Bank(), renderTime, m_time.FrozenMask(), m_time.EffectiveScales());
m_worldExtractor.Extract(m_world, m_assets.Bank(), renderTime, m_renderer.BeginFrame());
```
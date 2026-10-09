# Timeloop & Simulation Timing System: Design Report for a 2D Action Metroidvania

**Sourcing.** I checked the SDL3 timing API and Celeste's time handling directly. For Hollow Knight, Dead Cells and The Lost Crown, public information about their loop internals is thin, so I'm not attributing specifics to them. Celeste is the best-documented precedent. The other references below (Fiedler's *Fix Your Timestep*, Maddy Thorson's physics write-ups, Jared Cone's Rocket League physics talk, Overwatch's gameplay architecture talk) are from my own knowledge, so verify them before citing.

---

## 1. Core responsibilities and decisions

### 1.1 Timestep model: fixed step with accumulator and interpolation

| Model | Determinism | Feel | Verdict |
|---|---|---|---|
| Variable dt | None. Jump height, collision and tunneling all depend on frame time | Inconsistent | Reject for simulation |
| Semi-fixed (clamped variable substeps) | Weak. Results depend on how the step count was split | Mostly fine | Reject, since it only hides the problem |
| **Fixed step + accumulator + render interpolation** | Exact for a given input sequence | Frame-data-exact combat | **Use this** |

A precision-platformer or action game needs exact reproducibility of coyote time, jump buffers, attack frames and cancel windows. Only a fixed step gives that. Celeste's developer said the game is locked to 60 fps so that every playthrough stays deterministic The Celeste developer said the 60 fps lock is deliberate, to keep the game deterministic for every playthrough.

### 1.2 The exact math, in integers

At 120 Hz the step is 10⁹/120 = 8,333,333.33… ns, which is not an integer. Truncating it to 8,333,333 loses about 40 ns per second of sim time, roughly 0.14 ms per hour. That is harmless for gameplay but unnecessary, and it makes "sim time equals wall time" tests flaky. Instead, keep the accumulator in units of **ns × Hz**:

```
acc += dt_ns * SimHz              // dt_ns clamped first
while acc >= 1'000'000'000: acc -= 1'000'000'000; tick()
alpha = acc / 1e9                 // [0,1)
```

This is exact: no rounding, no drift, no floats in the accumulator. The overflow bound is fine (250 ms × 120 = 3×10¹⁰, well inside int64).

**Simulation time is the tick index, never a float.** Derive seconds as `tick / 120.0` when needed. A float `time += dt` loses resolution after hours and drifts relative to the tick count.

**Integration must not depend on the tick rate.** Semi-implicit Euler (`v += g*dt; x += v*dt`) adds a spurious `½·g·dt·t` term to the position, so jump apex height changes with tick rate. For constant acceleration use the exact form:

```
x += v*dt + 0.5f*a*dt*dt;   v += a*dt;
```

Then jump arcs match the analytic parabola at any tick rate. Define all tuning in px/s and px/s², not px/tick.

### 1.3 Tick rate: 60 vs 120

- **60 Hz** is the platformer tradition (Celeste). It is simple and cheap, and designers think in 60 Hz frames.
- **120 Hz** halves input quantization (≤8.3 ms instead of ≤16.7 ms). It reduces per-step displacement, which helps with fast dashes and projectiles and tunneling. Rocket League's physics also runs at 120 Hz, as I recall.
- 120 is a good highly-composite choice. It is an integer multiple of 60 and 30, and 240 Hz displays get an exact 2:1 ratio. 144 Hz displays don't divide evenly, which is why interpolation is mandatory.
- **Cost:** 2× sim CPU, and every authored duration is now in ticks. Keep designer-facing units in "60 Hz frames" and convert at compile time (`6_f60` becomes 12 ticks). Add `static_assert(kSimHz % 60 == 0)`.
- **Lock the rate.** Never make it configurable or variable. It defines your replays, your frame data and your feel.

I'd keep 120 Hz. Going higher buys little, since input arrives at the display or USB poll rate anyway, and it costs real CPU.

### 1.4 Pacing and presentation on 60–240 Hz and VRR

- **Interpolation is non-negotiable** with a 120 Hz sim. At 144 Hz the sim produces 0 or 1 ticks per frame in an irregular pattern, which judders without interpolation. At 240 Hz you render two frames per tick.
- **Cost:** the display shows state up to one tick (8.3 ms) behind the sim. Extrapolation avoids that but pops at collisions. Interpolate everything; if input latency becomes the concern, attack it by polling input as late as possible in the frame, not by extrapolating.
- **If vsync is on, vsync is the pacer.** Don't also sleep, or you double-wait and add latency. Only run the sleep-and-spin pacer when vsync is off or you want a cap.
- **VRR (G-Sync/FreeSync):** frame times are irregular by design. The accumulator absorbs that, so no special code is needed. If you cap, cap a few Hz *below* the panel maximum so you stay inside the VRR range.
- **Pacer implementation:** sleep coarsely, then spin for the last stretch. SDL3's `SDL_DelayPrecise` (since 3.2.0) does this for you: it tries to hit the requested time and may busy-wait SDL_DelayPrecise waits as close to the requested time as it can, busy-waiting if necessary, but can return late due to OS scheduling. A hand-rolled sleep-then-spin with an adaptive margin gives you control and measurability, and I show one below.
- **Render-side smoothing (optional, vsync-on only):** snap measured frame deltas to the nearest multiple of the refresh period when they're within a small tolerance. This removes timestamp noise that shows up as alpha jitter. Apply it to the value used for pacing/alpha only, never to the simulation accumulator, which must get true time.

### 1.5 Combat time: hitstop, slow-motion and time dilation

This is the part most engines get wrong. The key idea is to split time into **two layers**:

1. **Real ticks** advance at wall-clock rate, always. Input sampling, UI, audio, camera shake, particle/FX and all *time-effect timers* live here. This is what Celeste's Monocle engine does conceptually: it keeps a raw delta that ignores intentional freezes alongside a scaled delta that stops during them, plus a freeze timer and a time-rate scale Monocle exposes a raw delta that ignores freezes, a delta that stops during them, a freeze timer and a time-rate value.
2. **Sim ticks** are what gameplay runs on. A tick is gated per **time domain**.

**Time domains.** A small fixed set, for example `World`, `Player`, `Enemies`. Each domain has a Q16.16 scale and its own fractional accumulator:

```
each real tick:  d.acc += d.eff_scale_q16;  pending = d.acc >> 16;  d.acc &= 0xFFFF
```

- Scale `1.0` runs one sim tick per real tick. Scale `0` is a freeze (hitstop). Scale `0.25` runs a sim tick every fourth real tick.
- **The sim step stays constant.** You never feed a scaled dt into physics. You change *how often* the fixed step runs. That keeps physics stable, tunneling unchanged and the result deterministic, because it's pure integer logic.
- **Slow-mo stays smooth.** Interpolation alpha is computed per domain from its own fractional progress, so a 0.25× domain still renders smoothly at 144/240 Hz instead of stepping at 30 Hz.
- **Hitstop and slow-mo are the same mechanism.** A `TimeEffect{mask, scale, remaining_real_ticks}` is just a scale override with a countdown. Durations count in *real* ticks, so a freeze lasts a fixed wall-clock time regardless of dilation.
- **Overlap policy.** Same-scope effects take the max remaining time and never sum. Otherwise a shotgun or a multi-hit combo produces a one-second freeze. Also consider applying hitstop only on the first hit of a multi-hit move, and capping total freeze time per second.

**Input buffers and hitstop** (the classic bug is a buffered jump or attack expiring during the freeze, or a press during the freeze being lost):

- Sample input every **real** tick into an `InputSnapshot{held, pressed, released}`.
- Each domain keeps an **input latch** that ORs together pressed and released edges until that domain next runs a sim tick, then clears them.
- Buffer windows (jump buffer, coyote time, attack buffer) count in the **owner's domain ticks**, so a freeze pauses them instead of consuming them.
- Result: you can press jump during hitstop and it fires on the first tick after, and the buffer window isn't eaten by the freeze.

**Cross-domain trap.** Per-domain dilation breaks anything that couples entities across domains. A player riding a platform in a slower domain gets carried in chunks, so they slip relative to the interpolated platform. Rules: riders, grabbed enemies and projectiles-in-hand inherit their carrier's domain, or you restrict dilation to non-interacting groups. Start with global scope (one `World` domain) for hitstop and slow-mo. Add the per-entity domains (attacker plus victim only) once the rest is solid.

**Presentation during freeze.** Keep ticking on the real layer: camera shake, hit flashes, particles, UI, audio. Optionally pitch-shift audio during slow-mo. Animations and AI run on domain ticks, so hitboxes line up to exact tick indices.

### 1.6 Hardware and OS edge cases

- **Spiral of death.** Clamp the frame delta (about 250 ms) and cap ticks per frame (about 8). When the cap trips, *discard* the backlog (keep the fractional phase) and log it. Don't try to catch up.
- **Window minimize/focus loss/drag.** Pause the sim, mute or pause audio, and call `resync(now)` on resume so the accumulator is zero and `last_ns = now`. Win32 drag/resize modal loops can stall your main loop, so the clamp is your safety net there.
- **Debugger breakpoints and laptop sleep.** Same as above: huge dt gets clamped, then resync.
- **Sleep drift.** Always wait to an **absolute deadline** (`deadline += period`), never `sleep(period)`. If you miss a deadline by more than a period, re-anchor to `now + period` rather than "catching up" with a burst of fast frames.
- **Timer granularity on Windows.** The default granularity can be coarse, and the SDL3 community advice is that plain `SDL_Delay` is unreliable for loop pacing on Windows, which is why `SDL_DelayNS`/`SDL_DelayPrecise` exist A community discussion on the SDL forum advises against using SDL_Delay to pace a main loop on Windows because it is very inaccurate, pointing to the nanosecond delay functions in SDL3 instead. Use the nanosecond APIs plus your own spin tail.
- **Clock choice.** Use a monotonic clock (SDL's ticks or performance counter). Never use raw `rdtsc` (core migration, non-invariant TSC) or wall-clock time.
- **Refresh-rate changes** (moving the window between monitors, VRR toggles, alt-tab): re-query the display mode and re-evaluate the pacer.

---

## 2. Architecture and data flow

### Single source of truth

**The sim tick counter is the only simulation time.** Wall-clock time touches exactly one place, `TimeLoop::advance`, which converts elapsed nanoseconds into an integer number of real ticks. After that point, nothing in gameplay, physics or AI ever sees a wall-clock value or a variable dt.

### Data flow

```
OS monotonic counter  (SDL_GetTicksNS / QPC)
        │  now_ns : int64
        ▼
┌─────────────────────────── Application::frame() ───────────────────────────┐
│ 1. pump OS events ─► raw, timestamped input events; focus/minimize events   │
│        └─► on focus lost: loop.set_paused(true)   on regain: loop.resync()  │
│ 2. TimeLoop.advance(now_ns)                                                 │
│        clamp dt → acc += dt*Hz → N REAL ticks, alpha_real, dropped flag     │
│        for each REAL tick:                                                  │
│          a. InputSystem.sample()  → InputSnapshot[t]; latch edges per domain│
│          b. TimeDomains.begin_real_tick()                                   │
│               effects: scale override, age countdowns (real ticks)          │
│               per domain: acc += eff_scale → pending sim ticks              │
│          c. for each domain (fixed order), for each pending tick:           │
│               World.step(domain, sim_tick, latched input, kSimDt)           │
│               (entities push prev←curr, curr←new into Interp buffers)       │
│          d. cross-domain resolve: hits → events → TimeDomains.request(...)  │
│ 3. build RenderSnapshot: per entity lerp(prev, curr, alpha(domain))         │
│ 4. Renderer.draw(snapshot) → present  (vsync blocks here, or…)              │
│ 5. FramePacer.wait()   (no-op when vsync drives pacing)                     │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Components and what each owns

| Component | Owns | Must not know about |
|---|---|---|
| `Clock` | `now_ns()`, `sleep_ns()`, timer-resolution setup. Injectable for tests | Anything gameplay |
| `FramePacer` | Target period, absolute deadline, adaptive spin margin | The sim, vsync state beyond "enabled or not" |
| `TimeLoop` | `last_ns`, accumulator (ns×Hz), real-tick counter, clamps, pause flag, drop stats | Domains, input, rendering |
| `TimeDomains` | Per-domain scale/accumulator/pending/sim-tick; fixed pool of `TimeEffect`s; stacking policy | Wall-clock time, input devices |
| `InputSystem` | Real-tick snapshots, per-domain edge latches, replay ring | Domains' scale, hitstop rules |
| `InterpBuffer<T>` | `prev`/`curr` per interpolated value; `push` and `snap` | Who calls it or when ticks happen |
| `World`/sim | Gameplay state, advances by domain tick | Wall clock, alpha, render rate |
| `Renderer` | Nothing about time. It receives a snapshot and alpha values | The sim |

Existing pieces map onto this directly: `TimeLoop`, `HitstopEngine` (becomes `TimeDomains` plus effects), `FramePacer`, `InterpolationBuffer`. The structural change worth making is folding hitstop into the same mechanism as slow-mo, and splitting real ticks from sim ticks.

---

## 3. Traps and how robust engines avoid them

| Trap | Symptom | Fix |
|---|---|---|
| Float accumulator or `time += dt` | Drift, precision loss after hours, tick count ≠ wall time | Integer ns×Hz accumulator; sim time = tick index |
| Truncating the step to integer ns | ~40 ns/s of drift | Accumulate in ns×Hz units, step cost = 10⁹ |
| No frame-delta clamp | Spiral of death after a stall or breakpoint | Clamp dt, cap ticks/frame, discard backlog, log it |
| Semi-implicit Euler with tuned constants | Jump height changes with tick rate | `x += v·dt + ½a·dt²`; tune in px/s |
| Using render dt in gameplay (camera lerp, timers, "just this one thing") | Frame-rate-dependent feel, broken replays | Gameplay reads only sim tick/`kSimDt`; render-only effects use render dt |
| Interpolating across a teleport/respawn/room change | One-frame smear across the screen | `snap()` sets prev=curr; flag on any discontinuity |
| Interpolating angles/wrapped values naively | Spinning the long way round | Interpolate shortest arc; snap on wrap |
| Hitstop implemented as scaled dt | Physics changes under freeze; tunneling; input timers eat the freeze | Gate ticks, don't scale the step |
| Buffers counted in real time | Buffered jump expires during freeze | Count in owner's domain ticks; latch edges |
| Freeze durations that stack | One-second freezes on multi-hit attacks | Max-remaining policy, per-second budget |
| Sleeping *and* vsyncing | Added input latency, jitter | One pacer at a time |
| Relative `sleep(period)` | Cumulative drift | Absolute deadlines, re-anchor when late |
| Not resyncing after resume | Burst of ticks (or one giant clamped one) on alt-tab | `resync(now)` on focus regain/unpause/load |
| Reading sim state in the renderer mid-tick | Tearing between entities | Render only from interpolation buffers/snapshot |
| Update-order dependence | Desyncs, nondeterminism | Fixed domain order, stable entity iteration, no unordered containers in sim |
| FMA contraction and fast-math (GCC) | Different float results across builds | Build with `-ffp-contract=off`, no `-ffast-math`; avoid libm transcendentals in sim-critical paths or accept same-binary-only determinism |
| Animation hit frames by time | Hitboxes drift from visuals under dilation | Frame data in domain ticks |
| Cross-domain coupling (platform riders) | Slip/jitter | Riders join carrier's domain |

---

## 4. Idiomatic C++20 reference skeleton

Design points: no heap, value types, `noexcept` throughout (matches a no-exceptions policy), time injected as plain integers so the loop is trivially testable with a fake clock, and a template callback instead of `std::function` to avoid allocation.

```cpp
// time_core.h
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace en {

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

inline constexpr i64   kSimHz    = 120;
inline constexpr i64   kNsPerSec = 1'000'000'000;
inline constexpr float kSimDt    = 1.0f / float(kSimHz);
static_assert(kSimHz % 60 == 0, "keep 60 Hz authoring units exact");

struct Ticks { u32 v; };
consteval Ticks operator""_f60(unsigned long long f) {         // designer units: 60 Hz frames
    return Ticks{ u32(f * (kSimHz / 60)) };
}

inline constexpr u32 kQ16One = 1u << 16;
constexpr u32 q16(float s) noexcept { return u32(s * float(kQ16One) + 0.5f); }

// ───────────────────────────── TimeLoop ─────────────────────────────
struct LoopConfig {
    i64 max_frame_ns        = 250'000'000;
    u32 max_ticks_per_frame = 8;
};

struct FrameTiming {
    u32   real_ticks   = 0;
    float alpha        = 0.f;     // leftover inside the next real tick, [0,1)
    bool  dropped_time = false;   // spiral guard or dt clamp fired
};

class TimeLoop {
public:
    constexpr explicit TimeLoop(LoopConfig c = {}) noexcept : cfg_(c) {}

    void resync(i64 now_ns) noexcept { last_ns_ = now_ns; acc_ = 0; }
    void set_paused(bool p)  noexcept { paused_ = p; }
    u64  real_tick() const   noexcept { return real_tick_; }

    template <class OnTick>                 // void(u64 real_tick_index)
    FrameTiming advance(i64 now_ns, OnTick&& on_tick) noexcept {
        FrameTiming ft{};
        i64 dt = now_ns - last_ns_;
        last_ns_ = now_ns;

        if (paused_ || dt <= 0) { ft.alpha = alpha(); return ft; }
        if (dt > cfg_.max_frame_ns) { dt = cfg_.max_frame_ns; ft.dropped_time = true; }

        acc_ += dt * kSimHz;                // ns × Hz: one tick costs exactly 1e9
        while (acc_ >= kNsPerSec) {
            if (ft.real_ticks == cfg_.max_ticks_per_frame) {
                acc_ %= kNsPerSec;          // drop whole backlog, keep phase
                ft.dropped_time = true;
                break;
            }
            acc_ -= kNsPerSec;
            on_tick(real_tick_++);
            ++ft.real_ticks;
        }
        ft.alpha = alpha();
        return ft;
    }

private:
    float alpha() const noexcept { return float(double(acc_) / double(kNsPerSec)); }

    LoopConfig cfg_;
    i64  last_ns_   = 0;
    i64  acc_       = 0;
    u64  real_tick_ = 0;
    bool paused_    = false;
};

// ─────────────────────── Time domains + effects ───────────────────────
enum DomainId : u32 { kWorld, kPlayer, kEnemies, kDomainCount };
using DomainMask = u32;

struct TimeEffect {
    DomainMask mask      = 0;
    u32        scale_q16 = 0;   // 0 = freeze (hitstop)
    u32        remaining = 0;   // REAL ticks; unaffected by its own dilation
};

struct TimeDomain {
    u32 acc     = 0;            // Q16 progress toward next domain tick
    u32 eff     = kQ16One;      // effective scale this real tick
    u32 pending = 0;            // domain ticks to run this real tick
    u64 sim_tick = 0;           // domain-local tick count (anim/AI/buffers read this)
};

class TimeDomains {
public:
    // Same scope+scale: extend to the max, never sum. Returns false if pool is full.
    bool request(TimeEffect e) noexcept {
        for (auto& s : fx_)
            if (s.remaining && s.mask == e.mask && s.scale_q16 == e.scale_q16) {
                s.remaining = std::max(s.remaining, e.remaining);
                return true;
            }
        for (auto& s : fx_)
            if (!s.remaining) { s = e; return true; }
        return false;
    }

    void begin_real_tick() noexcept {
        for (auto& d : dom_) d.eff = kQ16One;
        for (auto& e : fx_) {
            if (!e.remaining) continue;
            for (u32 i = 0; i < kDomainCount; ++i)
                if (e.mask & (1u << i)) dom_[i].eff = std::min(dom_[i].eff, e.scale_q16);
            --e.remaining;
        }
        for (auto& d : dom_) {
            d.acc    += d.eff;
            d.pending = d.acc >> 16;          // eff <= 1.0 ⇒ pending ∈ {0,1}
            d.acc    &= 0xFFFFu;
        }
    }

    u32  pending(DomainId i)  const noexcept { return dom_[i].pending; }
    u64& sim_tick(DomainId i)       noexcept { return dom_[i].sim_tick; }

    // Interpolation progress for entities in domain i.
    // May saturate for < 1 real tick when eff doesn't divide evenly; invisible at 120 Hz.
    float alpha(DomainId i, float real_alpha) const noexcept {
        const auto& d = dom_[i];
        const float p = (float(d.acc) + real_alpha * float(d.eff)) / float(kQ16One);
        return std::min(p, 1.0f);
    }

private:
    std::array<TimeDomain, kDomainCount> dom_{};
    std::array<TimeEffect, 16>           fx_{};
};

// ───────────────────────────── Input latch ─────────────────────────────
struct InputSnapshot { u32 held = 0, pressed = 0, released = 0; };   // one per REAL tick

struct InputLatch {                       // one per domain
    u32 held = 0, pressed = 0, released = 0;
    void accumulate(const InputSnapshot& s) noexcept {
        held = s.held; pressed |= s.pressed; released |= s.released;
    }
    void consume() noexcept { pressed = released = 0; }
};

// ─────────────────────────── Interpolation ───────────────────────────
template <class T>
struct Interp {
    T prev{}, curr{};
    void push(const T& v) noexcept { prev = curr; curr = v; }   // end of each domain tick
    void snap(const T& v) noexcept { prev = curr = v; }         // teleport / spawn / room change
};

}  // namespace en
```

```cpp
// frame_pacer.h — only used when vsync is off or a cap is requested
class FramePacer {
public:
    void configure(i64 period_ns) noexcept { period_ = period_ns; deadline_ = 0; }

    void wait() noexcept {                          // no-op when period_ == 0 (vsync-driven)
        if (!period_) return;
        i64 now = Clock::now_ns();
        if (!deadline_) deadline_ = now + period_;
        for (;;) {
            const i64 remain = deadline_ - Clock::now_ns();
            if (remain <= 0) break;
            if (remain > margin_) {
                const i64 target = Clock::now_ns() + (remain - margin_);
                Clock::sleep_ns(remain - margin_);               // coarse (e.g. SDL_DelayNS)
                const i64 late = Clock::now_ns() - target;       // how late did we wake?
                margin_ = std::clamp(std::max(margin_ - margin_ / 64, late * 3 / 2),
                                     i64{500'000}, i64{3'000'000});   // peak-hold, slow decay
            } else {
                cpu_relax();                                     // spin tail
            }
        }
        deadline_ += period_;
        now = Clock::now_ns();
        if (now > deadline_) deadline_ = now + period_;          // missed: re-anchor, don't burst
    }

private:
    i64 period_ = 0, deadline_ = 0, margin_ = 1'000'000;
};
```

```cpp
// main loop
int Application::run() noexcept {
    Clock::init_high_res_timer();
    loop_.resync(Clock::now_ns());

    while (running_) {
        pump_events();    // focus lost → loop_.set_paused(true); regained → set_paused(false) + resync(now)

        const i64 now = Clock::now_ns();
        const FrameTiming ft = loop_.advance(now, [&](u64 /*real_tick*/) {
            const InputSnapshot snap = input_.sample();               // every REAL tick
            for (auto& l : latches_) l.accumulate(snap);

            domains_.begin_real_tick();
            for (u32 d = 0; d < kDomainCount; ++d) {                  // fixed order = determinism
                for (u32 n = domains_.pending(DomainId(d)); n--; ) {
                    world_.step(DomainId(d), domains_.sim_tick(DomainId(d))++, latches_[d], kSimDt);
                    latches_[d].consume();
                }
            }
            world_.resolve_cross_domain(domains_);                    // hits → domains_.request(...)
        });

        renderer_.draw(world_, domains_, ft.alpha);   // lerp(prev, curr, domains_.alpha(dom, ft.alpha))
        renderer_.present();                          // vsync blocks here if enabled
        pacer_.wait();                                // no-op under vsync
        telemetry_.record(ft);                        // dropped_time, ticks/frame, frame ms
    }
    return 0;
}
```

**Testing hooks that fall out of this design:**
- Feed `advance()` random-but-reproducible `now_ns` sequences and assert the sim state hash after N ticks is identical across them.
- Assert `total_ticks == floor(total_ns × Hz / 10⁹)` over a simulated 24-hour run (zero drift).
- Hash sim state per tick in debug builds for replay/desync detection.

---

## 5. Decisions you should make deliberately

1. **Tick rate.** I recommend 120 Hz fixed, never configurable. The alternative is 60 Hz for cheaper sim and Celeste-style authoring.
2. **Hitstop scope.** Start global (one domain). Go per-entity only when your design needs attacker and victim to freeze while everything else keeps moving, and take the rider/carrier rule with it.
3. **Determinism target.** Same-binary replays (flags and discipline are enough) versus cross-platform (needs fixed-point or integer-pixel sim-critical state, the way Celeste resolves collision on integer pixels with a sub-pixel remainder). Pick before you write physics.
4. **Interpolation vs latency.** Accept up to 8.3 ms of display lag from interpolation; fight latency with late input polling, not extrapolation.
5. **Threading.** Keep sim and render on one thread first. When you split them, replace the direct snapshot with a triple buffer; the time architecture doesn't change.

If you paste your current `TimeLoop`, `HitstopEngine`, `FramePacer` and `InterpolationBuffer` headers, I'll go through them against this list and give you itemized, concrete changes.
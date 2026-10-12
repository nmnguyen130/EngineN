# EngineN — Runtime Architecture Audit & Redesign

**Scope:** TimeLoop → Input → Simulation → Interpolation → Assets → Render Extraction → Batching → GPU Submission → Presentation
**Subject:** `enginen_pipeline_bundle.cpp` (2,044 lines: `TimeManager`, `AssetTypes/Table/Reader/Manager`, `RendererTypes`, `QuadInstance`, `SortKey`, `FrameArena`, `Batcher`, `RenderDevice`, `main.cpp`)
**Date of analysis:** 2026-10-12

---

## 0. Executive Summary

**What the code actually is.** The renderer backend is **`SDL_Renderer` + `SDL_RenderGeometry`** (`RenderDevice.cpp:1439, 1625`), not direct Direct3D 11. SDL may select its D3D11 driver on Windows, but the bundle only logs the driver name (`:1456`). The `FramePacket` / `Batcher` / `QuadInstance` front end was designed for a GPU-instanced backend that does not exist yet; the current backend expands every quad to four 32-byte vertices on the CPU.

**What is good and should be kept** (details in §1.6): the exact integer accumulator in `TimeManager` (`acc += dt_ns * Hz`, tick when `acc >= 1e9`), `BeginFrameManual` as a deterministic test seam, the stable LSD radix sort with constant-byte skipping, the safe-fallback `AssetReader::Resolve`, the 48-byte `QuadInstance` layout, and the `RenderBackend` seam.

**Highest-impact confirmed defects** (full registry in §1.4):

| # | Defect | Where |
|---|---|---|
| R1 | Per-batch blend mode is **ignored for every textured batch** (SDL uses the *texture's* blend mode, not the renderer draw blend mode). Demo hides it because only untextured quads use non-default blends. | `RenderDevice.cpp:1565-1576, 1625`; SDL `SDL_render.c:560-566` |
| R2 | Colors are packed **premultiplied** but blended with **straight-alpha** `SDL_BLENDMODE_BLEND` → alpha applied twice for `a < 1`. | `RendererTypes.h:843-850`, `QuadInstance.h:997`, `RenderDevice.cpp:1572-1575` |
| S1 | `prevPosition` is only snapshotted when `dt > 0`, so during hitstop `prev ≠ cur` and the sprite **oscillates between two positions every tick** (3.5–7.7 px sawtooth at default speeds). | `main.cpp:1913-1915` |
| S2 | Combat input edges, hitstop requests and time-scale changes mutate simulation state **outside the fixed-tick loop**; tick input is used only for axes. Not replayable, frame-rate dependent. | `main.cpp:1885-1909` |
| T1 | Hitstop is decremented by **render-frame** delta, not sim ticks → a 40 ms request freezes 8 / 6 / 5 ticks at 30 / 60 / 240 FPS. | `TimeManager.cpp:232-241, 313, 322-332` |
| R4 | Batch-cap overflow silently **merges the wrong instances** into the last stored batch. | `Batcher.cpp:1282-1305` |
| S4 | No view transform / virtual resolution: gameplay bounds and quad positions are raw window pixels; `ViewTransform2D` is defined but never consumed. | `main.cpp:1937-1939, 1955-1964`; `RendererTypes.h:926` |

**Headline design decisions** (rationale in §3):

1. **Keep** a strict 120 Hz fixed tick for gameplay; **count hitstop in ticks**; let cosmetic systems use render-time. Interpolation snapshots become **engine-owned and unconditional** (`Interpolated<T>`).
2. **Gameplay reads only tick-level input** (`CutTick`). Frame-level input is for UI/dev tools. The finalized `Ingest / PublishFrameInput / CutTick / Coalesce` contract is treated as **fixed**; the problem is the call site, not the contract.
3. **Premultiplied alpha everywhere**; additive via the alpha-0 trick, so alpha and additive sprites share one blend state and one batch.
4. **Drop CPU double-buffering of the arena** (no consumer exists on another thread). **Retain** zero-allocation only for Tick / Extract / Batch / Execute / Present, enforced by a test, not by convention.
5. **Keep generational handles** at the asset layer (cross-frame); pass **frame-scoped slot ids** to the backend.
6. **Do not replace the radix sort on faith** — keep the *stability* property, benchmark against `std::stable_sort`, decide on data (§5.5).
7. **Backend:** fix the `SDL_Renderer` backend first (cheap, removes confirmed bugs), then add a **D3D11 backend behind the existing `RenderBackend` seam**, justified by *features* (custom shaders, instancing, flip-model pacing control), not by an unmeasured speed claim.
8. **One `Presenter` owns** VSync / tearing / frame cap / waitable-swapchain wait; `FramePacer` moves out of `TimeManager`.

**What this document does not claim.** No speed-up figure is asserted anywhere; every performance statement is framed as a hypothesis with a measurement that would confirm or reject it (§5). Numeric thresholds in §5.6 are **proposed gates to be calibrated against the baseline captured in roadmap step R0**.

### Evidence legend

| Tag | Meaning |
|---|---|
| **CONFIRMED** | Demonstrated by reading the provided source (and, where stated, the SDL3 source). |
| **SUSPECTED** | Plausible from the code, but depends on a file or runtime behavior not in the bundle. |
| **MISSING** | The evidence needed to judge is not in the bundle. |
| **DESIGN** | Not a bug; a boundary/ownership weakness or a decision that should be revisited. |
| **FUTURE** | Not needed now; listed to avoid accidental over-engineering. |

Severity: **S1** visibly wrong / likely to bite now · **S2** latent or limited-scope · **S3** minor / hygiene.

> **SDL evidence caveat.** SDL claims below were checked against the `libsdl-org/SDL` **`main`** branch sources (`src/render/SDL_render.c`, `src/render/direct3d11/SDL_render_d3d11.c`, `src/timer/SDL_timer.c`, `include/SDL3/SDL_blendmode.h`, `SDL_render.h`) on the analysis date. Line numbers cited as `SDL_render.c:NNN` refer to that snapshot. **Re-verify against the SDL version you pin.**

---

## 1. Current Architecture Audit

### 1.1 Files reviewed and files missing

**Reviewed (in bundle):** `core/time/TimeManager.{h,cpp}` (1-392), `asset/AssetTypes.h` (395-483), `AssetTable.h` (486-679), `AssetReader.h` (682-757), `AssetManager.h` (760-816), `renderer/RendererTypes.h` (819-939), `QuadInstance.h` (942-1003), `SortKey.h` (1006-1050), `FrameArena.h` (1053-1140), `Batcher.{h,cpp}` (1143-1349), `RenderDevice.{h,cpp}` (1352-1637), `main.cpp` (1640-2043).

**Not in the bundle (findings that depend on them are tagged SUSPECTED / MISSING):** `core/Types.h`, `core/Assert.h`, `core/Result.h`, `FramePacket.h` (`DrawBatch`, `ViewSlotDesc`, `RenderPassDesc`, `FrameStats` layouts), `FrameArena.cpp` (`Push*` bodies), `AssetManager.cpp`, `TextureAsset.h`, `AtlasAsset.h`, `ITextureDevice.h`, `RenderBackend.h`, `platform/*` (`Platform`, `Window`, `EventRouting`), `InputSystem`, `math/Vec2.h`.

### 1.2 Actual call flow (traced from `main.cpp:1862-2037`)

```
main()
 ├─ Platform::Init → Window::Create(highDPI=true) → RenderDevice::Init  (SDL_CreateRenderer, nullptr driver)   :1788-1811
 ├─ EnsureDemoTexturesExist → AssetManager::Create(textureCapacity=128, device)                                  :1812-1818
 ├─ LoadTexture(id, path) ; CreateAtlasFromGrid                                                                  :1819-1828
 ├─ TimeManager::Init(120 Hz, maxCatchup=8)                                                                      :1844-1849
 └─ while (running)                                                                                              :1862
     1. time.BeginFrame()          QPC (SDL_GetTicksNS) → acc += dt_ns*Hz ; hitstop.Update(real dt)             :1863 → TimeManager.cpp:299-321
     2. platform.Pump(window,input) events → InputSystem                                                         :1865
     3. assets.Pump()                                                                                            :1866
     4. frameInput = input.PublishFrameInput()                                                                   :1867
     5. render-frame mutation: quit, F11, hotkeys, hitstop request, time-scale, *isAttacking/isDashing*          :1868-1909   ← S2
     6. while (time.ConsumeTick())                                                                               :1910
          tickInput = input.CutTick() ; dt = PlayerTime().GetScaledDt()
          if (dt > 0) { prev = pos; integrate; clamp(config.windowWidth…) }                                      :1911-1942   ← S1, S4
          time.EndTick()
     7. time.EndFrame() ; alpha = GetAlpha()                                                                     :1945-1946
     8. slot = arena.CurrentSlot(); slot.Reset(); PushPass; PushViewSlot                                         :1947-1956
     9. extraction inline in main: Lerp(prev,pos,alpha) → QuadInstance → PushQuad(key)                           :1957-2016
    10. batcher.Finalize(slot, packet)   SortEntries(radix) → LinearizeInstances(2 memcpys) → CoalesceBatches   :2017 → Batcher.cpp:1315-1348
    11. device.Submit(packet)            clear → per batch: CPU-expand quads → SDL_RenderGeometry → SDL_RenderPresent   :2018 → RenderDevice.cpp:1547-1636
    12. arena.AdvanceSlot()                                                                                      :2019
    13. telemetry (1×/s)                                                                                         :2021-2035
    14. time.Pacer().WaitForTarget()   (sleep + 1 ms spin)                                                       :2036
 shutdown: assets.Shutdown → device.Shutdown → window.Destroy → platform.Shutdown                                :2038-2041
```

### 1.3 Responsibilities and dependencies (as built)

| Component | Actual responsibility | Depends on | Boundary assessment |
|---|---|---|---|
| `TimeManager` | Clock sampling, accumulator, per-domain dt, hitstop, **frame pacer**, pause | SDL timer | Mixes simulation time with presentation pacing (`FramePacer`, `.h:151`). |
| `HitstopEngine` | Freeze timers per channel | — | Counts **real seconds per render frame** — wrong unit for a tick-based sim. |
| `AssetTable<T>` | Slot allocation, generations, open-addressing id→slot map, hot/cold SoA | `core/Assert.h` | Cohesive. Contains lifetime hazards (§1.4 A1-A2). |
| `AssetReader` | Branchless handle→hot-data resolve with fallback slot 0 | `AssetTable` | Good. Holds raw pointers into `AssetManager` members. |
| `AssetManager` | Load/dedup/unload, atlas creation, owns tables and frame store | `ITextureDevice` | `LoadTexture(AssetId, const char* path)` takes two independently-maintained keys. |
| `FrameArena(Slot)` | Per-frame instance/sort/batch/view/pass storage | — | Two slots, used one at a time on one thread. |
| `Batcher` | Sort → linearize → coalesce | `FrameArenaSlot` | Mutates the recording slot in place; two copies of instance data. |
| `RenderDevice` | `SDL_Renderer` wrapper **and** `ITextureDevice` **and** `RenderBackend` | SDL | Fuses texture factory, draw, and **present** in one class/one call. |
| `main.cpp` | Driver **and** gameplay **and** extraction **and** telemetry | everything | No engine-owned frame driver; gameplay logic is in the loop body. |

### 1.4 Findings registry

Format: **ID · Tag · Severity · Location → Evidence → Impact → Fix direction.**

#### Timing and simulation

| ID | Tag / Sev | Location | Finding |
|---|---|---|---|
| **T1** | CONFIRMED · S2 | `TimeManager.cpp:232-241, 313, 322-332, 342` | `HitstopEngine::Update(realDtSec)` runs once per **render frame** (`BeginFrameManual`, `:313`) while the freeze modifier is evaluated per **tick** (`UpdateDomainDeltas`, `:342`). Frozen tick count therefore depends on frame rate. Trace of a 40 ms request (4.8 ticks at 120 Hz), requested after `BeginFrame`: **30 FPS → 8 ticks, 60 FPS → 6 ticks, ≥120 FPS → 5 ticks.** Also breaks replay determinism. Fix: count hitstop in ticks (`_f60` literal already exists, `.h:18`). Reproduced on a reduced model of the `TimeManager` logic (30 FPS → 8, 60 FPS → 6, 120/144/240 FPS → 5 frozen ticks, independent of phase); confirm in-tree with test T-TIME-03. |
| **T2** | CONFIRMED · S3 | `TimeManager.h:74-75, 126-127`; `.cpp:223, 326-328, 383-391` | `m_accQ16` / `m_effScaleQ16` / `GetDomainAlpha` have **no consumer in the bundle** (only declaration + definition; `main.cpp` uses `GetAlpha()`, `:1946`). Their semantics (progress of a domain that advances only on some ticks) contradict how dt is actually applied (`PrepareTick`, `:217-219`: scaled dt **every** tick). A future caller would get a wrong interpolation factor under time-scale ≠ 1. Fix: delete after a repo-wide grep. |
| **T3** | CONFIRMED · S3 | `TimeManager.h:113`, `.cpp:283, 315, 337-341` | Two catch-up limits: `maxFrameNs` (default 250 ms = 30 ticks) and `maxCatchupTicks` (8 ticks = 66.7 ms). At default config the tick cap always fires first; `maxFrameNs` is redundant. Keep **one** policy (tick cap). |
| **T4** | CONFIRMED · S3 | `TimeManager.cpp:212-216` | `Real` channel's `m_simTicks` is incremented per **render frame**; `GetSimTicks()` on the Real channel is a frame counter. Rename or remove. |
| **T5** | CONFIRMED · S3 | `TimeManager.h:13-20, 110`; `.cpp:279` | `_f60` and the `% 60` `static_assert` use `kDefaultSimHz`, but `Init` accepts any positive Hz. With a configured 100 Hz the literal silently returns wrong tick counts. Fix: make `_f60`'s consumer take Hz from the scheduler, or assert at `Init`. |
| **T6** | SUSPECTED · S2 | grep: `SetPaused`, `ResetClocksOnRefocus` appear only in `TimeManager` | Not called anywhere in the bundle. If `Platform::Pump` does not call them, a focus loss/resume feeds one large delta; the tick cap bounds the damage (8 ticks) but hitstop/pacer state is not reset. **MISSING:** `Platform` source. |
| **T7** | CONFIRMED · S3 | `TimeManager.cpp:253-276`; SDL `SDL_timer.c:686-756` | `FramePacer` does `SDL_DelayPrecise(until deadline − 1 ms)` then spins 1 ms. `SDL_DelayPrecise` already sleeps in 1 ms slices, tracks max overshoot, and spins only the tail. The extra fixed 1 ms spin is a per-frame busy-wait whose benefit is unmeasured. Pacing is a **presentation** concern but lives in `TimeManager` (`:151`). |
| **T8** | SUSPECTED · S2 | `TimeManager.cpp:299-321` | dt is measured at loop top (`BeginFrame`), after the previous frame's work **and** `Present` blocking. On a VSync display the measured interval jitters around the true scan-out interval; interpolation alpha inherits that error → judder. **MISSING:** no frame-interval data exists. Collect first (§5.4); do not add smoothing speculatively. |

#### Input, simulation boundary, interpolation

| ID | Tag / Sev | Location | Finding |
|---|---|---|---|
| **S1** | CONFIRMED · S1 | `main.cpp:1913-1915` | `player.prevPosition = player.position` lives **inside** `if (dt > 0.0)`. During hitstop `PrepareTick` yields `dt = 0` (`TimeManager.cpp:218`), so prev/cur keep the values from the last moving tick, but `alpha` keeps sweeping 0→1 every 8.33 ms. Rendered `Lerp(prev, cur, alpha)` oscillates between two positions (≈ `420/120 = 3.5 px`, `7.7 px` while dashing). Fix: snapshot **unconditionally** at tick start, owned by the engine (`Interpolated<T>`). Test T-INT-01. |
| **S2** | CONFIRMED · S1 | `main.cpp:1885-1891, 1901-1909, 1911-1916` | `frameInput.Pressed(Attack/Dash/Sprint)` sets `isAttacking/isDashing/actionTimer` and requests hitstop from the **render-frame** section; the tick loop reads only `tickInput.Axis(0/1)`. Consequences: (a) recorded tick inputs do not reproduce attacks/dashes; (b) state change timing is quantized to render frames; (c) jump/attack buffering through `CutTick` cannot work for these actions. Time-scale toggling is also done at frame level. |
| **S3** | CONFIRMED · S3 | `main.cpp:1984` | Run-cycle frame = `GetSimTickCount()/15 % 4` (global ticks, not `PlayerTime`). Animation keeps advancing during player hitstop and does not follow player time-scale. Animation time must be per-entity, in the entity's channel. |
| **S4** | CONFIRMED · S2 | `main.cpp:1937-1939, 1955-1964, 1871-1876`; `RendererTypes.h:926-938` | Movement clamps, background quad and view slot use `config.windowWidth/Height`; F11 changes the window but not the world. `ViewTransform2D` is defined and never referenced elsewhere (grep). No virtual-resolution scaling, letterboxing or DPI handling exists in `RenderDevice::Submit`. |
| **S5** | SUSPECTED · S2 | `main.cpp:1793-1799`, `RenderDevice.cpp:1439`; no `SDL_SetRenderLogicalPresentation`/`SDL_SetRenderScale` in bundle | `highDPI = true` + coordinates passed through as pixels ⇒ on a scaled display the 1280×720 scene likely occupies only part of the window. Needs a run on a 150 % display (T-REND-04). **MISSING:** `Window::Create`. |
| **S6** | DESIGN · S3 | `main.cpp:1988` | Presentation polls live sim state (`Hitstop().IsFrozen`) at render time. Works for one flash; does not scale to hit-sparks, shake, audio cues. Prefer sim→presentation **events** emitted at tick time. |

#### Assets and handles

| ID | Tag / Sev | Location | Finding |
|---|---|---|---|
| **A1** | SUSPECTED · S2 | `AssetTable.h:551-556, 566` | `SetReady(slot, hot, cold)` assigns the **entire** `cold`, including `cold.id`, while `Free` calls `RemoveLookup(m_cold[slot].id)`. If a loader passes a `cold` whose `id` differs from the one given to `Allocate`, `Free` removes the wrong key (or `RemoveLookup(AssetId{0})` returns early) and leaves a **stale id→slot entry**; after slot reuse, `Find(oldId)` returns an unrelated asset. **MISSING:** `AssetManager.cpp`. Fix regardless: `SetReady` preserves `m_cold[slot].id` (assert equality). Test T-ASSET-03. |
| **A2** | CONFIRMED · S3 | `AssetTable.h:543, 570-576`; `AssetTypes.h:443-444` | Free list is LIFO and generations are 12-bit: the same slot is reused immediately and **retires permanently** after 4,095 free/alloc cycles (`m_gen = 0`, not returned to free list). Capacity decays silently with no stat/log. Realistic only for long hot-reload sessions; add a counter + log. (Retirement itself is the correct safety choice.) |
| **A3** | CONFIRMED · S2 | `AssetManager.h:796`; `main.cpp:1819`; `AssetTypes.h:411-419` | `LoadTexture(AssetId, const char* path)` takes the id and the path separately (call site uses `"textures/hero_sheet.tga"_aid` with path `"assets/textures/hero_sheet.tga"` — two different strings). Dedup is keyed by id, loading by path ⇒ same id with another path silently returns the first asset. `HashPath` only maps `\`→`/`; **no case folding, no `./`/`..`/`//` normalization** ⇒ on Windows `Hero.png` and `hero.png` become two ids and two GPU textures. 64-bit FNV-1a collisions are unchecked (astronomically unlikely, but silently aliasing). |
| **A4** | DESIGN · S2 | `AssetManager.h:799-803` | `Unload(handle)` frees regardless of other holders; every other copy silently degrades to the fallback. Safe, but ownership is implicit. For room streaming, scope-based lifetime (`AssetSet`) fits better than per-handle refcounts (§3.6). |
| **A5** | SUSPECTED · S3 | `AssetReader.h:754-755`; `AssetManager.h:792-793` | `AssetReader` stores pointers to `AssetManager` members; `AssetManager` is movable. A `Reader` captured before a move dangles. Today `Reader()` is called per frame after the move (`main.cpp:1977`) — safe by luck. Same shape: `AssetManager` holds `ITextureDevice*` into a **movable** `RenderDevice` (`RenderDevice.h:1373`). Make both non-movable after creation, or construct in place. |
| **A6** | MISSING | `AssetManager.h:812-813` | `m_frameStore` + `m_frameCursor` suggest a bump allocator for atlas frames (`maxFrames = 4096`). If `UnloadAtlas` does not reclaim, repeated room loads exhaust it. Needs `AssetManager.cpp`. |
| **A7** | CONFIRMED · S3 | `AssetTable.h:535-538, 503-520` | `Allocate` returns the existing slot even if its state is `Failed` (no retry path without `Unload`). `Init` does not assert `capacity < 2^kIndexBits` (20 bits). |
| **A8** | CONFIRMED · S3 | `AssetReader.h:726` | `Sprite()` silently clamps an out-of-range `frameIndex`; content errors become invisible. Add a debug-build counter/log. |
| **A9** | SUSPECTED · S2 | pipeline-wide | If textures are uploaded straight-alpha while color and blend assume premultiplied (R2), sprite edges show dark fringes. The demo TGA only has alpha 0/255 (`main.cpp:1688-1719`), which hides it. **MISSING:** `TextureAsset` loader. Test with a soft-edged sprite. |

#### Recording, sorting, batching

| ID | Tag / Sev | Location | Finding |
|---|---|---|---|
| **R1** | CONFIRMED · S1 | `RenderDevice.cpp:1565-1576, 1625-1632, 1475-1504`; SDL `SDL_render.c:560-566`, `SDL_render.h` (doc of `SDL_SetRenderDrawBlendMode`: "used for any drawing that doesn't involve textures"), `SDL_render.c:1671` | `Submit` calls `SDL_SetRenderDrawBlendMode` per batch, but `PrepQueueCmdDraw` takes `blendMode = texture->blendMode` whenever a texture is bound. `Create()` never calls `SDL_SetTextureBlendMode`; SDL defaults it to `BLEND` for alpha formats. **Every textured batch is drawn with straight-alpha BLEND regardless of `BlendMode`.** Demo: textured player = Premultiplied (correct by accident); additive trail/attack quads use texture slot 0 ⇒ untextured ⇒ renderer blend mode applies. A textured additive particle would render as normal alpha. |
| **R2** | CONFIRMED · S1 | `RendererTypes.h:843-850`, `QuadInstance.h:997-999`, `RenderDevice.cpp:1572-1575, 1596-1600`; SDL `SDL_blendmode.h:55-56` | `SetColor` packs **premultiplied** RGBA8; `Premultiplied` maps to `SDL_BLENDMODE_BLEND` = `dst = src.rgb·src.a + dst·(1−src.a)` (straight alpha). `SDL_BLENDMODE_BLEND_PREMULTIPLIED` exists and is the matching mode. For `a < 1` the color is multiplied by alpha twice (e.g. `a = 0.45` ⇒ weight 0.2 instead of 0.45). `SDL_BLENDMODE_ADD` (`dst += src.rgb·src.a`) has the same double factor (`main.cpp:1974, 2014`). |
| **R3** | CONFIRMED · S2 | `RenderDevice.cpp:1551-1634`; grep: `kindFlags`, `param`, `viewSlot` unused in `RenderDevice` | `Submit` ignores `QuadKind` (shapes, 9-slice, MSDF text), `QuadFlag_FlipX/Y`, `param[2]`, `batch.viewSlot`, `batch.passId`, `pipelineId`, and every pass field except `passes[0].clearColor`. `BlendMode::Multiply/Max` fall into `default:` (`:1573`) ⇒ `BLEND`. The instance format advertises features the only backend cannot execute. |
| **R4** | CONFIRMED · S2 | `Batcher.cpp:1282-1305`; `FrameArena.h:1068` | When `batchCount == kMaxBatches (4096)` a new batch is dropped but `currentOffset += count` still advances. A later entry whose state equals `batches[batchCount-1]` is **merged into that batch** (`prev.instanceCount += count`), extending it over instances that belong to the dropped batch — wrong state, wrong texture, and the intended instances are never drawn. No dropped-batch counter exists in `FrameStats`. Reachable only under extreme state thrash (>4,096 runs), but it is silent corruption. |
| **R5** | CONFIRMED · S3 | `Batcher.cpp:1251, 1259, 1258-1261, 1337` | When sorting happens, instances are copied **arena→scratch→arena** (two `memcpy` of `48·N` bytes). `LinearizeInstances` clamps the write count (`:1258`) but returns the **unclamped** `totalOutput`, which `Finalize` passes to `subspan(0, total)` (`:1337`); overlapping `SortEntry` ranges could make `total > instances.size()` ⇒ `subspan` precondition violation. Not reachable with well-formed producers. |
| **R6** | CONFIRMED · S3 | `FrameArena.h:1064-1139`; `main.cpp:1947, 2018-2019` | Two arena slots are allocated but `Submit` runs synchronously before `AdvanceSlot`, so slot N is never read while N+1 is written. Memory: instances `131,072 × 48 B = 6.0 MiB`, sort entries `65,536 × 16 B = 1.0 MiB` per slot (+ batches/views/passes) ⇒ ≥ 14 MiB for two slots, plus `Batcher` scratch `1 + 6 MiB`. Not a performance problem by itself (and `make_unique<T[]>` value-initializes, so it is touched at startup), but the second slot has **no consumer**. |
| **R7** | SUSPECTED · S3 | `FrameArena.h:1066-1067` | `kMaxSortEntries (65,536) < kMaxInstances (131,072)`. If `PushQuad` pushes one `SortEntry` per quad, the effective single-quad cap is 65,536, not 131,072. **MISSING:** `FrameArena.cpp`. |
| **R8** | CONFIRMED · S3 | `main.cpp:1966-2015, 2026-2032`; `Batcher.cpp:1340-1347` | All `PushQuad/PushPass/PushViewSlot` return values are discarded; `FrameStats` (incl. `droppedInstances`, `wasSorted`) is computed but never read; telemetry prints only `packet.instances.size()`. Overflow is invisible. |
| **R9** | DESIGN · S2 | `SortKey.h:1015-1031` | Key order is `pass · layer · depth(16) · blend · pipeline · texture`. `depth` outranks all state bits, so content with unique per-sprite depth (y-sorted actors) degrades batches toward one per sprite. Tilemaps/particles want `depth = 0` and rely on **stable** ordering. There is no per-layer sort policy; it is implicit in how callers fill `depth`. |
| **R10** | CONFIRMED · S3 | `RenderDevice.cpp:1578-1619, 1444-1446` | Per quad: 4 × `SDL_Vertex` (32 B) + 6 × `int` = **152 B** written (vs. 48 B instance), and SDL copies again into its own vertex buffer. `vector::resize` zero-fills growth and can allocate when a batch exceeds the 4,096-quad reserve ⇒ not strictly allocation-free after warm-up. Impact unmeasured. |
| **R11** | CONFIRMED · S3 | `SortKey.h:1021-1029`; `RenderDevice.cpp:1620-1624` | Only the texture **slot** survives into the sort key; generation is dropped and `m_textureSlots[texSlot]` is used unchecked. Safe today (record → submit in one frame, no unload between). If any asset commit moves between extraction and submit (async load, hot reload, threaded extraction), a recycled slot draws the wrong texture. `TextureHandle::White()` = `{1,1}` also relies on "first created texture is slot 1" (`RenderDevice.cpp:1448-1450`). |

#### Backend, presentation, measurement

| ID | Tag / Sev | Location | Finding |
|---|---|---|---|
| **P1** | CONFIRMED · S2 | `RenderDevice.cpp:1547-1636` | `Submit` = clear + draw + **`SDL_RenderPresent`** (`:1634`). No point to insert overlays, readback, a post pass, or to separate "GPU submission" from "presentation" for timing and pacing. |
| **P2** | CONFIRMED · S3 | `RenderDevice.cpp:1443`; `main.cpp:1763-1773, 1807`; `RendererTypes.h:909-913` | VSync result ignored (`(void)`); frame-rate policy is split across `RenderDeviceConfig.vsync`, `FramePacer`, and a `main.cpp` helper; `PresentMode {VSync, Immediate, Mailbox}` is defined and unused. |
| **P3** | CONFIRMED (SDL source) · info | SDL `SDL_render_d3d11.c:605-607, 682, 855-865, 957-970, 2951-2953` | If SDL picks D3D11, its swap chain is **flip-sequential, 2 buffers**, device-level `SetMaximumFrameLatency(1)`, **no waitable object**, tearing flag when the OS supports it (`Present1` with `ALLOW_TEARING` only when sync interval 0 and not exclusive fullscreen), and `SDL_SetRenderVSync(-1)` (adaptive) returns *unsupported*. `Mailbox` cannot be expressed through `SDL_Renderer`. |
| **P4** | CONFIRMED · S3 | `main.cpp:2021-2022` vs `RenderDevice.cpp:1634` | Telemetry's "CPU Work" is measured **after** `Submit`, which includes `SDL_RenderPresent` (VSync wait), and starts **after** `BeginFrame/Pump/assets.Pump`. It is neither CPU time nor frame time, and is a single sample per second, not a distribution. **Existing numbers must not be used as performance evidence.** |
| **P5** | SUSPECTED · S3 | `RenderDevice.h:1373`; `AssetManager.h:809` | See A5: moved `RenderDevice` ⇒ dangling `ITextureDevice*` in `AssetManager`. |
| **P6** | CONFIRMED · info | `main.cpp:2038-2041` + local declaration order | Shutdown order is correct: `assets` (declared after `device`) is destroyed/`Shutdown()` before `device`; `AssetManager::Shutdown` can still call `Destroy` on a live device. Keep this invariant explicit (§3.11). |

### 1.5 Worked traces for the highest-impact findings

**S1 — hitstop sawtooth.** State at tick *k* (moving, `v = 420 px/s`): `prev = x₍ₖ₋₁₎`, `cur = x₍ₖ₎`, `cur − prev = 3.5 px`. Hitstop starts. Tick *k+1*: `GetScaledDt() == 0` ⇒ body skipped ⇒ `prev`, `cur` unchanged, `EndTick()` still advances the accumulator. Render: `Lerp(x₍ₖ₋₁₎, x₍ₖ₎, α)` with α cycling 0→1 every 8.33 ms ⇒ the sprite travels 3.5 px back and forth ~120×/s for the whole freeze. A frozen entity must have `prev == cur`; that requires the snapshot at **every** tick start regardless of dt.

**R1/R2 — what the player sees.** `playerQuad` (`main.cpp:1997-2007`) is textured, `a = 1` ⇒ premultiplied = straight ⇒ looks correct (this is why the demo passes visual inspection). `trailQuad` (`a = 0.45`, additive, untextured, `:1974-1975`) is drawn with `ADD`: contribution `= premul_rgb · 0.45 = rgb · 0.45²` ⇒ ~0.2× instead of 0.45×.

**R4 — corruption sequence** (cap = 4096): entries `E₄₀₉₅` (stored), `E₄₀₉₆` (different state ⇒ dropped, `currentOffset` still advances), `E₄₀₉₇` (same state as `E₄₀₉₅` ⇒ `prev.instanceCount += count`). Batch 4095 now spans `[off₄₀₉₅, off₄₀₉₅ + n₄₀₉₅ + n₄₀₉₇)`, which covers `E₄₀₉₆`'s instances (wrong state) and omits `E₄₀₉₇`'s tail.

### 1.6 What is sound (keep)

| Item | Why it is correct / valuable | Where |
|---|---|---|
| Exact rational accumulator | `acc` is in `ns·Hz`; a tick is exactly `1e9`. No drift from truncating `1e9/120`; verified algebraically, test T-TIME-01 proves it over 10⁶ frames. | `TimeManager.cpp:319, 334, 350` |
| `BeginFrameManual(ns)` | Deterministic frame-time injection ⇒ unit tests without a real clock. | `.h:119` |
| Tick cap with remainder kept (`%=`) | Bounds work per frame, keeps sub-tick phase. | `.cpp:337-341` |
| `SDL_GetTicksNS` source | Backed by `SDL_GetPerformanceCounter` (QPC on Windows), monotonic — the `now >= prev` guard is harmless. | SDL `SDL_timer.c:640-648` |
| Stable LSD radix + monotonic early-out + constant-byte skipping | **Stability gives painter's-order tie-breaking for free** (equal keys keep submission order). Early-out makes pre-ordered tile layers free. | `Batcher.cpp:1172-1221` |
| Sort-key bit layout | Verified: `4+8+16+3+12+16+5 = 64` bits, shifts/masks consistent with the getters. | `SortKey.h:1015-1038` |
| `AssetReader::Resolve` | Branchless, never null, never out of bounds; stale/invalid handles degrade to the fallback slot. | `AssetReader.h:697-705` |
| Backward-shift deletion in `RemoveLookup` | Verified correct for linear probing (move iff ideal slot is cyclically outside `(i, j]`). Load factor ≤ 0.5. | `AssetTable.h:635-668` |
| Slot retirement on generation exhaustion | Right safety trade-off; just needs observability (A2). | `AssetTable.h:570-576` |
| `QuadInstance` (48 B, 16-byte aligned) | Maps 1:1 to a D3D11 per-instance vertex stream (offsets 0/8/16/24/32/36/40) — no CPU expansion needed on a real GPU backend. | `QuadInstance.h:966-1002` |
| `RenderBackend` seam, `FramePacket` as backend input | The correct place to add a D3D11 backend without touching gameplay. | `RenderDevice.h:1367-1379` |
| Shutdown order and `noexcept` / `Result<>` discipline | Consistent with the no-exceptions constraint. | `main.cpp:2038-2041` |

### 1.7 Design weaknesses (architectural, not line bugs)

1. **No engine-owned frame driver.** `main.cpp` interleaves platform, simulation, extraction and telemetry; every sample app will re-implement S1/S2-class mistakes. The tick/interp contract must be enforced by structure.
2. **Simulation, extraction and presentation share data directly** (`PlayerEntity` is read in render code, `Hitstop()` polled in render code).
3. **The instance format outran the backend** (R3): features in `QuadKind/QuadFlags/param` have no execution path, so they cannot be tested.
4. **Three owners of frame-rate policy** (P2) and one owner of pacing living in the wrong subsystem (T7).
5. **Asset identity is a path-hash** with two redundant keys (A3) and **lifetime by explicit Unload** (A4).
6. **Telemetry cannot attribute time** (P4): it does not separate extract / sort / backend CPU / Present wait.

### 1.8 Missing evidence that would change conclusions

| Needed | Would settle |
|---|---|
| `AssetManager.cpp`, `TextureAsset.h`, `AtlasAsset.h` | A1, A6, A9 (id handling in `SetReady`, atlas-frame reclaim, premultiply-on-load) |
| `FramePacket.h`, `FrameArena.cpp` | R4/R5 types (`DrawBatch::instanceCount` width), R7 |
| `Platform`/`Window`/`EventRouting`, `InputSystem` | T6, S5, tick-edge semantics for S2 (the audit treats the input contract as fixed and does not review its internals) |
| Reference-machine frame-interval capture | T8, any smoothing/pacing change |
| `SDL_GetRendererName` output on target machines | Whether the SDL backend is actually D3D11 today |

---

## 2. Target Architecture

### 2.1 Design constraints and assumptions

These are taken as **inputs** to the design. Each is re-examined in §3 where it is challenged.

| Constraint | Source | Status in this design |
|---|---|---|
| C++20, SDL3, MinGW-w64 GCC / CMake / Ninja | project toolchain | Fixed. Affects D3D11 shader tooling and sanitizer availability (§3.10, §5). |
| No exceptions; `Result<T, EngineError>` | project rule | Fixed. |
| 120 Hz fixed-tick simulation, variable-rate render, alpha interpolation | project rule | **Kept** for gameplay; scoped (§3.1). |
| Zero heap allocation on the per-frame hot path | project rule | **Kept, narrowed and test-enforced** (§3.7). |
| Input contract `Ingest / PublishFrameInput / CutTick / Coalesce` | finalized earlier | **Treated as frozen.** Not reviewed in this audit (source not in bundle); the redesign only changes who calls what, and when. |
| Renderer consumes `ViewTransform2D` + `alpha ∈ [0,1]` only | renderer spec | Kept; the camera is a gameplay/presentation concern. |
| Windows is the primary target; portability is not yet a stated requirement | inferred from toolchain | **Open question.** D3D11 is Windows-only; if Linux/macOS/Steam Deck are required, prefer `SDL_GPU` over D3D11 (§3.10). |

### 2.2 Lifecycle

```
PROCESS START
  Platform → Window → Backend(Device, Swapchain, Pipelines, Samplers, InstanceBuffer, Fallback textures)
          → AssetManager(tables, fallback slot 0) → TimeManager → Batcher(scratch) → FrameArena(1 slot)
          → Game::Init (world, AssetSets)                                      [allocation allowed: startup]
LEVEL / ROOM LOAD
  AssetSet::Load (sync now; async later) → handles resolved → World::Spawn      [allocation allowed: loading]
PER FRAME   (single thread; steady-state allocation = 0)
  0  Presenter.WaitForNextFrame()      block on waitable swapchain / software cap       ┐ frame boundary
  1  FrameClock.Sample()               QPC ns                                           │
  2  Platform.Pump → Input.Ingest      events                                           │
  3  AssetManager.Commit()             ← the ONLY point where asset slots mutate        │
  4  Input.PublishFrameInput()         render-rate view: UI, dev tools, window cmds     │
  5  TimeManager.BeginFrame(dt)        acc += dt·Hz ; real-time channel                 │
  6  while ConsumeTick():                                                               │
       tickInput = Input.CutTick()                                                      │
       Sim.BeginTick()                 snapshot ALL Interpolated<T> (unconditional)     │  SIMULATION
       Sim.Tick(tickInput, ctx)        gameplay @120 Hz; reads ctx.dt(channel)          │  (deterministic)
       Hitstop.AdvanceTick()           tick-counted                                     │
       TimeManager.EndTick()                                                            │
  7  TimeManager.EndFrame() → alpha                                                     │
  8  Presentation.Update(realDt, alpha, simEvents)   cosmetic: particles, shake, UI     │  PRESENTATION
  9  RenderQueue.Begin()  (arena.Reset)                                                 │  (non-deterministic OK)
     Game.Extract(queue, alpha, assets.Reader())     interpolate → QuadInstance         │
 10  Batcher.Finalize(arena, packet)     sort → gather → coalesce                       │  RENDER PREP
 11  Backend.Execute(packet)             upload → passes → draws  (no present)          │  GPU SUBMISSION
 12  Presenter.Present(policy)           Present1(sync, flags)                          │  PRESENTATION
 13  Profiler.EndFrame(stats)            per-stage ns, counts, dropped counters         ┘
SHUTDOWN (reverse of creation)
  Game → AssetManager(release all handles via backend) → Batcher/Arena → Backend(flush, ClearState, release, live-object report) → Window → Platform
```

Rules that make the lifecycle safe:

1. **Asset commit point (step 3).** No slot state, generation, atlas frame pointer, or GPU texture changes outside step 3 (and level-load, which runs *between* frames). Extraction (step 9) may therefore hold `const&` into asset tables for the whole frame.
2. **Frame boundary (step 0).** All per-frame arenas are reset at step 9 and are valid until step 12 of the same frame. Nothing keeps a pointer into them across frames.
3. **Tick boundary.** `Sim.BeginTick()` runs for every tick, including frozen (dt = 0) ticks — this is what makes `prev == cur` for frozen entities.
4. **One-way data flow.** Input → Sim → (events, state) → Presentation/Extract → Packet → Backend. Nothing downstream writes upstream; presentation never mutates sim state.

### 2.3 Data flow and ownership

```
 OS events ─► Platform ─► InputSystem ──CutTick()──► Simulation ──────────────┐
                             │                         │  owns: World, Interpolated<T>, Hitstop ticks
                             │ PublishFrameInput()     │  emits: SimEvents (fixed-capacity ring, frame lifetime)
                             ▼                         ▼
                        UI / DevTools           Presentation (owns: particles, shake, anim sampling, camera view)
                                                       │ interpolated state + alpha
                          AssetManager ──Reader()──►  Extract (game code, uses RenderQueue)
                           owns: tables,                │ QuadInstance + SortKey + ViewSlot + PassDesc
                           AssetSets, frame store       ▼
                                                   FrameArena (1 slot, reset each frame)
                                                        │
                                                   Batcher.Finalize  ─►  FramePacket {instances(sorted), batches, views, passes, stats}
                                                        │  (non-owning spans into Batcher/Arena memory)
                                                        ▼
                                                  RenderBackend.Execute   (owns: device, textures table, pipelines, buffers)
                                                        │
                                                  Presenter.Present        (owns: swapchain policy, pacer)
```

### 2.4 Subsystem boundaries

| Subsystem | Owns (lifetime) | Reads | Writes | Must **not** |
|---|---|---|---|---|
| **TimeManager** (clock, accumulator, channels, tick-counted hitstop) | accumulator, channel dt, hitstop ticks (process) | SDL clock | `alpha`, per-channel dt | own frame pacing; poll input; know about rendering |
| **Presenter** (+`FramePacer`) | swapchain policy, pacer deadline (process) | `PresentationPolicy` | present flags, pacing | influence simulation dt other than via the measured frame interval |
| **InputSystem** *(frozen contract)* | action buffers, bindings | platform events | `FrameInput`, `TickInput` | be consumed by gameplay at frame rate for combat |
| **Simulation** (game) | world state, `Interpolated<T>` (level) | `TickInput`, `TickContext` | state, `SimEvents` | read render-frame dt; touch GPU/asset tables except via handles |
| **Presentation** (game/engine) | particles, camera view, shake (level) | `SimEvents`, interpolated state, real dt | `RenderQueue` | mutate sim state |
| **AssetManager** | tables, id map, atlas frames (process); `AssetSet`s (level) | disk, `ITextureDevice` | slots at **commit/load only** | be mutated during extract/submit |
| **RenderQueue / FrameArena** | per-frame instance, sort, view, pass storage (frame) | — | arena | outlive the frame |
| **Batcher** | sort/gather scratch (process) | arena | `FramePacket` spans | call the backend; inspect textures |
| **RenderBackend** | device, resource tables, pipelines, instance buffer (process) | `FramePacket` | GPU | read gameplay data; mutate asset tables |
| **Profiler / FrameStats** | ring buffer (process) | stage timestamps | CSV/overlay | allocate per frame |

### 2.5 Resource lifetimes

| Tier | Contents | Created | Destroyed | Safety mechanism |
|---|---|---|---|---|
| **Process** | window, device, swapchain, shaders, blend/sampler/rasterizer states, instance buffer, constant buffers, fallback + white textures, asset tables, scratch | startup | shutdown (reverse order) | `Result` at creation; debug-layer live-object report at exit |
| **Level / Set** | `AssetSet` textures/atlases/fonts, world, `Interpolated` components | level load | level unload | `AssetSet` owns the handles it loaded; unload frees the whole set (§3.6) |
| **Frame** | arena contents, `FramePacket`, view/pass descriptors, `SimEvents` | step 9 (`Begin`) | next frame's step 9 | spans only; never stored; debug builds poison the arena on `Reset` |
| **GPU in-flight** | D3D11: textures/buffers referenced by queued commands | — | released by app any time | D3D11 runtime keeps in-flight resources alive; **no fences or deferred-delete queue needed** (unlike D3D12). |

### 2.6 Threading model

**Single-threaded now.** The design deliberately has no cross-thread data in steady state. The seams that would allow threads later (and the evidence that would justify using them) are:

| Seam | Enables | Do it only if measured |
|---|---|---|
| `FramePacket` is a self-contained, non-owning snapshot | extraction/batching on a worker while the backend consumes the previous packet (⇒ re-introduce a **second arena slot** at that time) | Main-thread extract+batch > ~25 % of frame budget on target hardware |
| `AssetManager.Commit()` is the only mutation point | async decode/IO on workers, publish at commit | Level load or hot-reload hitches visible in the frame-time trace |
| Backend owns all GPU calls | optional render thread | GPU submission CPU time is the bottleneck (unlikely for a 2D game on D3D11) |

### 2.7 Coordinate spaces

| Space | Units | Producer | Consumer |
|---|---|---|---|
| **World** | virtual pixels (float) | simulation | extraction |
| **Virtual screen** | e.g. `480×270` or `1920×1080` (project choice) | `ViewTransform2D` (camera) | backend scene pass |
| **Backbuffer** | physical pixels (DPI-aware) | `ViewportScaler` (`ScaleMode::Fit / IntegerFit / Cover`, already in `RendererTypes.h:914-918`) | present |

Gameplay never reads window or backbuffer size. `config.windowWidth/Height` disappears from simulation code (fixes S4).

---

## 3. Design Decisions

Each decision lists **alternatives → trade-offs / failure modes → recommendation → what evidence would reverse it.** "General practice" = widely used engine technique; "EngineN-specific" = depends on this codebase's constraints.

### 3.1 Timestep model

| Option | Pros | Cons / failure modes |
|---|---|---|
| **A. Fixed tick + interpolation** *(current)* | Deterministic gameplay, replayable inputs, stable physics, frame-data-exact combat; render-rate independent (Gaffer "Fix Your Timestep"). | Displayed state lags by up to one tick (≈ 4.2 ms average, 8.3 ms max at 120 Hz); alpha noise → judder if frame intervals are mis-measured; catch-up ticks at low FPS; needs spiral-of-death cap. |
| B. Fixed tick + extrapolation | Zero added latency. | Overshoot artifacts on direction changes/hits — unacceptable for precise platformer collisions. |
| C. Variable dt everywhere | Simplest; smooth at any FPS. | Non-deterministic jump heights/combat timing; tunneling; unreplayable. Wrong for frame-exact action games. |
| D. Fixed gameplay + render-rate cosmetics | Keeps A's determinism where it matters; particles/shake/UI fades stay smooth and cheap. | Two time bases to keep disjoint (rule: cosmetic code never writes sim state). |

**Recommendation (general practice + EngineN-specific):** **A for gameplay, D for cosmetics.** Keep **120 Hz** — it is an exact multiple of 60/120/240 refresh rates (`TimeManager.h:17`), halves worst-case input-to-tick latency vs 60 Hz, and the sim cost is small. Make Hz a **compile-time constant** (`kSimHz`): runtime configurability buys nothing (gameplay is authored in ticks) and is the root of T5. Camera follows the **sim** (so triggers/locks are deterministic) and is interpolated like any other state; screen shake and trauma decay run on real time.
**Reverse if:** p99 tick cost exceeds the proposed gate (A8, §5.6) or a measured judder metric cannot be brought under threshold by the measures in §3.3.

**Frame-interval smoothing (snapping `dt` to the refresh interval)** is a known remedy for judder (T8) but changes timing semantics. It is **not** adopted; it is an **experiment gated on data** (§5.4: if per-frame interval std-dev on VSync exceeds the gate, prototype behind a flag and compare the judder metric).

### 3.2 Catch-up, hitstop, time scale, pause

* **Catch-up cap.** Keep `maxCatchupTicks = 8` (66.7 ms). Alternatives (unbounded; adaptive; larger cap) trade responsiveness against the spiral of death: if one tick costs more than 8.33 ms the loop can never catch up. Remove the redundant `maxFrameNs` (T3). Surface `WasTimeDropped()` as a counter in stats.
* **Hitstop in ticks.** `Request{channel, ticks}`; decrement once per *committed tick* for each frozen channel; `Request` takes `max(remaining, ticks)` (existing semantics); takes effect from the **next** tick. Authored in `_f60` frames (`4_f60 == 8 ticks`). Ticks keep running during hitstop (dt = 0 for frozen channels), so input buffers keep aging and accepting input — freezing the tick *loop* instead would break buffering. Result: freeze length is identical at any render rate (fixes T1) and is replayable.
* **Time scale.** Scale **dt per tick** (kept), never the tick rate: tick cadence stays fixed, so input sampling, determinism and interpolation are unchanged. `SetTimeScale` is called **only from simulation code at a tick boundary** (or via a dev-command queue drained at tick start).
* **Delete** `m_accQ16`, `m_effScaleQ16`, `GetDomainAlpha`, Q16 helpers after a repo-wide grep (T2). Reintroduce only with a concrete consumer and a test.
* **Pause/refocus.** Keep `SetPaused` (resets clock on resume). Wire `ResetClocksOnRefocus` from the platform's focus event, and reset the pacer deadline there (T6).

### 3.3 Frame pacing and presentation

| Mode | Mechanism | Use |
|---|---|---|
| **VSync** *(default)* | `Present(1, 0)`; no software pacer; optional waitable swap chain wait at frame start | Shipping default |
| **Capped** | VSync off + software pacer; `Present(0, ALLOW_TEARING)` when supported and windowed | Fixed-FPS testing, power saving |
| **Uncapped** | VSync off, no pacer | Benchmarking only (tears) |

Alternatives considered: (i) Present-blocking only — simplest, but queue depth is driver-controlled; (ii) **waitable swap chain** (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` + `IDXGISwapChain2::SetMaximumFrameLatency(1)`) — Microsoft's documented way to cut effective frame latency: the app waits on the handle *before* starting a frame; the wait must precede the first `Present`; the flag must be identical at `ResizeBuffers`; flip model only ([MS Learn: Reduce latency with DXGI 1.3 swap chains]); (iii) software pacer only — adds a busy-wait and cannot see scan-out; (iv) VRR-aware capping (cap just below max refresh) — **FUTURE**.

**Recommendation (EngineN-specific):**
* A single `PresentationPolicy { VSync | Capped(fps) | Uncapped }` owned by the **Presenter**; delete the three-way split (P2). Check and log `SDL_SetRenderVSync`/`Present` failures.
* **Move `FramePacer` out of `TimeManager`** (T7) into the Presenter; replace the manual 1 ms spin with `SDL_DelayPrecise(deadline − now)` (SDL already sleeps in 1 ms slices, tracks overshoot and spins only the tail — `SDL_timer.c:686-756`). **A/B-test** CPU% and frame-interval std-dev at 60/144/240 caps before committing; keep the old loop if it measures better.
* On the D3D11 backend: flip-discard, 2 buffers, waitable object, `MaxLatency = 1`; `Present(1,0)` for VSync; tearing only when `DXGI_FEATURE_PRESENT_ALLOW_TEARING` is supported **and** windowed (the same conditions SDL's own D3D11 backend applies — `SDL_render_d3d11.c:957-970`; NVIDIA's swap-chain guidance also requires the flag pair for unlimited-FPS tearing [NVIDIA: Advanced API Performance — Swap Chains]).
* **Avoid exclusive fullscreen**; use borderless flip (the OS can promote to independent flip). Exclusive fullscreen is incompatible with the tearing present flag (SDL's own comment, `SDL_render_d3d11.c:962`) and is reported (forum-level evidence, not re-verified) not to support the waitable object. If the platform spec requires `ExclusiveFullscreen`, treat it as a separate mode without waitable/tearing.
* Remove `PresentMode::Mailbox` from `RendererTypes.h` until implemented (YAGNI; it cannot be expressed through `SDL_Renderer`, P3).

**Failure modes to test:** device busy → `Present` blocking > refresh interval; minimized window (0×0 buffers) → skip `ResizeBuffers`/present; mode switch with a different swap-chain flag set → DXGI error; pacer + VSync both on → double throttle (policy makes this unrepresentable).

### 3.4 Input (contract frozen)

| Option | Verdict |
|---|---|
| Sample at event time into gameplay | ✗ non-deterministic, cadence-dependent |
| Poll once per render frame and let gameplay read it | ✗ current behavior (S2): edges land between ticks, not replayable |
| **Frame view for UI/dev + tick view for gameplay, one source** | ✔ already provided by `PublishFrameInput` / `CutTick` |

**Rules.** (1) Simulation reads **only** `TickInput`; `FrameInput` is for UI, dev tools and window commands. (2) A press is delivered to **exactly one** tick even if the frame ran 0 or N ticks (0-tick frames keep the edge pending; N-tick frames give it to the first tick). (3) Buffers (jump/attack/dash) are measured in **ticks** (`_f60`). (4) Debug hotkeys that touch gameplay (bullet-time, hitstop) become **dev commands** drained at tick start; purely visual toggles may stay frame-level. (5) Replays record `TickInput` per tick; a sim hash is logged for comparison.
**Reverse if:** the `CutTick` contract cannot guarantee (2) — then fix inside `InputSystem`; this audit could not inspect it (§1.8).

### 3.5 Interpolation

**Recommendation.** An engine-owned value type `Interpolated<T>` (§4.1) with `Snapshot()` called for **every** component at **every** tick start by the world (not by individual gameplay branches), `Snap()` for teleports/spawns/room transitions/camera cuts, and `Get(alpha)` in extraction. Interpolate: position, rotation (shortest arc), scale, continuous tints. **Do not** interpolate: facing, animation frame index (derive from entity tick count), visibility, hitbox state. Optional per-layer **pixel snapping** at extraction for pixel-art layers (`round(pos·zoom)/zoom`); HD-filtered layers keep sub-pixel positions.

Failure modes: forgotten snapshot (S1 — structurally prevented), interpolation across a teleport (streak — `Snap`), double interpolation (sim and presentation both lerp), `alpha` outside `[0,1]` (clamped in `EndFrame`, kept), pixel-snapped position + bilinear sampling shimmer (snap only point-sampled layers).

### 3.6 Assets and handle safety

**Generational handles — challenged.** Alternatives: raw pointers (dangle on reload/unload), plain indices (stale reuse silently aliases), `shared_ptr` (ownership diffusion, atomic refcounts, unclear when GPU memory frees), intrusive refcounts. **Verdict: keep generational indices at the asset layer** because they are the mechanism that makes *hot reload in place*, *fallback on stale*, and *value-type gameplay references* simple, and `Resolve` is already branchless and bounded. **Costs to acknowledge:** 12-bit generation retires slots after 4,095 reuses of one slot (A2) — acceptable with a counter; handles don't express *who* keeps an asset alive (A4).

**Recommendations.**
1. **Lifetime by scope, not by handle:** `AssetSet` (room/level/UI) owns what it loaded; `Unload(set)` frees all. Per-handle `Unload` becomes dev/hot-reload only. Refcounting is **not** adopted; revisit only if assets are shared by independently-timed systems.
2. **Single identity:** `Load<Texture>(VirtualPath)`. Normalize (`/`, lowercase on Windows, collapse `.`/`//`, reject `..`) *then* hash; id derived internally (kills A3's two-key API). Debug builds store the normalized path in cold data and assert on id collision. Keep `"…"_aid` only for `Find`.
3. **`SetReady` preserves `m_cold[slot].id`** (assert equality) — fixes A1 regardless of loader behavior.
4. **Hot reload keeps the handle:** re-decode → upload → swap hot data at the commit point; bump generation only on `Unload`.
5. **GPU ids are frame-scoped:** extraction resolves `SpriteRef → SpriteView{ textureSlot, uv, size, pivot }`; `textureSlot` is valid for the current frame only (R11). The backend validates in debug builds (slot live) and treats `Destroy` immediately at commit.
6. **Premultiply on load** (RGBA8) so texels match the blend state (A9); later move to the asset build step. `TextureDesc` carries the filter (point/linear); the backend keeps one sampler per texture slot.
7. **Capacity coupling:** `AssetManagerConfig.textureCapacity (128)` must be ≤ backend slots (256) minus internal textures; assert in `Create`.
8. **Async loading — FUTURE.** Only the commit point (`AssetManager::Commit`) may publish results; sim never sees `Loading` except as fallback. No threads until a frame-time trace shows load hitches.

### 3.7 Memory and allocation

**"Zero allocation on the hot path" — challenged.** *Value:* avoids allocator latency spikes and fragmentation in a 120 Hz loop. *Cost:* fixed caps cause silent loss when exceeded (R4, R8) and encourage arbitrary constants (131,072 / 65,536 / 4,096). **Verdict: keep, but (a) narrow it to `Tick`, `Extract`, `Finalize`, `Execute`, `Present`, `Profiler`; (b) enforce it with a test (§5, T-ALLOC-01) rather than convention; (c) make every cap a config value derived from content budgets and surface overflow as a counter + debug assert + rate-limited log, never silently.** Allocation in loading, tools and WARN+ logging is allowed.

**Double-buffered frame arena — challenged.** Valid only when a producer on thread A writes slot *N+1* while a consumer on thread B reads slot *N* (or when the backend retains pointers past `Execute`). Neither exists (R6): `Submit` is synchronous, and both SDL and D3D11 copy vertex/instance data during the call (D3D11: `WRITE_DISCARD` renaming). **Verdict: one arena slot.** Re-introduce a second slot together with threaded extraction (§2.6), not before.

Other: transient per-frame containers (e.g., `SimEvents`, scratch lists) use `std::pmr::monotonic_buffer_resource` over a fixed buffer with `std::pmr::null_memory_resource()` as upstream (allocation failure is explicit, never silently heap) and `release()` at frame start. The `SDL_Vertex`/`int` scratch vectors (R10) disappear with the D3D11 backend; in the SDL backend, pre-size to the arena cap instead of per-batch `resize`.

### 3.8 Render recording and views

* **Stateless per-frame recording** (kept). A retained scene graph/cached tile chunks is **FUTURE**, only if tilemap extraction shows up in the profile.
* **`RenderQueue`** wraps the arena with typed helpers (`Sprite`, `Rect`, `NineSlice`, `Text`) so gameplay never fills `QuadInstance` or `SortKey` by hand. Layers are named constants; each layer has a **`SortPolicy`**:

| Policy | Key contribution | For | Reordering by state? |
|---|---|---|---|
| `Submission` | `depth = 0` | tile layers, parallax, particles, UI panels | **Yes** within layer (order-independent content) — stable sort preserves call order for ties |
| `Depth` | explicit 16-bit depth | actors with authored z | only among equal depth |
| `YSort` | quantized world-Y | top-down/2.5D actors | only among equal depth |

* **Views.** `ViewSlot` carries `ViewTransform2D`; the backend applies it. Extraction works in world space only; no `config.windowWidth/Height` in gameplay (S4).
* **Virtual resolution.** Scene is composed in virtual-screen space; the backend scales to the backbuffer via `ViewportScaler` (`Fit/IntegerFit/Cover`) with letterboxing and DPI awareness. For the SDL backend (stage 1) this can use `SDL_SetRenderLogicalPresentation`; the D3D11 backend renders the scene to a virtual-resolution target and composites.

### 3.9 Sorting, transparency, batching

**Transparency strategy.** Painter's order by explicit layers + stable sort. Alternatives: depth buffer with alpha-test (helps only opaque overdraw; soft edges still need sorted blending), OIT (overkill for 2D). **Premultiplied alpha** throughout (general practice: correct filtering and compositing, enables the additive trick). Order-dependence classes:

| Class | Order matters? | Batching consequence |
|---|---|---|
| Opaque | No | Sort freely by state |
| Additive (`alpha = 0` premultiplied) | No (commutative; 8-bit clamp equals clamp of the sum for non-negative terms) | Sort freely; **shares the Premultiplied blend state** ⇒ merges with adjacent alpha batches |
| Alpha-blended | **Yes** | Preserve layer/depth order; batch only adjacent same-state runs |
| Multiply | Yes | Own state; rare |

**Sort algorithm — challenged.** Candidates: current stable LSD radix; `std::stable_sort(key)`; `std::sort` on `(key, seq)`; 32-bit compressed key radix. *Non-negotiable property:* **stability or an explicit sequence tie-break** — without it equal-key sprites flicker. *Not established:* that radix is faster at EngineN's actual `N` (likely a few thousand entries); the monotonic early-out already makes pre-ordered input O(N). **Verdict: keep the current implementation, add the stability test, and run the benchmark in §5.5; replace with `std::stable_sort` only if radix is not ≥ 1.5× faster at the N where sort exceeds 5 % of the frame budget.** Don't claim a win in either direction without the numbers.

**Gather without the second copy** (R5): write sorted instances into the scratch array and point `FramePacket.instances` at it (no copy back); validate `SortEntry` ranges **once, at `PushSortEntry`**, so later stages can rely on well-formed entries (keep release-mode guards, assert in debug). With D3D11 the backend then performs **one** sequential `memcpy` into the mapped `WRITE_DISCARD` buffer. Fusing gather and upload (Batcher writing straight into mapped memory) would couple the Batcher to the backend and is **not adopted** unless profiling shows the `48·N`-byte copy matters.

**Coalescing.** Keep adjacency merge keyed on `(pass, blend, pipeline, texture, view)`; fix overflow (R4: stop merging after the first dropped batch, count `droppedBatches`, assert in debug); make `DrawBatch::instanceCount` 32-bit if it is not already (**MISSING:** `FramePacket.h`).

**Failure modes:** unique depth per sprite ⇒ batch count ≈ sprite count (R9; mitigated by `SortPolicy` + atlases); texture thrash between layers; view/pass changes mid-sequence — all visible in the batch counters (§5.4).

### 3.10 Backend: SDL_Renderer vs D3D11 vs SDL_GPU

| Criterion | `SDL_Renderer` (now) | `SDL_Renderer` + `SDL_GPURenderState` (SDL ≥ 3.4) | **Direct D3D11** | `SDL_GPU` |
|---|---|---|---|---|
| Instanced 48 B quads | ✗ CPU-expands to 152 B/quad (R10) | ✗ same geometry path | ✔ per-instance stream, 6 verts via `SV_VertexID` | ✔ |
| Custom shaders (distortion, CA, lights, MSDF) | ✗ | ✔ fragment shaders only; requires a GPU-device-backed renderer (`SDL_render.c:6268-6277`) | ✔ | ✔ (shader toolchain per API) |
| Per-batch blend with textures | ✗ must set *texture* blend mode (R1) | same | ✔ pipeline state | ✔ |
| Pacing control (waitable, flags) | ✗ (P3: flip-sequential, no waitable) | ✗ | ✔ | partial (SDL swapchain params) |
| Render targets / post | basic | ✔ | ✔ | ✔ |
| Debug tools | RenderDoc via D3D11 driver | – | RenderDoc, debug layer, info queue | RenderDoc / backend tools |
| Portability | ✔ | ✔ | ✗ Windows only | ✔ |
| Implementation cost | 0 | medium | medium–high | high (shader pipeline, explicit sync) |
| Risk | known bugs R1–R3 | newer API surface | device-lost, HLSL tooling in MinGW | newest API, shader cross-compile |

**Recommendation (EngineN-specific, staged).**
1. **Stage 1 — fix `SDL_Renderer`** (cheap, removes confirmed bugs): per-batch `SDL_SetTextureBlendMode` (set before each `RenderGeometry`; SDL snapshots `texture->blendMode` at queue time), switch to `SDL_BLENDMODE_BLEND_PREMULTIPLIED`, honor `FlipX/Y` by swapping UVs, apply `ViewSlot` matrices on the CPU, split `Execute` from `Present`.
2. **Stage 2 — add a D3D11 backend behind `RenderBackend`**, because the target feature set (offscreen targets, distortion/chromatic aberration, 2D lights, MSDF text, SDF shapes, waitable pacing) needs custom shaders and pacing control. **Justified by features, not by a measured speed gap** — no profile of the current backend exists (P4).
3. **If cross-platform is a product requirement, choose `SDL_GPU` instead of D3D11**; the `RenderBackend`/`FramePacket` seam makes that choice local. This is the one open question for the owner.

**D3D11 specifics (general practice unless marked):**
* **Device:** `D3D11CreateDevice`, hardware, `BGRA_SUPPORT`, `DEBUG` in debug builds; FL 11_0 minimum; **WARP** for CI/headless golden-image tests.
* **SDL integration:** create the SDL window *without* a renderer; get the `HWND` via `SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)`; backbuffer size from `SDL_GetWindowSizeInPixels`; resize on `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`; DPI on `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED`; `MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)` (SDL owns fullscreen transitions).
* **Swap chain:** `CreateSwapChainForHwnd`, `DXGI_SWAP_EFFECT_FLIP_DISCARD`, 2 buffers, `B8G8R8A8_UNORM`, flags `FRAME_LATENCY_WAITABLE_OBJECT` and (if supported) `ALLOW_TEARING`. Resize: release RTV + backbuffer refs → `ResizeBuffers(0,0,0,UNKNOWN,sameFlags)` → recreate RTV; skip when 0×0.
* **Color:** keep gamma-space (UNORM) blending to match the current renderer (SDL's D3D11 swap chain is `B8G8R8A8_UNORM`, `SDL_render_d3d11.c:848`) and typical 2D art; linear/sRGB pipeline is **FUTURE** and only matters for bloom/lights.
* **Geometry:** one input layout over `QuadInstance` (`R32G32_FLOAT`×3 @0/8/16, `R16G16B16A16_UNORM` @24, `R8G8B8A8_UNORM` @32, `R32_UINT` @36, `R32G32_FLOAT` @40 = 48 B), no vertex buffer, `DrawInstanced(6, count, 0, firstInstance)` per batch.
* **Upload:** one `DYNAMIC` instance buffer sized to the arena cap; `Map(WRITE_DISCARD)` **once per frame**; gather sorted instances straight into it; per-view constant buffer.
* **Blend states (pre-created):** Premultiplied `ONE, INV_SRC_ALPHA`; Opaque off; Multiply `DEST_COLOR, ZERO`; Additive aliases Premultiplied (alpha 0).
* **Resource lifetime:** D3D11 keeps in-flight resources alive until the GPU is done, so `Destroy` = unbind SRV if bound, then `Release()`; **no fence or deferred-delete queue** (that machinery is a D3D12 concern).
* **Shaders:** compile offline in CMake (Windows SDK `fxc`/`dxc`) and embed bytecode; optional runtime `D3DCompile` (`d3dcompiler_47.dll` via `LoadLibrary`) in debug for hot reload. **Verify** that `d3d11.h`, `dxgi1_5.h` and your shader compiler are available in the w64devkit/mingw-w64 setup; use a minimal in-house `ComPtr` instead of `wrl/client.h`.
* **Debuggability:** debug layer + `ID3D11InfoQueue` break-on-error, `SetPrivateData(WKPDID_D3DDebugObjectName)` names, `ID3DUserDefinedAnnotation` pass markers, `ReportLiveDeviceObjects` at shutdown, timestamp-disjoint queries for GPU ms.
* **Device removed/reset:** v1 treats it as **fatal with a clear log** (`GetDeviceRemovedReason`). Recovery (recreate + re-upload all assets) is **FUTURE**.

### 3.11 Shutdown and object lifetime

* Destroy in reverse creation order; `AssetManager` (which calls `ITextureDevice::Destroy`) strictly **before** the backend; backend: `ClearState`, `Flush`, release in reverse, live-object report.
* **Make `RenderDevice`/`Backend` and `AssetManager` non-movable after creation** (A5/P5): use `Init()` two-phase construction (as `RenderDevice::Init` already does) and `= delete` the move operations, or hold them in `std::unique_ptr`. `Result<AssetManager,…>` by value is the thing that forces a move today.
* `Shutdown()` stays idempotent (already true for `RenderDevice`).

### 3.12 Debuggability

Always-on, allocation-free instrumentation: per-stage `ns` timers in a fixed ring (clock-pump, assets, ticks, extract, sort, gather, coalesce, backend CPU, **present wait**); counters (ticks/frame, catch-up/dropped-time events, quads, sort entries, batches, texture switches, upload bytes, fallback resolves, **dropped instances/batches**); overlay toggle; CSV dump of the last N frames; GPU timestamps (D3D11); deterministic replay with a sim hash. A frame that drops anything logs `FrameStats` once (rate-limited).

### 3.13 Challenged assumptions — verdict table

| Assumption | Verdict | Condition to reverse |
|---|---|---|
| Strict fixed timestep | **Keep for gameplay**; cosmetics use render time | Judder metric above gate that smoothing/pacing cannot fix; tick cost over gate |
| 120 Hz (compile-time) | **Keep** | p99 tick cost > gate on lowest-spec target |
| Zero-allocation hot path | **Keep, narrowed + test-enforced; caps → config + counters** | — |
| Double-buffered arena | **Remove (single slot)** | Threaded extraction introduced |
| Generational handles | **Keep at asset layer**; frame-scoped slots at backend boundary | Need cross-thread ref sharing ⇒ consider refcounts |
| Radix sort | **Keep stability; keep impl pending benchmark** | `std::stable_sort` within 1.5× at the critical N ⇒ simplify |
| 64-bit sort key | **Keep** (layout verified); add per-layer `SortPolicy` | Layers/passes/pipelines outgrow bit budget |
| `SDL_Renderer` as the long-term backend | **Fix now; replace for features** | Features reduced to what `SDL_Renderer` can do |
| Direct D3D11 | **Conditional on Windows-only scope** | Cross-platform required ⇒ `SDL_GPU` |
| `FramePacer` inside `TimeManager` | **Move to Presenter** | — |
| Interpolation snapshot in gameplay code | **Engine-owned** | — |

---

## 4. API and Code Examples

**Verification status.** Snippets 4.1–4.5 were **compile-checked** (`g++ 13.3 -std=c++20 -Wall -Wextra -fsyntax-only`) against a minimal stub header that mirrors the types visible in the bundle; the Batcher logic for R4 and the hitstop counts for T1 were additionally **executed on reduced models** and reproduce the findings. 4.6 (SDL stage-1 fixes) and 4.7 (D3D11/HLSL) are **illustrative and not compiled here** (SDL/Windows headers not available in the analysis environment). None of this has been built inside your tree — treat as a design reference, and expect to adjust names to `core/Types.h`, `FramePacket.h`, `TickInput`, etc.

Assumption: `TickInput` exposes edge queries (`Pressed(action)`) and `Axis(i)` (`Axis` is used in `main.cpp:1916`; `Pressed` on the tick view is what the frozen contract's jump-buffering requires). If the tick edge accessor has another name, only the `PlayerSim::Tick` lines change.

### 4.1 Time, hitstop in ticks, tick context, interpolation (`core/time`)

```cpp
// ---- core/time/TimeManager.h (changes only) --------------------------------
inline constexpr u32 kSimHz = 120;                       // compile-time: gameplay is authored in ticks
static_assert(kSimHz % 60 == 0, "sim rate must be an integer multiple of 60");
constexpr u32 operator""_f60(unsigned long long frames60) noexcept {
    return static_cast<u32>(frames60 * (kSimHz / 60));   // 3_f60 == 6 ticks == 50 ms
}

struct HitstopRequest { HitstopChannel channel = HitstopChannel::World; u32 ticks = 0; };

class HitstopEngine {                                    // counted in TICKS, not real seconds
public:
    void Request(const HitstopRequest& r) noexcept {     // max() semantics kept; effective from the NEXT tick
        const auto i = static_cast<usize>(r.channel);
        if (i < m_ticks.size()) m_ticks[i] = std::max(m_ticks[i], r.ticks);
    }
    void AdvanceTick() noexcept {                        // TimeManager::EndTick(), once per committed tick
        for (u32& t : m_ticks) if (t > 0) --t;
    }
    void Reset() noexcept { m_ticks.fill(0); }
    [[nodiscard]] bool IsFrozen(HitstopChannel c) const noexcept {
        const auto i = static_cast<usize>(c);
        return i < m_ticks.size() && m_ticks[i] > 0;
    }
private:
    std::array<u32, static_cast<usize>(HitstopChannel::Count)> m_ticks{};
};

// Simulation -> presentation, fixed capacity, frame lifetime. Overflow is counted, never silent.
enum class SimEventKind : u8 { AttackStarted, HitLanded, DashStarted, Landed };
struct SimEvent { SimEventKind kind{}; u32 tick = 0; Vec2 pos{}; f32 magnitude = 0.0f; };
class SimEvents {
public:
    static constexpr u32 kCapacity = 256;
    bool Push(const SimEvent& e) noexcept {
        if (m_count >= kCapacity) { ++m_dropped; return false; }
        m_events[m_count++] = e; return true;
    }
    [[nodiscard]] std::span<const SimEvent> View() const noexcept { return {m_events.data(), m_count}; }
    [[nodiscard]] u32 Dropped() const noexcept { return m_dropped; }
    void Clear() noexcept { m_count = 0; }
private:
    std::array<SimEvent, kCapacity> m_events{};
    u32 m_count = 0, m_dropped = 0;
};

class TimeManager;                                       // excerpt: only what TickContext needs
class TimeManager {
public:
    [[nodiscard]] f64 ScaledDt(ChannelId) const noexcept;
    [[nodiscard]] u64 SimTickCount() const noexcept;
    void SetTimeScale(ChannelId, f64) noexcept;
    [[nodiscard]] HitstopEngine& Hitstop() noexcept { return m_hitstop; }
private:
    HitstopEngine m_hitstop;
};

// What simulation code sees during one tick. No wall-clock, no frame dt, no GPU.
class TickContext {
public:
    TickContext(TimeManager& t, SimEvents& e) noexcept : m_time(t), m_events(e) {}
    [[nodiscard]] f32 Dt(ChannelId c) const noexcept { return static_cast<f32>(m_time.ScaledDt(c)); } // 0 when frozen
    [[nodiscard]] u64 TickIndex() const noexcept { return m_time.SimTickCount(); }
    void RequestHitstop(HitstopChannel c, u32 ticks) noexcept { m_time.Hitstop().Request({c, ticks}); }
    void SetTimeScale(ChannelId c, f64 s) noexcept { m_time.SetTimeScale(c, s); }  // tick-aligned by construction
    bool Emit(const SimEvent& e) noexcept { return m_events.Push(e); }
private:
    TimeManager& m_time;
    SimEvents&   m_events;
};

// ---- Interpolation: engine-owned, impossible to snapshot conditionally -----------
template <class T>
concept Interpolatable = requires(const T& a, const T& b, f32 t) { { Lerp(a, b, t) } -> std::convertible_to<T>; };

template <Interpolatable T>
class Interpolated {
public:
    constexpr Interpolated() = default;
    constexpr explicit Interpolated(const T& v) noexcept : m_prev(v), m_cur(v) {}
    constexpr void BeginTick() noexcept { m_prev = m_cur; }          // EVERY tick, frozen or not
    constexpr void Snap(const T& v) noexcept { m_prev = m_cur = v; } // spawn / teleport / room change / camera cut
    [[nodiscard]] constexpr T&       Cur()       noexcept { return m_cur; }
    [[nodiscard]] constexpr const T& Cur() const noexcept { return m_cur; }
    [[nodiscard]] constexpr T Get(f32 alpha) const noexcept {
        return Lerp(m_prev, m_cur, std::clamp(alpha, 0.0f, 1.0f));
    }
private:
    T m_prev{}, m_cur{};
};
```

Notes: `TimeManager::EndTick()` calls `m_hitstop.AdvanceTick()` after `CommitTick()`; `HitstopEngine::Update(realDt)` and the call in `BeginFrameManual` are deleted. `m_accQ16`, `m_effScaleQ16`, `GetDomainAlpha`, `FramePacer` (moved to `Presenter`, §4.6) and `maxFrameNs` are removed from `TimeManager`.

### 4.2 Simulation call site (replaces `main.cpp:1901-1944`)

```cpp
enum class PlayerState : u8 { Idle, Run, Attack, Dash };

struct PlayerSim {
    Interpolated<Vec2> pos{Vec2{640.0f, 360.0f}};
    Vec2        vel{};
    PlayerState state = PlayerState::Idle;
    u32         stateTicks = 0;      // per-entity clock: advances only on ticks where the Player channel runs
    f32         speed = 420.0f;

    void BeginTick() noexcept { pos.BeginTick(); }          // called by the world for every tick

    void Tick(const TickInput& in, TickContext& ctx) noexcept {
        const f32 dt = ctx.Dt(ChannelId::Player);           // 0 during hitstop
        if (dt <= 0.0f) return;                             // frozen: prev == cur already, nothing drifts
        ++stateTicks;

        // Edges come from the TICK view. Exactly one tick sees each press (replayable).
        if (state != PlayerState::Attack && in.Pressed(GameAction::Attack)) {
            state = PlayerState::Attack; stateTicks = 0;
            ctx.RequestHitstop(HitstopChannel::Player, 3_f60);          // 6 ticks at any render rate
            ctx.Emit({SimEventKind::AttackStarted, static_cast<u32>(ctx.TickIndex()), pos.Cur(), 1.0f});
        } else if (state != PlayerState::Dash && in.Pressed(GameAction::Dash)) {
            state = PlayerState::Dash; stateTicks = 0;
            ctx.Emit({SimEventKind::DashStarted, static_cast<u32>(ctx.TickIndex()), pos.Cur(), 1.0f});
        }

        const Vec2 dir{in.Axis(0), in.Axis(1)};
        const f32  v   = speed * (state == PlayerState::Dash ? 2.2f : 1.0f);
        if (dir.LengthSquared() > 0.0001f) vel = dir * v;
        pos.Cur() += vel * dt;

        constexpr u32 kAttackTicks = 18_f60, kDashTicks = 12_f60;       // authored in 60 Hz frames
        if ((state == PlayerState::Attack && stateTicks >= kAttackTicks) ||
            (state == PlayerState::Dash   && stateTicks >= kDashTicks)) state = PlayerState::Idle;
    }
};

struct World {
    PlayerSim player;
    // ... other interpolated entities
    void BeginTick() noexcept { player.BeginTick(); /* for (auto& e : enemies) e.BeginTick(); */ }
    void Tick(const TickInput& in, TickContext& ctx) noexcept { player.Tick(in, ctx); }
};
```

What changed relative to `main.cpp`: attack/dash are decided **inside the tick** from the **tick view**; hitstop and time-scale are requested through `TickContext`; `prev` is snapshotted by `BeginTick()` regardless of dt; animation time is the entity's own `stateTicks`; there is no `config.windowWidth` in gameplay.

### 4.3 Assets: single identity key, scope-owned lifetime

```cpp
// ---- Identity: normalize first, hash second, one key -----------------------------
class AssetPath {
public:
    [[nodiscard]] static Result<AssetPath, EngineError> Make(std::string_view raw) noexcept {
        AssetPath p;
        usize len = 0; bool lastSlash = true;                 // true => strips leading '/'
        for (usize i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\\') c = '/';
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');   // Windows is case-insensitive
            if (c == '/') {
                if (lastSlash) continue;                      // collapse "//"
                lastSlash = true;
            } else if (c == '.' && lastSlash) {
                const bool dot     = (i + 1 == raw.size()) || raw[i + 1] == '/' || raw[i + 1] == '\\';
                const bool dotdot  = i + 1 < raw.size() && raw[i + 1] == '.' &&
                                     (i + 2 == raw.size() || raw[i + 2] == '/' || raw[i + 2] == '\\');
                if (dotdot) return Err(EngineError{"asset path may not contain '..'"});
                if (dot)    { continue; }                     // drop "./"
                lastSlash = false;
            } else {
                lastSlash = false;
            }
            if (len + 1 >= p.m_buf.size()) return Err(EngineError{"asset path too long"});
            p.m_buf[len++] = c;
        }
        if (len == 0) return Err(EngineError{"empty asset path"});
        p.m_len = static_cast<u16>(len);
        u64 h = 14695981039346656037ULL;                      // FNV-1a 64 over the NORMALIZED bytes
        for (usize i = 0; i < len; ++i) { h ^= static_cast<u8>(p.m_buf[i]); h *= 1099511628211ULL; }
        p.m_id = AssetId{h};
        return p;
    }
    [[nodiscard]] AssetId          Id()   const noexcept { return m_id; }
    [[nodiscard]] std::string_view View() const noexcept { return {m_buf.data(), m_len}; }
private:
    std::array<char, 256> m_buf{};
    u16     m_len = 0;
    AssetId m_id{};
};

// ---- Lifetime by scope: a set owns what it loaded --------------------------------
class AssetManager {                                          // excerpt of the proposed surface
public:
    [[nodiscard]] Result<AssetHandle<Texture>, EngineError> LoadTexture(const AssetPath&) noexcept;
    [[nodiscard]] Result<AssetHandle<Atlas>,   EngineError> CreateAtlasGrid(const AssetPath&, const AtlasGridDesc&) noexcept;
    void Unload(AssetHandle<Texture>) noexcept;
    void UnloadAtlas(AssetHandle<Atlas>) noexcept;
    void Commit() noexcept;                                   // the ONLY point where slots/generations mutate
};

class AssetSet {
public:
    explicit AssetSet(AssetManager& m) noexcept : m_mgr(&m) {}
    AssetSet(const AssetSet&) = delete;            AssetSet& operator=(const AssetSet&) = delete;
    ~AssetSet() { Release(); }

    [[nodiscard]] Result<AssetHandle<Texture>, EngineError> LoadTexture(std::string_view path) noexcept {
        auto p = AssetPath::Make(path);
        if (!p) return Err(p.Error());
        auto h = m_mgr->LoadTexture(*p);
        if (h) m_textures.push_back(*h);                      // loading may allocate; steady state may not
        return h;
    }
    [[nodiscard]] Result<AssetHandle<Atlas>, EngineError> LoadAtlasGrid(std::string_view path, const AtlasGridDesc& d) noexcept {
        auto p = AssetPath::Make(path);
        if (!p) return Err(p.Error());
        auto h = m_mgr->CreateAtlasGrid(*p, d);
        if (h) m_atlases.push_back(*h);
        return h;
    }
    void Release() noexcept {                                 // atlases first: they reference textures
        for (auto a : m_atlases)  m_mgr->UnloadAtlas(a);
        for (auto t : m_textures) m_mgr->Unload(t);
        m_atlases.clear(); m_textures.clear();
    }
private:
    AssetManager*                    m_mgr;
    std::vector<AssetHandle<Atlas>>   m_atlases;
    std::vector<AssetHandle<Texture>> m_textures;
};

// ---- AssetTable::SetReady — the table owns identity (fixes A1) -------------------
//   void SetReady(u32 slot, const typename T::Hot& hot, typename T::Cold cold) noexcept {
//       if (slot == 0 || slot >= m_capacity) return;
//       ENGINE_ASSERT(cold.id == AssetId{} || cold.id == m_cold[slot].id, "Cold::id must match Allocate()");
//       cold.id = m_cold[slot].id;                 // Free() -> RemoveLookup(m_cold[slot].id) stays correct
//       m_hot[slot] = hot; m_cold[slot] = std::move(cold); m_state[slot] = AssetState::Ready;
//   }
```

Call site (replaces `main.cpp:1819-1828`):

```cpp
AssetSet level{assets};
auto sheet = level.LoadTexture("textures/Hero_Sheet.tga");              // normalized -> "textures/hero_sheet.tga"
if (!sheet) { LOG_ERROR("{}", sheet.Error().message); }
AtlasGridDesc d{.texture = *sheet, .frameWidth = 32, .frameHeight = 32, .frameCount = 4, .pivotX = 16, .pivotY = 16};
auto atlas = level.LoadAtlasGrid("atlases/hero", d);
// ... room exit: level.Release();   hot reload: Reload keeps handles, bumps nothing
```

### 4.4 Recording API and gameplay extraction

```cpp
// ---- Recording API (gameplay never touches QuadInstance / SortKey) ----------------
enum class Blend : u8 { Alpha, Additive, Opaque, Multiply };       // Additive == Premultiplied + alpha 0
enum class SortPolicy : u8 { Submission, Depth, YSort };

struct LayerDesc { u8 id; SortPolicy policy; };
namespace Layers {
    inline constexpr LayerDesc Parallax{ 10, SortPolicy::Submission };
    inline constexpr LayerDesc Tiles   { 20, SortPolicy::Submission };   // order-independent => state-sorted
    inline constexpr LayerDesc Actors  { 30, SortPolicy::YSort };
    inline constexpr LayerDesc Fx      { 40, SortPolicy::Submission };
    inline constexpr LayerDesc Ui      { 60, SortPolicy::Submission };
}

struct SpriteDesc {
    SpriteView sprite{};
    Vec2  pos{};                  // pivot position in world space
    Vec2  scale{1.0f, 1.0f};
    f32   rotation = 0.0f;        // radians, about the pivot
    Color tint = Colors::White;
    Blend blend = Blend::Alpha;
    bool  flipX = false;
    u16   depth = 0;              // SortPolicy::Depth
    f32   sortY = 0.0f;           // SortPolicy::YSort
};

[[nodiscard]] constexpr u32 PackPremultiplied(const Color& c) noexcept { return c.ToPremultipliedRGBA8(); }
[[nodiscard]] constexpr u32 PackAdditive(const Color& c) noexcept {      // rgb*a, alpha byte forced to 0
    const f32 a = std::clamp(c.a, 0.0f, 1.0f);
    const auto q = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.b * a) << 16) | (q(c.g * a) << 8) | q(c.r * a);
}

struct QueueCounters { u32 droppedQuads = 0; };

class RenderQueue {
public:
    explicit RenderQueue(FrameArenaSlot& arena) noexcept : m_arena(arena) {}

    void Begin(const ViewTransform2D& view, Color clear) noexcept {
        m_arena.Reset();
        m_counters = {};
        (void)m_arena.PushPass({.pass = RenderPass::Scene, .enabled = true, .clearColor = clear});
        (void)m_arena.PushViewSlot({.view = view});
        m_view = view;
        m_layer = Layers::Tiles; m_pass = RenderPass::Scene;
    }
    void SetLayer(LayerDesc l) noexcept { m_layer = l; }
    void SetPass(RenderPass p)  noexcept { m_pass = p; }
    [[nodiscard]] const ViewTransform2D& View() const noexcept { return m_view; }
    [[nodiscard]] const QueueCounters& Counters() const noexcept { return m_counters; }

    [[nodiscard]] bool Sprite(const SpriteDesc& d) noexcept {
        const f32 w = d.sprite.size[0] * d.scale.x, h = d.sprite.size[1] * d.scale.y;
        const f32 c = std::cos(d.rotation), s = std::sin(d.rotation);
        // quad center = pivot position + R * (half size - pivot)
        const f32 ox = w * 0.5f - d.sprite.pivot[0] * d.scale.x;
        const f32 oy = h * 0.5f - d.sprite.pivot[1] * d.scale.y;
        QuadInstance q{};
        q.pos[0] = d.pos.x + ox * c - oy * s;  q.pos[1] = d.pos.y + ox * s + oy * c;
        q.size[0] = w;  q.size[1] = h;
        q.rotCosSin[0] = c;  q.rotCosSin[1] = s;
        std::memcpy(q.uvRect, d.sprite.uvRect, sizeof(q.uvRect));
        q.colorPremul = (d.blend == Blend::Additive) ? PackAdditive(d.tint) : PackPremultiplied(d.tint);
        q.kindFlags   = d.flipX ? static_cast<u32>(QuadFlag_FlipX) : 0u;   // kind 0 == Sprite

        const BlendMode bm = ToBackendBlend(d.blend);                       // Additive -> Premultiplied
        const u16 depth = DepthFor(d);
        const u64 key = SortKey::Pack(static_cast<u8>(m_pass), m_layer.id, depth, static_cast<u8>(bm),
                                      /*pipeline: Sprite*/ 0, d.sprite.texture.slot);
        return Push(q, key);
    }

private:
    static constexpr BlendMode ToBackendBlend(Blend b) noexcept {
        switch (b) {
            case Blend::Opaque:   return BlendMode::Opaque;
            case Blend::Multiply: return BlendMode::Multiply;
            default:              return BlendMode::Premultiplied;           // Alpha and Additive share GPU state
        }
    }
    [[nodiscard]] u16 DepthFor(const SpriteDesc& d) const noexcept {
        switch (m_layer.policy) {
            case SortPolicy::Submission: return 0;                           // stable sort keeps call order
            case SortPolicy::Depth:      return d.depth;
            case SortPolicy::YSort:      return static_cast<u16>(std::clamp(d.sortY, 0.0f, 65535.0f));
        }
        return 0;
    }
    bool Push(const QuadInstance& q, u64 key) noexcept {
        if (m_arena.PushQuad(q, SortKey{key}, 0)) return true;
        ++m_counters.droppedQuads;                                           // visible in FrameStats, never silent
        return false;
    }
    FrameArenaSlot&  m_arena;
    ViewTransform2D  m_view{};
    LayerDesc        m_layer = Layers::Tiles;
    RenderPass       m_pass  = RenderPass::Scene;
    QueueCounters    m_counters{};
};
```

```cpp
// ---- Gameplay call sites ---------------------------------------------------------
struct Camera { Interpolated<Vec2> center; ViewTransform2D View(f32 alpha) const noexcept; };

class Game {
public:
    // Fixed tick: deterministic. Receives the TICK view of input only.
    void Tick(const TickInput& in, TickContext& ctx) noexcept { m_world.Tick(in, ctx); }
    void BeginTick() noexcept { m_world.BeginTick(); m_camera.center.BeginTick(); }
    void OnFrameInput(const FrameInput&) noexcept { /* UI navigation, menus; dev commands are queued for tick start */ }

    // Render-rate: cosmetic only. May read sim events; must not write sim state.
    void Update(f64 /*realDt*/, f32 /*alpha*/, const SimEvents& ev) noexcept {
        for (const SimEvent& e : ev.View()) {
            if (e.kind == SimEventKind::HitLanded) { /* spawn spark, add trauma, play sfx */ }
        }
    }

    void Extract(RenderQueue& q, f32 alpha, const AssetReader& assets) noexcept {
        q.Begin(m_camera.View(alpha), Color::FromRGBA8(18, 18, 24));

        q.SetLayer(Layers::Actors);
        const Vec2 p = m_world.player.pos.Get(alpha);                       // interpolated position
        (void)q.Sprite({.sprite = assets.Sprite({m_heroAtlas, AnimFrame(m_world.player)}),
                        .pos = p, .tint = Colors::White, .flipX = m_facingLeft, .sortY = p.y});

        if (m_world.player.state == PlayerState::Dash) {                    // additive trail, same batch as alpha
            q.SetLayer(Layers::Fx);
            (void)q.Sprite({.sprite = assets.Sprite({m_heroAtlas, 1}), .pos = p + m_world.player.vel * -0.035f,
                            .tint = Color{0.7f, 0.4f, 1.0f, 0.45f}, .blend = Blend::Additive});
        }
    }
private:
    static u16 AnimFrame(const PlayerSim& pl) noexcept {                    // per-entity clock, not global ticks
        switch (pl.state) {
            case PlayerState::Attack: return 2;
            case PlayerState::Dash:   return 1;
            case PlayerState::Run:    return static_cast<u16>((pl.stateTicks / 15) % 4);
            default:                  return 0;
        }
    }
    World               m_world;
    Camera              m_camera;
    AssetHandle<Atlas>  m_heroAtlas{};
    bool                m_facingLeft = false;
};
```

### 4.5 Batcher: validate at the boundary, gather without copy-back, no silent overflow

```cpp
// ---- FrameArenaSlot::PushSortEntry — validate once, at the boundary --------------
bool FrameArenaSlot::PushSortEntry(const SortEntry& e) noexcept {
    const bool inRange = e.instanceCount > 0 &&
                         static_cast<u64>(e.instanceIndex) + e.instanceCount <= m_instanceCount;
    if (m_sortEntryCount >= kMaxSortEntries || !inRange) {
        ++m_droppedEntries;                              // surfaced via FrameStats; assert in debug
        ENGINE_ASSERT(inRange, "SortEntry range outside recorded instances");
        return false;
    }
    m_sortEntries[m_sortEntryCount++] = e;
    return true;
}

// ---- Batcher.cpp: gather without copy-back (fixes R5) ----------------------------
// Returns the draw-ordered instances. Either a view of the arena (already ordered) or of `scratch`.
std::span<const QuadInstance> GatherInstances(const FrameArenaSlot& slot, bool wasSorted,
                                              QuadInstance* scratch) noexcept {
    const auto entries = slot.SortEntries();
    const auto src     = slot.Instances();

    if (!wasSorted) {                                     // fast path: recording order == draw order, contiguous
        u32 expected = 0; bool contiguous = true;
        for (const SortEntry& e : entries) {
            if (e.instanceIndex != expected) { contiguous = false; break; }
            expected += e.instanceCount;
        }
        if (contiguous) return src.subspan(0, std::min<usize>(expected, src.size()));
    }
    u32 out = 0;
    for (const SortEntry& e : entries) {
        if (e.instanceIndex >= src.size()) continue;
        const u32 n = std::min({static_cast<u32>(e.instanceCount),
                                static_cast<u32>(src.size()) - e.instanceIndex,
                                FrameArenaSlot::kMaxInstances - out});
        std::memcpy(scratch + out, src.data() + e.instanceIndex, n * sizeof(QuadInstance));
        out += n;
    }
    return {scratch, out};                                // packet.instances points here: no second memcpy
}

// ---- Batcher.cpp: coalesce without silent corruption (fixes R4) ------------------
struct CoalesceResult { u32 batches = 0; u32 droppedBatches = 0; };

CoalesceResult CoalesceBatches(FrameArenaSlot& slot) noexcept {
    const auto entries = slot.SortEntries();
    auto batches = slot.MutableBatches();
    CoalesceResult r{};
    u32 offset = 0;
    bool overflowed = false;                              // once a batch is dropped, NEVER merge again
    for (const SortEntry& e : entries) {
        const u32 count = e.instanceCount;
        if (count == 0) continue;
        const u8  pass  = static_cast<u8>((e.key >> 60) & 0x0F);
        const u8  blend = static_cast<u8>((e.key >> 33) & 0x07);
        const u16 pipe  = static_cast<u16>((e.key >> 21) & 0x0FFF);
        const u16 tex   = static_cast<u16>((e.key >> 5) & 0xFFFF);
        const u8  view  = static_cast<u8>(e.viewSlot);

        if (!overflowed && r.batches > 0) {
            DrawBatch& p = batches[r.batches - 1];
            if (p.passId == pass && p.blendMode == blend && p.pipelineId == pipe &&
                p.textureSlot == tex && p.viewSlot == view) {
                p.instanceCount += count;                 // DrawBatch::instanceCount must be 32-bit
                offset += count;
                continue;
            }
        }
        if (r.batches < FrameArenaSlot::kMaxBatches) {
            batches[r.batches++] = DrawBatch{offset, count, tex, pipe, blend, pass, view, 0};
        } else {
            overflowed = true;
            ++r.droppedBatches;                           // FrameStats.droppedBatches; debug assert
        }
        offset += count;
    }
    slot.SetBatchCount(r.batches);
    return r;
}
```

`Batcher::Finalize` becomes: `SortEntries(...)` (unchanged) → `GatherInstances(...)` → `CoalesceBatches(...)`; `outPacket.instances = gathered;` and `FrameStats` gains `droppedBatches` and `droppedEntries`. `FrameArena` keeps a single slot; `AdvanceSlot()` and the second `FrameArenaSlot` are deleted until threaded extraction exists.

### 4.6 Frame driver, presentation policy, backend seam

```cpp
// ---- Presentation policy: one owner, unrepresentable invalid combinations ---------
enum class PacingMode : u8 { VSync, Capped, Uncapped };
struct PresentationPolicy { PacingMode mode = PacingMode::VSync; u32 capFps = 0; };   // capFps used by Capped only

struct PresentResult { bool occluded = false; u64 cpuNs = 0; };

// ---- RenderBackend: Execute and Present are separate (fixes P1) -------------------
class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual void WaitForNextFrame() noexcept {}                                   // waitable swap chain; no-op elsewhere
    virtual Result<void, EngineError>          Resize(u32 pixelW, u32 pixelH) noexcept = 0;
    virtual Result<void, EngineError>          Execute(const FramePacket&) noexcept = 0;   // draw, never present
    virtual Result<PresentResult, EngineError> Present(const PresentationPolicy&) noexcept = 0;
};

// Software cap lives here, NOT in TimeManager. Only active in Capped mode.
class FramePacer { public: void SetTargetFps(u32 fps) noexcept; void Wait() noexcept; void Reset() noexcept; };

class Presenter {
public:
    void SetPolicy(const PresentationPolicy& p) noexcept {
        m_policy = p;
        m_pacer.SetTargetFps(p.mode == PacingMode::Capped ? p.capFps : 0);
        m_pacer.Reset();
    }
    [[nodiscard]] const PresentationPolicy& Policy() const noexcept { return m_policy; }
    void BeginFrame(RenderBackend& b) noexcept { b.WaitForNextFrame(); }          // before the clock is sampled
    [[nodiscard]] Result<PresentResult, EngineError> EndFrame(RenderBackend& b) noexcept {
        auto r = b.Present(m_policy);
        if (m_policy.mode == PacingMode::Capped) m_pacer.Wait();
        return r;
    }
private:
    PresentationPolicy m_policy{};
    FramePacer         m_pacer{};
};

// ---- The engine-owned frame: ordering is enforced by structure --------------------
struct EngineContext {
    class Platform*     platform;     class Window*     window;   class InputSystem* input;
    TimeManager*        time;         AssetManager*     assets;   FrameArenaSlot*    arena;
    RenderQueue*        queue;        class Batcher*    batcher;  RenderBackend*     backend;
    Presenter*          presenter;    class Profiler*   profiler; SimEvents*         events;
};

template <class G>
concept GameLoop = requires(G g, const FrameInput& fi, const TickInput& ti, TickContext& tc,
                            const SimEvents& ev, f64 dt, f32 alpha, RenderQueue& q, const AssetReader& ar) {
    { g.OnFrameInput(fi) } -> std::same_as<void>;        // UI, window commands, dev commands (queued for tick start)
    { g.BeginTick() }      -> std::same_as<void>;        // snapshot Interpolated<T>: runs EVERY tick
    { g.Tick(ti, tc) }     -> std::same_as<void>;        // 120 Hz, deterministic
    { g.Update(dt, alpha, ev) } -> std::same_as<void>;   // cosmetic, real-time
    { g.Extract(q, alpha, ar) } -> std::same_as<void>;
};

// Skeleton: Platform/Input/Profiler calls are shown as comments where their APIs are not in the bundle.
template <GameLoop G>
[[nodiscard]] Result<void, EngineError> RunFrame(EngineContext& e, G& game) noexcept {
    e.presenter->BeginFrame(*e.backend);                  // 0  waitable wait / (Capped: pacing happens after Present)
    // 1-2  clock sample + platform pump + input.Ingest   (time->BeginFrame() samples the clock)
    // 3    e.assets->Commit();                           //    the only asset mutation point
    // 4    game.OnFrameInput(e.input->PublishFrameInput());
    e.events->Clear();
    TickContext ctx{*e.time, *e.events};
    // 6    while (e.time->ConsumeTick()) {
    //          const TickInput in = e.input->CutTick();
    //          game.BeginTick(); game.Tick(in, ctx);     //    Hitstop().AdvanceTick() runs inside time->EndTick()
    //          e.time->EndTick();
    //      }
    // 7    e.time->EndFrame();  const f32 alpha = static_cast<f32>(e.time->GetAlpha());
    const f32 alpha = 0.0f; const f64 realDt = 0.0;
    game.Update(realDt, alpha, *e.events);                // 8
    // 9-10 game.Extract(*e.queue, alpha, e.assets->Reader()); e.batcher->Finalize(*e.arena, packet);
    FramePacket packet{};
    if (auto r = e.backend->Execute(packet); !r) return Err(r.Error());   // 11
    auto p = e.presenter->EndFrame(*e.backend);                            // 12
    if (!p) return Err(p.Error());
    // 13   e.profiler->EndFrame(...)
    return {};
}
```

`main.cpp` shrinks to: create platform/window/backend/assets, build `Game`, `while (running) { if (auto r = RunFrame(ctx, game); !r) { LOG_ERROR(...); break; } }`, then reverse-order teardown. Frame-rate hotkeys call `presenter.SetPolicy({PacingMode::Capped, 144})`; `SetFramerateMode` and `RenderDeviceConfig.vsync` are deleted (P2).

### 4.7 Stage-1 fixes in the existing `SDL_Renderer` backend (R1, R2, R3, P1)

```cpp
// ---- Stage 1: RenderDevice (SDL_Renderer) fixes -----------------------------------
// RenderDevice::Create(): textures are premultiplied by contract.
//     SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
//
// RenderDevice::Submit(), per batch — textured batches use the TEXTURE's blend mode (SDL_render.c:560-566):
static SDL_BlendMode ToSdlBlend(BlendMode m) noexcept {
    switch (m) {
        case BlendMode::Opaque:   return SDL_BLENDMODE_NONE;
        case BlendMode::Multiply: return SDL_BLENDMODE_MUL;
        case BlendMode::Max:      /* no SDL preset: log once, fall through */
        default:                  return SDL_BLENDMODE_BLEND_PREMULTIPLIED;   // Alpha and Additive (alpha == 0)
    }
}
//     const SDL_BlendMode bm = ToSdlBlend(static_cast<BlendMode>(batch.blendMode));
//     if (batchTexture) SDL_SetTextureBlendMode(batchTexture, bm);           // queued with the draw command
//     else              SDL_SetRenderDrawBlendMode(m_renderer, bm);
//
// FlipX/FlipY (QuadInstance.h:962-963) — swap UVs when expanding the quad:
//     float u0 = ..., u1 = ..., v0 = ..., v1 = ...;
//     if (q.kindFlags & QuadFlag_FlipX) std::swap(u0, u1);
//     if (q.kindFlags & QuadFlag_FlipY) std::swap(v0, v1);
//
// ViewSlot: precompute a 2x3 affine per view slot once per Execute(); apply to (cx,cy) and to the rotated
// corner offsets in the existing expansion loop (RenderDevice.cpp:1582-1595). Pre-size m_vertexScratch /
// m_indexScratch to kMaxInstances*4 / *6 at Init() to remove per-batch resize().
//
// Split Submit(): Execute(packet) = clear + draw; Present() = SDL_RenderPresent (RenderDevice.cpp:1634).
```

Color note: `Color::ToPremultipliedRGBA8` stays; add `PackAdditive` (§4.4). Textures must be premultiplied at load (§3.6 item 6), or sprite edges will show dark fringes (A9).

### 4.8 Stage-2: D3D11 backend (illustrative) and instanced quad shader

```cpp
// ILLUSTRATIVE — not compiled in this audit (Windows headers unavailable here). Error handling abbreviated.
class D3D11Backend final : public RenderBackend {
public:
    [[nodiscard]] static Result<void, EngineError> Init(D3D11Backend& out, SDL_Window* window, bool debug) noexcept;
    void WaitForNextFrame() noexcept override {
        if (m_latencyWaitable) WaitForSingleObjectEx(m_latencyWaitable, 1000, TRUE);   // before the clock sample
    }
    Result<void, EngineError> Resize(u32 w, u32 h) noexcept override {
        if (w == 0 || h == 0) return {};                                                // minimized: skip
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_backbufferRtv.Reset();                                                        // release ALL backbuffer refs
        if (FAILED(m_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, m_swapFlags)))   // flags must match creation
            return Err(EngineError{"ResizeBuffers failed"});
        return CreateBackbufferView();
    }
    Result<PresentResult, EngineError> Present(const PresentationPolicy& p) noexcept override {
        const bool vsync = p.mode == PacingMode::VSync;
        UINT flags = 0;
        if (!vsync && m_tearingSupported && !m_exclusiveFullscreen) flags |= DXGI_PRESENT_ALLOW_TEARING;
        const HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, flags);
        if (hr == DXGI_STATUS_OCCLUDED) return PresentResult{.occluded = true};
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)           // v1: fatal, with reason
            return Err(EngineError{"D3D11 device lost (see GetDeviceRemovedReason)"});
        if (FAILED(hr)) return Err(EngineError{"Present failed"});
        return PresentResult{};
    }
    Result<void, EngineError> Execute(const FramePacket& pk) noexcept override {
        // 1. one upload per frame; packet.instances is already in draw order
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(m_ctx->Map(m_instanceBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return Err(EngineError{"Map(instance buffer) failed"});
        std::memcpy(m.pData, pk.instances.data(), pk.instances.size_bytes());          // sequential, write-only
        m_ctx->Unmap(m_instanceBuf.Get(), 0);

        // 2. frame-constant state
        const UINT stride = sizeof(QuadInstance), offset = 0;
        m_ctx->IASetInputLayout(m_quadLayout.Get());
        m_ctx->IASetVertexBuffers(0, 1, m_instanceBuf.GetAddressOf(), &stride, &offset);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);          // 6 verts from SV_VertexID

        // 3. per batch: bind only what changed
        u16 curTex = 0xFFFF; u8 curBlend = 0xFF, curView = 0xFF, curPass = 0xFF;
        for (const DrawBatch& b : pk.batches) {
            if (b.passId   != curPass)  { BindPass(pk, b.passId);   curPass  = b.passId; }
            if (b.viewSlot != curView)  { BindView(pk, b.viewSlot); curView  = b.viewSlot; }
            if (b.blendMode!= curBlend) { m_ctx->OMSetBlendState(m_blend[b.blendMode].Get(), nullptr, 0xFFFFFFFF); curBlend = b.blendMode; }
            if (b.textureSlot != curTex){ BindTexture(b.textureSlot); curTex = b.textureSlot; }  // SRV + per-slot sampler
            m_ctx->DrawInstanced(6, b.instanceCount, 0, b.instanceOffset);              // StartInstanceLocation = offset
        }
        return {};
    }
    // Destroy(TextureHandle): if the SRV may still be bound, unbind (PSSetShaderResources null), then Reset().
    // D3D11 keeps in-flight resources alive until the GPU is done: no fence / deferred-delete queue.
private:
    Result<void, EngineError> CreateSwapChain(HWND hwnd, u32 w, u32 h) noexcept {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc = {1, 0};  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;      d.Scaling = DXGI_SCALING_NONE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        m_swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                      (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);   // IDXGIFactory5::CheckFeatureSupport
        d.Flags = m_swapFlags;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(m_factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &d, nullptr, nullptr, &sc1)) ||
            FAILED(sc1.As(&m_swapChain)))
            return Err(EngineError{"CreateSwapChainForHwnd failed"});
        m_swapChain->SetMaximumFrameLatency(1);
        m_latencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);                  // SDL owns fullscreen transitions
        return CreateBackbufferView();
    }
    // HWND: SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
    // Input layout over QuadInstance (per-instance, step rate 1), total 48 B:
    //   INST_POS   R32G32_FLOAT        @0    INST_SIZE  R32G32_FLOAT        @8    INST_ROT   R32G32_FLOAT @16
    //   INST_UV    R16G16B16A16_UNORM  @24   INST_COLOR R8G8B8A8_UNORM      @32
    //   INST_KIND  R32_UINT            @36   INST_PARAM R32G32_FLOAT        @40
    HANDLE m_latencyWaitable = nullptr;  UINT m_swapFlags = 0;  bool m_tearingSupported = false, m_exclusiveFullscreen = false;
    // ComPtr<> members: m_device, m_ctx, m_factory, m_swapChain (IDXGISwapChain2), m_backbufferRtv,
    //                   m_instanceBuf (DYNAMIC, kMaxInstances*48 B), m_quadLayout, m_blend[BlendMode::Count], ...
};
```

```hlsl
// ILLUSTRATIVE sketch. Matrix convention: row-vector mul(v, M); upload gViewProj accordingly.
cbuffer View : register(b0) { float4x4 gViewProj; float2 gViewportPx; float2 _pad; };
Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);                       // point or linear: chosen per texture slot by the backend

struct Inst {
    float2 pos : INST_POS;   float2 size : INST_SIZE;  float2 rot : INST_ROT;     // rot = (cos, sin)
    float4 uv  : INST_UV;                                                          // UNORM16 -> [0,1] (u0,v0,u1,v1)
    float4 col : INST_COLOR;                                                       // premultiplied; additive => a == 0
    uint   kind: INST_KIND;  float2 param : INST_PARAM;                            // low 8 bits kind; bit8 FlipX, bit9 FlipY
};
struct VSOut { float4 sv : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0;
               nointerpolation uint kind : KIND; float2 local : LOCAL; nointerpolation float2 param : PARAM; };

static const float2 kCorner[6] = { float2(-.5,-.5), float2(.5,-.5), float2(.5,.5),
                                   float2(.5,.5),  float2(-.5,.5), float2(-.5,-.5) };

VSOut VSMain(uint vid : SV_VertexID, Inst i) {
    const float2 c = kCorner[vid];
    const float2 l = c * i.size;
    const float2 w = i.pos + float2(l.x * i.rot.x - l.y * i.rot.y, l.x * i.rot.y + l.y * i.rot.x);
    float2 t = c + 0.5;                                                            // (0,0) = top-left, matches SDL path
    if (i.kind & (1u << 8)) t.x = 1.0 - t.x;
    if (i.kind & (1u << 9)) t.y = 1.0 - t.y;
    VSOut o;
    o.sv = mul(float4(w, 0, 1), gViewProj);
    o.uv = lerp(i.uv.xy, i.uv.zw, t);  o.col = i.col;  o.kind = i.kind & 0xFF;  o.local = l;  o.param = i.param;
    return o;
}
float4 PSMain(VSOut i) : SV_Target {                                               // output is PREMULTIPLIED
    switch (i.kind) {
        case 0:  return gTex.Sample(gSamp, i.uv) * i.col;                          // Sprite (texels premultiplied at load)
        case 1:  return i.col;                                                     // RectFilled
        // case 3 Circle / 4 Capsule / 5 Line: SDF from i.local, i.param ; 6 MSDFText ; 7 NineSlice
        default: return i.col;
    }
}
```

Backend notes: bind a 1×1 white texture to slot 0 so untextured kinds need no branch on the CPU; one sampler per texture slot (point for pixel art, linear for filtered art); per-view constant buffer updated with `Map(WRITE_DISCARD)` only when the view slot changes; build with the D3D11 debug layer in debug configurations and call `ReportLiveDeviceObjects` after releasing everything at shutdown.

---

## 5. Testing and Performance Validation

### 5.1 Test infrastructure (prerequisites)

| Need | Mechanism | Notes |
|---|---|---|
| Deterministic time | `TimeManager::BeginFrameManual(ns)` (exists) driven by **frame-time traces** (CSV of ns deltas): `60fps`, `144fps`, `240fps`, `30fps`, `jittered 60 (±1 ms)`, `stutter (one 120 ms frame / 10 s)`, `focus-loss gap (2 s)` | Core tests need no SDL window |
| Replay | Record `TickInput` per tick (+ initial state); replay through `Sim.Tick` headlessly; `SimHash(world)` after N ticks | Hash = FNV-1a over quantized state |
| Null backend | `RenderBackend` that records the `FramePacket` | Exercises Extract → Finalize without a GPU |
| Golden images | D3D11 **WARP** (`D3D_DRIVER_TYPE_WARP`) or SDL software renderer → offscreen target → readback → compare | Tolerance ≤ 1/255 per channel for exact paths |
| Allocation counter | Replace global `operator new/delete` in the test binary; count after warm-up | T-ALLOC-01 |
| Sanitizers | MinGW-w64 GCC generally lacks an ASan/UBSan runtime — **verify on your toolchain**; run the platform-independent core tests (time, assets, batcher, sort) in a Linux CI job with `-fsanitize=address,undefined` | The D3D11 backend uses the debug layer + `ReportLiveDeviceObjects` instead |

### 5.2 Correctness tests

| ID | Subject | Setup → Expected | Today |
|---|---|---|---|
| **T-TIME-01** | Accumulator exactness | 10⁶ frames of random dt (mean 8.33 ms) via `BeginFrameManual`; total ticks == `floor(Σdt·120)` ±1; accumulator never exceeds 1 tick after consuming | pass (expected) |
| **T-TIME-02** | Catch-up cap | dt = 1 s → exactly 8 ticks, `WasTimeDropped()`, residual `< 1 tick`, `alpha ∈ [0,1)`; next normal frame resumes cleanly | pass (expected) |
| **T-TIME-03** | **Hitstop tick-exactness** | `Request{Player, 3_f60}` at tick *k*: Player channel `dt == 0` for exactly 6 ticks (k+1…k+6) under **every** trace; also while paused/unpaused | **fails** (T1) |
| **T-TIME-04** | Pause / refocus | pause 5 s → resume: ≤ 1 tick burst; focus-gap trace: no more than `maxCatchupTicks`; pacer deadline reset | unknown (T6) |
| **T-TIME-05** | Time scale | scale 0.25 on Player: per-tick dt == `fixedDt·0.25`; 120 ticks advance 0.25 s of Player time; tick count unaffected | pass (expected) |
| **T-DET-01** | Determinism | Scripted 60 s input; sim hash after final tick identical across all frame-time traces | **fails** (S2, T1) |
| **T-INT-01** | **Frozen entity is still** | During hitstop, sample `Get(α)` for α ∈ {0, .25, .5, .75, 1}: all equal | **fails** (S1) |
| **T-INT-02** | Snap | `Snap(p)` then render: position == `p` for all α (no streak) | n/a (new) |
| **T-INT-03** | Endpoints & clamp | `Get(0)==prev`, `Get(1)==cur`, `Get(1.5)==cur`, `Get(-1)==prev` | n/a (new) |
| **T-IN-01** | Edge delivery | One physical press → exactly one tick with `Pressed`, for frames with 0, 1, 2, 8 ticks | contract (not audited) |
| **T-IN-02** | Replay equivalence | Record `TickInput` at 144 FPS, replay headless: identical hash and identical emitted `SimEvent` sequence | **fails** (S2) |
| **T-ASSET-01** | Stale handle | Load A → Unload → Load B (reuses slot): `Resolve(oldA)` returns fallback; new handle returns B; `Find(idA)` is null | pass (expected) |
| **T-ASSET-02** | Generation exhaustion | 4,096 free/alloc cycles of one slot → slot retired, `retiredSlots == 1`, one log line; capacity−1; no crash | counter missing (A2) |
| **T-ASSET-03** | Lookup consistency | 10⁶ random `Allocate/Free/Find` vs `std::unordered_map` oracle (exercises backward-shift deletion); `SetReady` with mismatching `cold.id` → debug assert, identity preserved | **may fail** (A1) |
| **T-ASSET-04** | Normalization | `"A/b.png"`, `"a\\B.PNG"`, `"./a//b.png"` → same `AssetId`; `"../x"` rejected | n/a (new; fails today, A3) |
| **T-SORT-01** | Sort stability | N ∈ {0,1,2,1000,65,536}; random keys with many duplicates; result equals `std::stable_sort` oracle (including `instanceIndex` order of ties) | pass (expected) |
| **T-SORT-02** | Monotonic early-out | pre-ordered input → `wasSorted == false`, array unchanged | pass (expected) |
| **T-BATCH-01** | Coalescing | adjacent same-state merge; non-adjacent not merged; `Σ batch.count == Σ entry.count`; offsets contiguous | pass (expected) |
| **T-BATCH-02** | **Overflow integrity** | 5,000 alternating-state entries with cap 4,096: every instance in drawn batches belongs to the state it was recorded with; `droppedBatches == 5000 − 4096`; none merged after first drop | **fails** (R4; reproduced on a reduced model) |
| **T-BATCH-03** | Malformed entries | out-of-range / overlapping `SortEntry` → rejected at push or clamped; `FrameStats.dropped*` incremented; no OOB | latent (R5) |
| **T-REND-01** | Blend semantics (golden) | 50 % red over blue; additive white glow over mid-gray; **textured** additive sprite; multiply; opaque. Compare to analytic premultiplied math | **fails** for textured additive (R1) and alpha < 1 (R2) |
| **T-REND-02** | Flip / rotation / pivot (golden) | FlipX, FlipY, rotation about pivot, scale | FlipX/Y ignored (R3) |
| **T-REND-03** | Soft-edge sprites | sprite with 50 % alpha edges over bright/dark backgrounds: no dark/light fringe | unknown (A9) |
| **T-REND-04** | Virtual resolution & DPI | same scene at 100/150/200 % scale and at 1280×720 → 3840×2160: logical output identical; letterbox bars correct; `IntegerFit` has no sub-pixel scaling | **fails** (S4/S5, expected) |
| **T-REND-05** | Resize / fullscreen / minimize stress | 1,000 toggles (windowed ↔ borderless, resize drags, minimize/restore): no crash; `ReportLiveDeviceObjects` shows no delta in live objects | n/a (D3D11) |
| **T-PRES-01** | Policy matrix | VSync / Capped(60,144,240) / Uncapped × windowed/borderless: policy honored, `Present` flags as designed, no double throttle | n/a (new) |
| **T-LIFE-01** | Shutdown | Exit normally and via early init failure paths: no use-after-free (sanitizer where available); D3D debug layer clean; `Shutdown()` twice is a no-op | pass (expected) |
| **T-ALLOC-01** | Zero-allocation hot path | After 60 warm-up frames, 10⁴ frames of Tick → Extract → Finalize → Execute(null) → Present(null): **0** allocations | `vector::resize` may allocate (R10) |

### 5.3 Edge cases to cover explicitly

* Zero-tick frames at 240+ FPS (alpha only) and N-tick frames after a hitch; `ConsumeTick` called after `EndFrame`.
* Hitstop requested **during** a frozen tick; overlapping requests on different channels; request on the final frozen tick.
* Time-scale change mid-frame via dev command (applies at next tick start); scale 0.
* Teleport on the same tick a hitstop begins; entity spawned mid-frame (must not interpolate from origin).
* Camera cut at room transition with `Snap` on camera and followed entities.
* Window minimized (0×0), occluded present (`DXGI_STATUS_OCCLUDED`), display change / DPI change mid-run, monitor sleep.
* Texture slot recycled between two frames while a sprite referencing the old slot is still in the *next* frame's extraction (must resolve through handles, not cached slots).
* Arena/instance overflow at the exact boundary (`kMax` and `kMax+1`); sort of exactly `kMaxSortEntries`.
* Asset load failure → fallback rendering and one log per asset; `Failed` asset re-load path.
* Uncapped mode with tearing unavailable; Capped at a cap above the display refresh.

### 5.4 Metrics to collect (always-on ring buffer, dumpable to CSV)

| Group | Metric | Why |
|---|---|---|
| Frame | **frame interval** (QPC at `Present` return), p50/p95/p99/p99.9/max; **missed refresh** count (interval > 1.5× refresh) | Measures real pacing, replaces P4's single sample |
| CPU stages (ns) | pump, assets.Commit, ticks (count + each cost), update, extract, sort, gather, coalesce, backend CPU, **present wait** | Attributes time |
| Simulation | ticks/frame histogram; catch-up (`dropped time`) events; per-tick cost p99 | Spiral-of-death margin |
| Recording | quads, sort entries, **batches**, texture/blend/view/pass switches, draw calls, upload bytes | Batching quality |
| Overflow | dropped quads/entries/batches, `SimEvents` dropped, fallback resolves | No silent loss |
| GPU (D3D11) | `TIMESTAMP_DISJOINT` GPU ms per pass | Separates CPU- from GPU-bound |
| Memory | arena high-water marks per frame; heap allocation count in hot path | Right-size caps (§3.7) |
| Visual smoothness | **judder metric**: for a constant-velocity test entity, std-dev of per-presented-frame Δposition (normalized by expected Δ); also for camera scroll | Evidence for/against dt smoothing (T8) |
| Latency | scripted press → tick that consumed it (in ticks); optional end-to-end with a photodiode/high-speed camera | Interpolation cost is ≈ 1 tick; verify |

### 5.5 Benchmark protocol (sort / gather / batch)

1. **Inputs:** `N ∈ {1k, 5k, 20k, 65k}` entries × key distributions: *(a)* already sorted (tile layers), *(b)* random, *(c)* few distinct states with long runs, *(d)* y-sorted actors (many unique depths, few textures).
2. **Candidates:** current radix; `std::stable_sort` on `key`; `std::sort` on `(key, seq)`.
3. **Method:** pinned core, 200 warm-up + 2,000 timed iterations per cell, report median and p99, same compiler flags as release; also time `GatherInstances` (`48·N` bytes) separately.
4. **Decision rule (proposed):** keep radix only if it is ≥ 1.5× faster than `std::stable_sort` at the smallest `N` where sort ≥ 5 % of the frame budget on the target low-spec machine; otherwise replace it (less code, same semantics). Report results in the roadmap R6 PR.

### 5.6 Acceptance criteria (proposed gates)

**Calibration rule:** thresholds marked ◇ are **proposals**; they become binding only after R0 records baselines on reference machines. Non-◇ criteria are exact.

| ID | Criterion | Applies to |
|---|---|---|
| A1 | Sim hash identical across **all** frame-time traces for the 60 s script (T-DET-01) | R2 |
| A2 | Hitstop frozen-tick count equals requested ticks under all traces — deviation 0 (T-TIME-03) | R2 |
| A3 | Frozen entity: max position spread across α sweep == 0 (T-INT-01) | R1 |
| A4 | T-REND-01/02 golden images within ≤ 1/255 per channel (exact paths) | R1, R7 |
| A5 | T-BATCH-02 passes; `dropped*` counters always surfaced in the overlay and logged once per drop episode | R1, R6 |
| A6 | 0 heap allocations in the hot path over 10⁴ frames (T-ALLOC-01) | R6, R7 |
| A7 | Batch count for the reference scenes ≤ number of distinct `(pass, layer-run, blend, pipeline, texture, view)` runs in the sorted order (no regression vs. oracle) | R6 |
| A8 | ◇ p99 tick cost ≤ 2 ms on the lowest-spec target (≥ 4× headroom to the 8.33 ms tick period) | R2 |
| A9 | ◇ p99 (extract + sort + gather + coalesce) ≤ 1.0 ms at 20k quads on reference hardware | R6 |
| A10 | ◇ On a 60 Hz VSync display, scene S2 (8 parallax layers, ~4k tiles, ~2k particles, ~50 sprites): ≤ 1 missed refresh per 10 min | R4, R7 |
| A11 | ◇ Judder metric on VSync not worse than baseline from R0 after R4; any smoothing experiment must beat it by a margin set at R0 | R4 |
| A12 | 1,000 resize/fullscreen/minimize cycles: no crash, zero live-object delta (T-REND-05) | R7 |
| A13 | Premultiplied pipeline: no fringing on T-REND-03 sprite (visual + pixel diff) | R1 |

**Performance claims policy.** No change in this document may be described as an optimization in a PR until the corresponding metric in §5.4 shows the delta against the R0 baseline on the same machine and scene.

---

## 6. Refactoring Roadmap

Principles: every step is **independently shippable**, starts with a **failing test** where a bug is confirmed, changes **one subsystem boundary at a time**, and keeps the app runnable. Steps are ordered by *risk removed per unit of change*; R0 is a prerequisite for claiming any improvement.

### 6.1 Dependency graph

```
R0 baseline & tests ─┬─► R1 correctness fixes (SDL backend, batcher, snapshot) ──┬─► R3 view / virtual resolution ─┐
                     │                                                          │                                  ├─► R7 D3D11 backend (optional gate)
                     ├─► R2 tick-correct simulation boundary ───────────────────┤                                  │
                     │                                                          └─► R4 presenter & pacing ─────────┤
                     ├─► R5 assets (identity, AssetSet, lifetime) ──────────────────────────────────────────────────┤
                     └─► R6 batcher/arena simplification + sort decision ───────────────────────────────────────────┘
```
R1–R2 can proceed in parallel after R0. R3 needs R1 (`ViewSlot` consumption). R7 needs R1, R3, R4, R6 and the golden-image harness from R0.

### 6.2 Steps

| Step | Scope (symbols) | Depends on | Regression risk → mitigation | Completion criteria |
|---|---|---|---|---|
| **R0 — Baseline & harness** *(no behavior change)* | Ring-buffer profiler; read `FrameStats` and log drops; **fix telemetry** (separate Present wait, `main.cpp:2021-2022`); frame-trace replay via `BeginFrameManual`; null backend; golden-image harness (SDL software or WARP); allocation counter; CI job for core tests (Linux + sanitizers if MinGW lacks them); log `SDL_GetRendererName` on target machines; capture baseline CSVs (frame interval, stage times, judder) for scenes S1/S2 | — | Minimal (additive) | Tests T-TIME-01/02/05, T-SORT-01/02, T-BATCH-01 green; baseline CSVs committed; A-criteria ◇ calibrated |
| **R1 — Correctness fixes** | **R1/R2:** `SDL_SetTextureBlendMode` per batch + `BLEND_PREMULTIPLIED`, `PackAdditive` (§4.7) · **R3:** FlipX/Y UV swap · **R4/R5:** `CoalesceBatches` overflow rule + `droppedBatches`, `PushSortEntry` validation, gather w/o copy-back · **S1:** unconditional snapshot (interim: move `prev = pos` above the `dt > 0` test, `main.cpp:1915`) · **A1:** `SetReady` preserves id · **A9:** premultiply on load | R0 | Visual change in alpha/additive sprites (intended). Mitigate with golden images reviewed once; premultiply-on-load changes texel data — verify with T-REND-03 | T-BATCH-02, T-REND-01/02/03, T-INT-01 (via interim fix), T-ASSET-03 green; A3, A4, A5, A13 met |
| **R2 — Tick-correct simulation boundary** | Hitstop in ticks (`HitstopRequest::ticks`, `AdvanceTick`, delete `Update(realDt)`); `TickContext`; `Interpolated<T>` + `World::BeginTick`; move attack/dash/hitstop/time-scale decisions into `Tick`; dev-command queue drained at tick start; per-entity animation clocks; `SimEvents`; delete `m_accQ16`, `m_effScaleQ16`, `GetDomainAlpha`, `maxFrameNs`; `kSimHz` compile-time | R0 (R1 helpful) | **Gameplay feel changes** (attack start time, hitstop length now exact). Mitigate: record input before/after; tune authored `_f60` durations; keep old behavior behind a temporary flag for one release | T-TIME-03, T-DET-01, T-IN-02, T-INT-01/02/03 green; A1, A2, A8 met |
| **R3 — View & virtual resolution** | Consume `ViewSlot`/`ViewTransform2D` in the SDL backend (2×3 affine); `ViewportScaler` (`Fit/IntegerFit/Cover`); DPI-aware backbuffer size; remove `config.window*` from gameplay (`main.cpp:1937-1964`) | R1 | Different on-screen scale at non-100 % DPI (intended). Verify on 100/150/200 % displays | T-REND-04 green; F11 toggle leaves world/gameplay unchanged |
| **R4 — Presenter & pacing** | `PresentationPolicy`/`Presenter`; split `Execute`/`Present`; move `FramePacer` out of `TimeManager`; delete `SetFramerateMode`, `RenderDeviceConfig.vsync`, unused `PresentMode::Mailbox`; check/log VSync failures; **A/B** `SDL_DelayPrecise` vs the 1 ms spin; wire `ResetClocksOnRefocus` | R0 (needs frame-interval metrics) | Pacing behavior can regress silently. Gate on A10/A11 against the R0 baseline; keep the old pacer behind a flag until measured | T-PRES-01, T-TIME-04 green; A10, A11 met |
| **R5 — Assets** | `AssetPath` normalization; single-key `LoadTexture(path)`; `AssetSet`; `Commit()` as sole mutation point; retirement counter + log; non-movable `AssetManager`/backend (`Init()` two-phase); capacity coupling assert; async **not** started | R0 | Asset ids change (hash of normalized path) ⇒ any persisted ids/save data referencing old hashes break. Mitigate: ids are runtime-only today (verify); provide a one-shot migration if not | T-ASSET-01/02/03/04, T-LIFE-01 green |
| **R6 — Batcher/arena simplification + sort decision** | Single `FrameArenaSlot`; delete `AdvanceSlot`; arena caps from config; `RenderQueue` + `SortPolicy` layers; run §5.5 benchmark and **decide** radix vs `std::stable_sort`; pre-size SDL scratch (remove per-batch `resize`) | R1, R0 | Key/layer constants change ordering if misassigned. Mitigate with golden scenes and A7 oracle | T-ALLOC-01, T-SORT-01, T-BATCH-0x green; A6, A7, A9 met; benchmark report attached |
| **R7 — D3D11 backend** *(gated)* | `D3D11Backend : RenderBackend` (device, flip swap chain, waitable object, instanced quads, blend states, texture table, debug layer); parity with the SDL backend via goldens; then render-target/post passes as separate follow-ups. **Gate:** needs custom shaders or pacing control (feature list in §3.10) **and** Windows-only scope confirmed (else `SDL_GPU`) | R1, R3, R4, R6, R0 goldens | New failure classes: device lost, HLSL tooling in MinGW, resize/flags mismatches. Mitigate: debug layer in CI (WARP), `ReportLiveDeviceObjects`, fatal-on-device-loss in v1 | T-REND-01/02/04/05 green on D3D11; A4, A10, A12 met; SDL backend kept as fallback until parity confirmed, then optionally retired |

### 6.3 Ordering rationale

1. **R0 first**: the current telemetry (P4) cannot show improvement or regression; baselines make every later claim checkable.
2. **R1 before R2/R3**: removes visible rendering bugs and the S1 sawtooth with the smallest diff, and creates the golden images later steps compare against.
3. **R2 before R7**: simulation determinism and the tick/interpolation contract are independent of the backend and are where gameplay risk lives; do them while the backend is still the simple one.
4. **R7 last and gated**: highest cost and portability implication; justified only by features that `SDL_Renderer` cannot provide.

### 6.4 Explicitly deferred (FUTURE — do not build now)

* Async asset loading and hot-reload watchers (seam: `AssetManager::Commit`).
* Threaded extraction/batching (would re-introduce a second arena slot).
* Frame-interval smoothing / refresh-snapped dt (only if the judder metric demands it).
* Retained tile-chunk geometry (only if tilemap extraction dominates a profile).
* Linear-light / sRGB pipeline; render-target pool, bloom, 2D lights.
* Device-lost recovery (v1: fatal with a clear log).
* VRR-aware capping; bindless/texture-array batching.

---

## Appendix A — Deletions and simplifications (net code reduction)

| Remove | Reason | Finding |
|---|---|---|
| `TimeChannel::m_accQ16`, `m_effScaleQ16`, `GetDomainAlpha`, Q16 helpers (after repo-wide grep) | No consumer; wrong semantics | T2 |
| `TimeConfig::maxFrameNs`, `HitstopEngine::Update(realDt)`, runtime `simulationHz` | Redundant / wrong unit / unnecessary | T3, T1, T5 |
| `FramePacer` inside `TimeManager`; `SetFramerateMode`; `RenderDeviceConfig.vsync` | Presentation concern; triple ownership | T7, P2 |
| `PresentMode::Mailbox` | Unimplementable on current backend | P3 |
| Second `FrameArenaSlot`, `AdvanceSlot` | No consumer | R6 |
| `LinearizeInstances` copy-back | Redundant copy | R5 |
| `LoadTexture(AssetId, const char*)` two-key API | Key divergence | A3 |
| `config.windowWidth/Height` in gameplay/extraction | Resolution coupling | S4 |

## Appendix B — Source index (symbols cited)

| File (bundle lines) | Symbols |
|---|---|
| `TimeManager.h` (1-173) | `kDefaultSimHz:13`, `_f60:18`, `TimeChannel:56`, `HitstopEngine:77`, `FramePacer:96`, `TimeConfig:109`, `TimeManager:115` |
| `TimeManager.cpp` (175-392) | `HighResolutionClock:190`, `CommitTick:220`, `HitstopEngine::Update:232`, `FramePacer::WaitForTarget:253`, `Init:277`, `BeginFrameManual:302`, `UpdateDomainDeltas:322`, `ConsumeTick:333`, `EndTick:346`, `GetDomainAlpha:383` |
| `AssetTypes.h` (395-483) | `HashPath:411`, `AssetHandle:441`, `SpriteView:477` |
| `AssetTable.h` (486-679) | `Allocate:531`, `SetReady:551`, `Free:562`, `RemoveLookup:635` |
| `AssetReader.h` (682-757) | `Resolve:697`, `Sprite:706` |
| `AssetManager.h` (760-816) | `LoadTexture:796`, `Unload:799`, `m_frameStore:812` |
| `RendererTypes.h` (819-939) | `Color:826`, `TextureHandle:883`, `BlendMode:901`, `PresentMode:909`, `ViewTransform2D:926` |
| `QuadInstance.h` (942-1003) | `QuadFlags:960`, `QuadInstance:966`, `SetColor:997` |
| `SortKey.h` (1006-1050) | `Pack:1015`, `SortEntry:1041` |
| `FrameArena.h` (1053-1140) | caps `1066-1070`, `FrameArena:1124` |
| `Batcher.cpp` (1165-1349) | `SortEntries:1172`, `LinearizeInstances:1222`, `CoalesceBatches:1263`, `Finalize:1315` |
| `RenderDevice.{h,cpp}` (1352-1637) | `Init:1432`, `Create:1475`, `Destroy:1524`, `Submit:1547` |
| `main.cpp` (1640-2043) | frame loop `1862-2037`; hitstop/input `1885-1909`; ticks `1910-1944`; extraction `1947-2016`; telemetry `2021-2035`; shutdown `2038-2041` |

## Appendix C — References and evidence

**Primary sources inspected during this analysis** (SDL `main` branch; re-verify on your pinned version):
* `libsdl-org/SDL` — `src/render/SDL_render.c` (`PrepQueueCmdDraw` blend selection `:553-603`; texture default blend `:1671`; `SDL_CreateGPURenderState` requirements `:6259-6277`), `src/render/direct3d11/SDL_render_d3d11.c` (swap chain, tearing, VSync), `src/timer/SDL_timer.c` (`SDL_GetTicksNS`, `SDL_DelayPrecise`), `include/SDL3/SDL_blendmode.h` (`BLEND`, `BLEND_PREMULTIPLIED` definitions), `include/SDL3/SDL_render.h` (`SDL_SetRenderDrawBlendMode` scope, `SDL_SetRenderVSync`).
* Microsoft Learn, *Reduce latency with DXGI 1.3 swap chains* — https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains (waitable object, `SetMaximumFrameLatency`, flag consistency across `ResizeBuffers`, wait before first `Present`).
* NVIDIA, *Advanced API Performance: Swap Chains* — https://developer.nvidia.com/blog/advanced-api-performance-swap-chains (flip model, latency control, tearing flag pair).

**General-practice references (cited by name; not re-fetched in this session):** Glenn Fiedler, *Fix Your Timestep!* (Gaffer On Games) — fixed tick + accumulator + interpolation; Christer Ericson, *Order your graphics draw calls around!* — packed sort keys; Shawn Hargreaves, *Premultiplied alpha* — premultiplied compositing and the additive (alpha = 0) trick; Andre Weissflog, *Handles are the better pointers* — generational handles. Treat these as background, not as evidence for any EngineN-specific claim.

**Reproductions run for this report** (reduced models, not the real tree): (1) hitstop frozen-tick counts vs frame rate (T1); (2) batch-cap overflow merge vs the proposed rule (R4). Compile checks: snippets 4.1–4.5 and the `GameLoop` concept against a stub header (g++ 13.3, `-std=c++20 -Wall -Wextra`).

## Appendix D — Open questions for the owner

1. **Is Windows the only shipping platform?** (decides D3D11 vs `SDL_GPU`, §3.10)
2. **Pixel-art or HD-filtered art?** (pixel snapping, sampler choice, `IntegerFit` default)
3. **Which driver does `SDL_GetRendererName` report on your target machines today?** (confirms whether the current backend is already D3D11)
4. **Are `AssetId`s persisted anywhere** (saves, tools)? (R5 migration)
5. **Does `TickInput` expose per-action edges, and does `CutTick` guarantee one-tick delivery for 0- and N-tick frames?** (§3.4, T-IN-01)

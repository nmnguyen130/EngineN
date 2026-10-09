# EngineN — Milestone 03: Renderer Subsystem

Design, trade-offs, zero-allocation strategy, test plan and roadmap.
Status legend: **[verified]** = compiled and executed in a sandbox (g++ 13, `-std=c++20 -fno-exceptions`, ASan/UBSan) · **[specified]** = header contract written, implementation is a roadmap phase · **[verify on your machine]** = depends on SDL3 / GCC 16 behaviour I could not run here.

---

## 1. Decision summary

| Question | Decision |
|---|---|
| Backend strategy | **Option C**: backend-agnostic `RenderDevice` + `RenderContext`, with **`SDL_Renderer` + `SDL_RenderGeometryRaw` as the first (and Milestone 03 only) backend**. `SDL_GPU` becomes a second `IRenderBackend` later without touching gameplay code. |
| Who sorts and batches | The **engine**, not SDL. One 64-bit key, stable radix sort, run merging, one backend submission per frame. |
| Vertex | One canonical **20-byte** `Vertex {x, y, u, v, rgba8}` for sprites and primitives, already in virtual-screen space. |
| Alpha model | **Premultiplied** end to end (textures premultiplied once at load, vertex colors premultiplied at record time). |
| Primitives | Sample a built-in 1×1 white texture, so they share draw calls and painter order with sprites. |
| Camera boundary | Renderer consumes only a `ViewTransform2D` value. Tracking, damping, shake stay in Milestone 06. |
| Overflow policy | Fixed arenas; overflow **drops the draw and counts it** (`FrameStats::droppedDraws`). Never grows, never allocates. |
| Init convention | Two-phase `Create()/Init()` + RAII destructors, matching `Window`/`Platform`/`TimeLoop`. |

---

## 2. Backend trade-off matrix

| Criterion | A: thin `SDL_Renderer` wrapper | B: `SDL_GPU` directly | **C: abstraction, SDL_Renderer first** |
|---|---|---|---|
| Time to a correct, tested renderer | days | weeks (shader toolchain, pipelines, buffers, sync) | days |
| Toolchain friction on MinGW/w64devkit | none | high: shaders must exist as SPIR-V/DXIL/MSL at build time | none |
| 10k+ sprites @ 240 FPS | fine if you batch yourself | best ceiling | fine (same data path as A) |
| Custom shaders / post-FX / lighting | limited by SDL_Renderer | full | deferred to a later backend |
| Risk of gameplay code touching SDL types | high | high | **none** (opaque handles, value types) |
| Cost of switching later | rewrite call sites | n/a | add one `IRenderBackend` |
| Architectural purity | low | medium (couples engine to GPU API early) | high |

**Why not B now.** A Metroidvania at this stage needs sprites, tiles, primitives, HUD and hit-flash. All of that fits a vertex-colored, textured-triangle pipeline. `SDL_GPU` earns its cost at lighting, bloom, palette shaders and distortion, which are not in Milestone 03. Taking on shader compilation and pipeline state before the renderer's *data model* is proven is the expensive order of operations.

**Why not A.** A thin wrapper leaves sorting, state grouping and zero-allocation guarantees to whatever `SDL_Renderer` does internally. You would be unable to test them, and call sites would accumulate `SDL_*` assumptions.

**Why C costs almost nothing.** The seam is exactly one batch per frame: `BatchView { vertices, indices, runs, scissors }`. Everything above the seam (sort key, queue, primitives, sprites, view transform) is pure CPU code that is identical for both backends. **[verified]** (the whole layer above the seam is unit-tested without a device).

---

## 3. Responsibilities

### `RenderDevice` (device lifetime, resources, presentation)
- Creates the backend (`Create(window, cfg)` / `CreateHeadless(cfg)` for tests and CI).
- `BeginFrame()` clears the backbuffer (letterbox bars included); `Present()` swaps. Out-of-order calls return `RenderErrc::FrameOrderViolation`.
- **VSync**: `Off / On / Adaptive`; `Caps().vsyncAdaptive` tells you if adaptive is real.
- **Logical presentation**: `SetPresentation(w, h, Disabled | Stretch | Letterbox | Overscan | IntegerScale)`. `PresentationRect()`, `WindowToVirtual()` / `VirtualToWindow()` give mouse mapping. For a pixel-art look use a small virtual canvas + `IntegerScale`; for HD art use a larger canvas + `Letterbox`.
- **Texture table**: generational handles (`u16 index | u16 generation`). Slot 0 = none, slot 1 = built-in white. Stale handles are detected, never dereferenced.
- **Capabilities**: backend/driver name, max texture size, adaptive VSync, custom-shader support.
- `Submit(queue)`: the single hand-off to the backend per frame.

### `RenderContext` (per-frame state and recording)
- State: view transform, blend mode, tint, `(pass, layer, depth, material)`, scissor, pixel snap.
- **Matrix handling**: `SetView(ViewTransform2D)` caches `WorldToScreen()` once. `SetScreenSpace()` switches to identity for HUD. `PushState()/PopState()` (fixed depth 16, `ScopedState` RAII) save and restore the whole state, not just the matrix.
- Vertices are transformed **at record time** into virtual-screen space. `SDL_Renderer` has no user matrix for geometry, and the same data feeds `SDL_GPU` later (a uniform matrix would simply replace the CPU transform there).
- Recording is order-independent: the sort key decides painter's order, so UI recorded first still lands on top.
- `EndFrame()`: `Finalize()` (sort + merge) then one `device.Submit()`.

---

## 4. Core types ([verified] unless noted)

- **`Color`** — float RGBA, default white. `FromRGBA8`, `FromHex(0xRRGGBBAA)`, `ToPacked()` (memory order R,G,B,A), `Premultiplied()/Unpremultiplied()`, `Lerp`, `operator*` (modulate). NaN-safe byte conversion; no implicit sRGB↔linear conversion (values are used as authored).
- **`Rect` / `FRect`** — half-open `[x, x+w)`. Touching edges don't intersect. Over-shrinking `Expanded()` collapses to a zero-size rect at the center, never to negative size. NaN-safe `IsEmpty()`.
- **`Mat2D`** — 2×3 affine, column-vector convention, `A*B` = apply B first. `TryInverse()` rejects singular and non-finite matrices.
- **`Transform2D`** — `world = position + R·S·(local − pivot)`. `Lerp` is component-wise with **shortest-arc rotation** (`std::remainder`), because matrix lerp shears rotations. Gameplay must set `prev = current` on teleports.
- **`ViewTransform2D`** — `{viewport, center, zoom, rotation, screenSpace}`; `WorldToScreen()`, `ScreenToWorld()`, `Lerp()`. This is the entire contract with the Milestone 06 camera.
- **Enums** — `BlendMode {Opaque, Alpha, Premultiplied, Additive, Multiply}`, `TextureFilter`, `PresentationMode`, `VSyncMode`, `RenderPass {Background, World, Overlay, Ui}`, `SpriteFlip`.

---

## 5. Batching pipeline

```
record (any order)                         finalize (EndFrame)                     submit
─────────────────                          ───────────────────                     ──────
DrawSprite / Primitives.*                  stable LSD radix sort of                 Backend.Submit(
  ├─ Reserve(v, i)  ─┐                       SortEntry{key, item}                   vertices,   (20 B each)
  ├─ write vertices  │ transactional       copy item indices in sorted order ──►     indices,    (u32, sorted)
  └─ Commit(key,     ┘ (no commit = no     merge neighbours with equal              runs,       (≈ draw calls)
      scissorState)    consumption)          (texture, blend, material, scissor)      scissors)
```

### 64-bit sort key

```
[63:60] pass 4   Background < World < Overlay < Ui
[59:52] layer 8  parallax / tile / entity layers
[51:36] depth 16 quantized 0 (far) .. 1 (near); y-sorting
[35:32] blend 4
[31:24] material 8   reserved for effects/shader variants
[23: 8] texture 16   slot index (0 none, 1 white)
[ 7: 0] reserved     ignored by the sort
```

- **Painter's order is `pass → layer → depth`.** Texture/blend/material sit *below* depth, so batching never overrides visual correctness. Inside one `(pass, layer, depth)` bucket, items group by blend then texture, which is what removes state thrashing for tile layers.
- **Sequence number is unnecessary**: the radix sort is stable, so submission order is the final tie-break by construction **[verified against `std::stable_sort` for 13 sizes × 3 key distributions]**.
- **Run merging ignores pass/layer/depth.** Layers 3 and 4 using the same atlas and blend become one draw call, with indices still in painter order **[verified]**. A→B→A across layers correctly stays 3 runs.
- **Cost**: 7 radix passes max; passes whose digit is uniform are skipped; `n ≤ 32` uses stable insertion sort. Measured here: **0.197 ms** to sort + merge 10,000 randomly keyed quads into 2,034 runs (sandbox CPU, not yours). Real frames merge far better than random keys.

### Primitives with zero allocation
Each emitter reserves exactly the vertices/indices it needs in the pre-allocated arena, writes them, and commits. Temporaries are stack values with compile-time bounds (`kMaxCircleSegments = 128`, `kMaxPolygonPoints = 64`, `kMaxGridLines = 2048`).

| Emitter | Vertices / indices | Notes |
|---|---|---|
| `FillRect` | 4 / 6 | |
| `StrokeRect` | 8 / 24 | centered stroke; inner collapse degrades to a filled rect |
| `Line` | 4 / 6 | butt caps; zero-length draws nothing |
| `FillTriangle`, `FillPolygon` | 3 / 3, n / 3(n−2) | convex fan |
| `FillCircle` | n+1 / 3n | n from on-screen radius (0.25 px sagitta tolerance) |
| `StrokeCircle` | 2n / 6n | inner radius clamps at 0 |
| `FillCapsule`, `StrokeCapsule` | N+1 / 3N, 2N / 6N | one convex outline; `a==b` degrades to a circle |
| `Grid` | 4·lines / 6·lines | single reservation: all-or-nothing |

Circle resolution follows zoom automatically through `RecordState::unitsPerPixel`.

### Vertex layout
`struct Vertex { f32 x, y, u, v; u32 rgba; }` = **20 bytes**, `static_assert`ed. This is also the layout `SDL_GPU` wants (RGBA8 unorm color). The `SDL_Renderer` backend needs `SDL_FColor` colors, so it expands `rgba → SDL_FColor` into a backend-owned scratch array at submit time (16 B/vertex, positions and UVs are passed in place via strides). That conversion is the price of backend neutrality, and it is a flat loop.

### Sprite recording
Quad corners come from `view * transform.ToMatrix()` (pivot honored), UVs from `TextureRegion` with `SpriteFlip`. A viewport AABB test culls off-screen sprites (counted in `culledDraws`). Per-sprite `depth` override supports y-sorting without changing context state. Non-finite input is rejected before anything is reserved.

---

## 6. Zero-allocation and zero-exception strategy

- `RenderQueue::Init()` makes **one** allocation (`new (std::nothrow)`) for all arenas, 64-byte aligned, carved into vertex / index / sorted-index / item / entry×2 / run / scissor regions. Defaults ≈ 12 MiB (262,144 vertices, 393,216 indices, 65,536 items).
- After `Init`, `BeginFrame / Reserve / Commit / InternScissor / Finalize` never allocate. **[verified]**: a test replaces global `operator new`, runs 20 frames × ~4,000 mixed draws (sprites + every primitive + scissor changes + sort + merge), and asserts **0 allocations**.
- No `std::vector`, no `std::function`, no virtual calls in recording. The only virtual dispatch is `IRenderBackend::Submit`, once per frame.
- **Errors**: init-time and resource-time operations return `RenderStatus` (`Result<bool, EngineError>`) with a `RenderErrc` code. Hot-path draws return `bool` (drew / didn't) and count drops in `FrameStats`; there is nothing to propagate mid-frame and no exception path. `RenderStatus` is a single alias in `RendererTypes.h`; if your `Result` supports `void`, change that one line.
- **Load-time allocations are allowed**: texture premultiplication uses a staging copy. Offline-premultiplied assets can set `premultiplyAlpha = false`.
- **For the real backend**, assert the contract end-to-end with `SDL_SetMemoryFunctions` in the soak test (Phase 5). SDL's internal command queue may grow during warm-up; the test must discard the first N frames.

---

## 7. Test strategy

### Implemented: `tests/test_renderer.cpp` **[verified]**
54 tests / 920 checks, no framework, device-free, compiled with `-fno-exceptions`, clean under `-Wall -Wextra -Wpedantic`, also clean under ASan + UBSan.

| Group | Highlights |
|---|---|
| Color | hex round-trip for all channels, packed byte order, NaN/±Inf/negative clamping, premultiply round-trip, `a=0` unpremultiply |
| Rect/FRect | touching edges, negative sizes, union with empties, over-shrink collapse, NaN rects |
| Math | composition order, 1,000 random inverse round-trips, singular/NaN/Inf inverse leaves output untouched, pivot invariant, shortest-arc across ±π, view center/zoom/rotation/degenerate zoom |
| Sort key | field isolation (no bleed), strict priority order, depth quantization monotone + NaN |
| Radix | oracle comparison vs `std::stable_sort` (n ∈ {0…10,000}), all-equal stability, descending, single-byte, reserved byte ignored |
| Queue | transactional reserve, absolute indices, overflow of each arena stays valid, painter order, run merge rules (layers, texture grouping, blend, material, scissor, scissor-table overflow), determinism across frames, 10k stress |
| Texture | handle packing, exhaustive premultiply (all 65,536 combos vs `lround`), region UV mapping and rejection of bad rects |
| Primitives | exact areas (rect, stroke, line, triangle), circle/ring/capsule areas within polygon tolerance, degenerate/NaN rejection, matrix/tint/premultiply/pixel-snap, overflow never commits partial geometry, all-emitters index validity |
| Sprites | UV + all four flips, pivot/rotation/scale, culling edge cases, depth override, view matrix |
| Allocation | zero heap allocations across the full record→finalize path |

**Mutation check [verified]:** I broke the code five ways (unstable insertion sort, ignoring scissor in run merging, non-absolute indices, non-shortest-arc angle lerp, unclamped ring inner radius). The suite failed in every case.

### Planned (needs `RenderContext.cpp` / backend), Phase 1–5
1. State stack: push/pop restores view, scissor, blend, tint, layer; overflow/underflow handled; `ScopedState` restores on scope exit.
2. UI recorded before world still presents after it (end-to-end through a headless device).
3. `VisibleWorldRect()` matches the inverse-mapped viewport corners under rotation.
4. Headless texture lifecycle: stale-handle rejection after destroy and slot reuse, `maxTextures` exhaustion → `TextureLimitReached`, double destroy is a no-op.
5. `FrameOrderViolation` for `Present()` without `BeginFrame()`.
6. SDL integration (needs a window): golden-image smoke scene, letterbox/integer-scale mouse round-trip, resize/fullscreen toggles, 10-minute alloc soak.

---

## 8. SDL_Renderer backend mapping (Phase 2–3) **[verify on your machine]**

| Engine concept | SDL3 call |
|---|---|
| Device creation | `SDL_CreateRenderer(window, driverHint or nullptr)` |
| VSync | `SDL_SetRenderVSync(renderer, 0 / 1 / SDL_RENDERER_VSYNC_ADAPTIVE)` |
| Presentation | `SDL_SetRenderLogicalPresentation(renderer, w, h, mode)` with `STRETCH / LETTERBOX / OVERSCAN / INTEGER_SCALE` |
| Mouse mapping | `SDL_RenderCoordinatesFromWindow / ToWindow` |
| Texture | `SDL_CreateTexture(RGBA32, STATIC or STREAMING)` + `SDL_UpdateTexture`; `SDL_SetTextureScaleMode` |
| Blend per run | `SDL_SetTextureBlendMode`: Premultiplied → `BLEND_PREMULTIPLIED`, Alpha → `BLEND`, Additive → `ADD_PREMULTIPLIED`, Multiply → `MUL`, Opaque → `NONE` |
| Scissor per run | `SDL_SetRenderClipRect` (null to disable) |
| Draw one run | `SDL_RenderGeometryRaw(r, tex, &v[0].x, 20, colors, 16, &v[0].u, 20, nVerts, idx + run.firstIndex, run.indexCount, 4)` |
| Present | `SDL_RenderPresent` |

Check these against the exact SDL3 version you vendor (signatures such as the `SDL_FColor` color pointer and the presentation enum names have shifted across 3.x releases). Then benchmark. `SDL_RenderGeometryRaw` cost per call is the reason the engine merges runs itself.

---

## 9. `main.cpp` integration sketch

```cpp
EngineN::RenderDevice device;
device.Create(window, { .vsync = VSyncMode::On,
                        .presentation = PresentationMode::Letterbox,
                        .virtualWidth = 480, .virtualHeight = 270 });
EngineN::RenderContext ctx;
ctx.Init(device);

// ... inside the frame, after the fixed-step loop:
const f32 alpha = static_cast<f32>(time.GetAlpha());
device.BeginFrame();
ctx.BeginFrame();
ctx.SetView(camera.Interpolated(alpha));            // Milestone 06 supplies this
ctx.SetLayer(RenderPass::World, 3);
ctx.DrawSprite({ .region = playerFrame,
                 .transform = Transform2D::Lerp(player.prevTransform, player.transform, alpha) });
ctx.SetScreenSpace();
ctx.SetLayer(RenderPass::Ui, 0);
ctx.Primitives().FillRect({8, 8, 64, 6}, Colors::Red);
ctx.EndFrame();
device.Present();
time.Pacer().WaitForTarget();
```

---

## 10. Roadmap with exit criteria

| Phase | Work | Exit criterion |
|---|---|---|
| **0** (½ day) | Drop headers into `src/renderer/`; add `tests/test_renderer.cpp` target (below); reconcile `Result`/`Vec2` signatures. | Tests pass with MinGW GCC 16. |
| **1** | `RenderContext.cpp` (state stack, `Rebuild*`, `EndFrame`, `VisibleWorldRect`), `NullBackend`, `RenderDevice.cpp` (slot table, frame-order checks), `Texture.cpp`. Add planned tests 1–5. | Headless frame loop runs; all headless tests green. |
| **2** | `SdlRendererBackend`: init, VSync, presentation, texture create/update/filter/destroy. | Cleared window with letterbox bars; textured quad on screen. |
| **3** | Submit mapping (§8): geometry runs, blend, scissor. | Golden scene: sprites, tiles, every primitive, additive VFX, scissored panel. |
| **4** | Integrate into `main.cpp`; remove raw `SDL_Renderer`; hook resize/DPI. Demo box interpolated at 60/120/144/240 Hz. | Motion stays smooth at all refresh rates; mouse maps correctly under letterbox and integer scale. |
| **5** | Performance gate + soak. | On your reference machine: 10k sprites + 2k primitives, record + finalize + submit **≤ 1.0 ms** CPU (240 FPS = 4.17 ms budget); 10-minute soak with `SDL_SetMemoryFunctions` shows 0 allocations after warm-up. |
| **6** | Hardening: minimize/restore, device loss, DPI change, window-mode switch; docs; tag milestone. | Checklist in `TASKS.md` closed. |

CMake test target:
```cmake
add_executable(test_renderer tests/test_renderer.cpp)
target_include_directories(test_renderer PRIVATE src)
target_compile_features(test_renderer PRIVATE cxx_std_20)
target_compile_options(test_renderer PRIVATE -fno-exceptions -Wall -Wextra -Wpedantic)
add_test(NAME renderer COMMAND test_renderer)
```
The test file replaces global `operator new` (for the allocation test), so keep it as its own executable.

---

## 11. Known limits and explicit non-goals

- **Not verified here**: any SDL code, `RenderDevice/Context/Texture` `.cpp` files (headers only), MinGW GCC 16, and your real `Result`/`Vec2` signatures (I used minimal stand-ins built from your spec; the two adaptation points are the `RenderStatus` alias and `Vec2(f32, f32)`).
- **Pixel snapping** rounds final vertex positions. It is right for axis-aligned pixel art; rotated sprites will show slight distortion, so use it per context state (`SetPixelSnap`).
- **Linear-filtered atlases** need extruded padding in the asset pipeline to avoid bleeding. The renderer exposes UV regions but does not pad.
- **Tiling/repeat textures** (parallax) are not part of this milestone. Pre-tile strips or emit multiple quads; address modes arrive with the `SDL_GPU` backend.
- **Polyline joins, text, lighting, render targets, custom shaders** are out of scope. Each has a clean extension point (`material` key bits, a new `IRenderBackend` method, or a new pass), but none is built.
- **Overflow drops draws** by design. Tune arenas via `RenderQueueConfig` using `FrameStats` and assert `droppedDraws == 0` in debug builds.

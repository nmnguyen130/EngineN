# EngineN — Renderer Subsystem Context (`engine/renderer/`)

## 1. Architecture & Invariants
- **Core:** C++23, zero heap allocation in hot paths, 5-file footprint (`RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp`), two-layer design (Pure C++ Core + SDL3 Hardware Presentation Adapter).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`RenderList`):** Pure C++23 command recorder, view-space frustum culler, 6-byte LSD radix sorter, state coalescer, and vertex expander. Zero SDL headers; 100% testable headless via `ComputeAuditHash()`.
  - **Layer 2 (`Renderer`):** SDL3 hardware coordinator & **Asset Sink**. Owns `SDL_Renderer` and generational texture pool (`kMaxTextures = 4096`). `<SDL3/...>` headers are strictly confined to `Renderer.cpp`.
- **Self-Contained 64-Bit `SortKey`:**
  - Layout: `[Pass:4][Layer:8][Depth:16][Blend:2][View:4][TextureIdx:14][QuadIdx:16]`.
  - Embedding `QuadIdx` in bits [0..15] eliminates separate sort indices, guarantees stable submission order, and restricts radix sort to bytes [2..7] (at most 6 passes).
- **$O(1)$ Histogram Accumulation & Fast Path:** Histograms (`m_histograms[6][256]`) accumulate during `PushQuad`. Monotonic frames skip radix sort entirely; byte passes where all keys share identical byte value are skipped in $O(1)$.
- **Early View-Space Frustum Culling:** `PushQuad` computes $L_1$ view-space bounding radius (`|hxX| + |hyX|`, `|hxY| + |hyY|`) against viewport. Culled quads incur zero sorting or vertex overhead.
- **Generational Texture Sink & Slot 0 White Texture:** Slot `0` (`kWhiteTexture = {0, 1}`) is permanently bound to a `1x1` opaque white texture. Stale or destroyed handles safely fallback to Slot `0`.
- **Zero-Conversion Streaming:** `Vertex2D` (32B) and `Color` (16B premultiplied float) match SDL3 binary layouts. `Submit` streams chunks of 4096 quads directly into `SDL_RenderGeometryRaw`.

## 2. Core Layouts & Types (`RendererTypes.h`)
- `TextureHandle`: `{u16 index, u16 generation}` (`kWhiteTexture = {0, 1}`).
- `TextureDesc`: `width`, `height`, `format` (`RGBA8`, `BGRA8`), `filter` (`Nearest`, `Linear`), `pixels` (`span<const u8>`), `pitch`.
- `RendererErrc`: `DeviceInitFailed`, `TextureCreateFailed`, `TextureUpdateFailed`, `InvalidHandle`, `PoolExhausted`, `InvalidDimension`.
- `Color` (`alignas(16)`, 16B): Premultiplied `{r, g, b, a}` float. Helpers: `Premultiplied()`, `FromRGBA8(r,g,b,a)`.
- `Vertex2D` (`alignas(16)`, 32B): `{x, y, Color, u, v}`. Binary-compatible with `SDL_Vertex`.
- `QuadCmd`: Center `(x, y)`, `(w, h)`, precomputed `(cosRot, sinRot)`, `uv`, `color`, `texture`, `depth`, `passId`, `layer`, `viewSlot`, `blend`, `flipX`, `flipY`.
- `PreparedQuad` (`alignas(64)`, 64B): View-space transformed quad cacheline.
- `Camera2D`: `worldCenterX`, `worldCenterY`, `zoom`, `rotationRad`, `viewport`, `scissor`, `screenSpace`.
- `RenderPassDesc`: `clearColor`, `clear`, `enabled`.
- `RenderStats`: `submittedQuads`, `culledQuads`, `droppedQuads`, `drawBatches`, `radixPassesRun`, `monotonicFastPath`.

## 3. Subsystem APIs
### `RenderList` (`engine/renderer/RenderList.h` — Command Buffer & Batch Compiler)
- `Reset(Extent2i size)`: Clears buffer, sets default screen-space views & passes.
- `SetPass(passId, RenderPassDesc)`, `SetView(viewSlot, Camera2D)`
- `PushQuad(QuadCmd)`, `PushRect(rect, color, ...)`, `PushLine(x0, y0, x1, y1, thickness, color, ...)`
- `Compile()`: Runs 6-byte LSD radix sort (if non-monotonic) and coalesces batches.
- `ExpandVertices(firstQuad, quadCount, span<Vertex2D>)`: Generates 4 vertices per quad.
- `ComputeAuditHash()` $\to$ `u64`: Deterministic 64-bit FNV-1a state hash.

### `Renderer` (`engine/renderer/Renderer.h` — Hardware Adapter & Asset Sink)
- **Lifecycle & Configuration:**
  - `Init(Window&, RendererConfig)` $\to$ `Result<void, RendererErrc>`, `Shutdown()`
  - `ResizeViewport(Extent2i)`, `SetPresentMode(PresentMode)` $\to$ `Result<void, RendererErrc>`
- **Generational Texture Pool:**
  - `CreateTexture(TextureDesc)` $\to$ `Result<TextureHandle, RendererErrc>`
  - `UpdateTexture(handle, region, pixels, pitch)` $\to$ `Result<void, RendererErrc>`
  - `DestroyTexture(handle)`, `IsTextureValid(handle)` $\to$ `bool`
- **Frame Submission:**
  - `BeginFrame()` $\to$ `RenderList&`: Resets internal `RenderList`.
  - `Submit()`, `Submit(RenderList&)`: Compiles list, sets camera viewports, clears passes, and submits chunks via `SDL_RenderGeometryRaw`.

## 4. Main Loop Integration
```cpp
auto& renderList = m_renderer.BeginFrame();
// Extraction binds CameraRig2D view slots and emits quads
world::WorldExtractor::Extract(m_world, m_assetBank, m_time.GetRenderView(), renderList);
m_renderer.Submit(renderList);
```
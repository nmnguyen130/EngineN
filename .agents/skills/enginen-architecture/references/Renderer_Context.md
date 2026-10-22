# Phase 5 — `renderer` Subsystem (LOCKED — Final Industry Production v2)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()`, minimal 5-file footprint (`RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp`) + `tests/RendererTests.cpp`, two-layer pure C++23 separation (`namespace en`, zero redundant `using` aliases or sub-namespace wrappers).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Includes:**
  - **Layer 1 (`RendererTypes.h`, `RenderList.h/.cpp`):** Pure C++23 command recorder, `yUp`-correct pivot & rotation transformer, tight rotated-AABB frustum & hierarchical scissor culler (`PushClipRect`/`PopClipRect`), O(1) XOR-variance 6-byte LSD radix sorter, single-pass batch coalescer, true 3x3 9-slice UV patch generator, Gouraud 2-color gradient expander, tiled sprite emitter, and half-op corner-offset vertex expander. Depends solely on `"core/Math.h"` — zero OS, `<SDL3/SDL_*.h>`, `assets/`, or `platform/` includes. 100% deterministic in headless CI via 64-bit word-mixed `RenderList::ComputeAuditHash()`.
  - **Layer 2 (`Renderer.h/.cpp`):** Move-only RAII SDL3 hardware presentation coordinator, offscreen render-target FBO manager (`SDL_TEXTUREACCESS_TARGET` + `SDL_SetRenderTarget`), and generational **Asset Sink**. Owns `SDL_Renderer` and a 16-byte aligned `SDL_Texture` slot pool (`kMaxTextures = 4096`). `Renderer.h` has zero `platform/` includes (using `ViewportExtent` implicit structural conversion and forward-declared `en::Window`); `<SDL3/SDL.h>` and `"platform/Window.h"` are strictly confined to `Renderer.cpp`.
- **L2/L3-Resident Footprint (`sizeof(RenderList) == 1,328,256 B`, 20,754 cache lines exact, 0B padding; `sizeof(Renderer) == 1,393,856 B`, 21,779 cache lines exact, 0B padding):**
  - **Right-Sized 16-Bit Hardware Vertex Capacity (`kMaxQuads = 16384`, `kMaxBatches = 1024`, `kChunkQuads = 2048`):** Sized directly to the 16-bit GPU index limit (`16,384` quads = `65,536` vertices) so the working set fits inside modern CPU L2/L3 caches.
  - **`RenderList` Cache Line 0 (`offset 0..63`, `alignas(64)`):** Packs all hot recording state (`m_prevKey`, `m_varyingBits`, `m_stats`, `m_viewport`, `m_enabledPassMask`, `m_clearPassMask`, `m_activePassMask`, `m_clipViewStack[6]`, `m_clipStackDepth`, `m_nextClipViewSlot`, `m_activeClipView`, `m_isMonotonic`, `m_compiled`, `m_pixelSnap`) into Cache Line 0 with zero compiler padding and zero dead variables.
  - **`PreparedQuad` (`sizeof == 64`, `alignas(64)`, 0B padding):** Stores pre-summed corner offset vectors (`d0 = hx + hy`, `d1 = hx - hy`) and packed RGBA8 secondary gradient color `c1Rgba8` (`kGradActiveBit`, `kGradHorizontalBit` in upper bits of `texture.index`) so `ExpandVertices()` computes each vertex coordinate in `8` FP ops/quad and supports single-quad 2-color Gouraud gradients.
  - **`DrawBatch` (`sizeof == 16`, `alignas(16)`, 4 batches per 64B cache line, 0B padding)** & **`Renderer::TextureSlot` (`sizeof == 16`, `alignas(16)`, 4 slots per 64B cache line, 0B padding):** While unoccupied (`(flags & kSlotOccupied) == 0`), `slot.width` stores the intrusive `nextFree` index.
- **Single-Pass Active-Byte Histogram Radix Sort & Y-Sort Coalescing (`Compile()`):**
  - `PushQuad()` tracks byte variance in a single CPU register instruction (`m_varyingBits |= (key ^ m_prevKey)`).
  - Monotonic frames (`m_isMonotonic == true`) bypass Stage 1 sorting completely (`0` histogram scans, `0` radix passes). Non-monotonic frames extract only the `numPasses` (`1..6`) byte shifts where `(m_varyingBits >> shift) & 0xFF != 0` under `SortKey::kSortStateMask` (`0xFFFFFFFFFFFF0000ULL`), accumulate all active histograms in **a single linear scan** over `m_keys[0..quadCount)`, and scatter only active passes.
  - Stage 2 coalesces sorted runs using `SortKey::kBatchStateMask` (`0xF000000FFFFF0000ULL`: `Pass | Blend | View | TextureIdx`), allowing already-sorted sprites across different `Layer` and `Depth` values on the same texture atlas to merge into a single GPU draw call without merging incompatible overflow batches.

## 2. Core Types (`renderer/RendererTypes.h`)
- **Viewport & Handles (0B Padding, Zero Aliases):**
  - `struct alignas(8) ViewportExtent` (`8B`): `{i32 w, h}`, `IsValid()`, `AspectRatio()`, implicit `constexpr` conversion from any `{.w, .h}` extent (such as `en::Extent2i`).
  - `using TextureHandle = Handle<TextureTag>` (`4B`, `alignas(4)`): `index` (`u16`), `generation` (`u16`), `IsValid()`. Built-in slots: `kWhiteTexture = {0, 1}`, `kBuiltinFontTexture = {1, 1}`, `kBuiltinSoftLightTexture = {2, 1}`.
  - `struct alignas(8) TextureDesc` (`32B`, `0B` padding): `pixels` (`std::span<const u8>`), `width`, `height`, `pitch`, `format` (`RGBA8`, `BGRA8`), `filter` (`Nearest`, `Linear`, `PixelArt`), `renderTarget`, `premultipliedAlpha`.
  - `enum class RendererErrc : u8`: `None`, `DeviceInitFailed`, `TextureCreateFailed`, `TextureUpdateFailed`, `InvalidHandle`, `PoolExhausted`, `InvalidDimension`.
- **Rendering Primitives (0B Padding):**
  - `struct alignas(16) Vertex2D` (`32B`): `{f32 x, y; Color color; f32 u, v}` binary-identical to `SDL_Vertex`.
  - `struct alignas(16) QuadCmd` (`80B`): `center`, `size`, `pivot` (default `{0.5f, 0.5f}`), `cosRot`, `sinRot`, `uv`, `color`, `texture`, `depth`, `depthBias`, `passId`, `layer`, `viewSlot`, `blend`, `flipX`, `flipY`, `pixelSnap`, `flash8`, `SetRotation(angleRad)`, `SetHitFlash(ratio)`.
  - `struct SortKey`: 64-bit key `[Pass:4][Layer:8][Depth:16][Blend:2][View:4][TextureIdx:14][QuadIdx:16]`, `kSortStateMask`, `kBatchStateMask`, `Pack(cmd, quadIdx)`, `EncodeYDepth(worldY, minY, maxY, zBias)`, `QuadIndex()`, `TextureIndex()`, `ViewSlot()`, `Blend()`, `Depth()`, `Layer()`, `PassId()`.
  - `struct alignas(16) DrawBatch` (`16B`): `firstQuad`, `quadCount`, `texture`, `passId`, `viewSlot`, `blend`, `layer`.
  - `struct alignas(16) Camera2D` (`48B`): `worldCenterX`, `worldCenterY`, `zoom`, `pixelsPerMeter` (`UnitScaleQ8`), `screenSpace`, `yUp`, `viewport`, `scissor`, `ApplyTraumaShake()`, `WorldToViewport()`, `ViewportToWorld()`.
  - `struct alignas(4) RenderPassDesc` (`20B`): `clearColor`, `targetTexIndex` (`0` = default backbuffer, `1..4095` = offscreen render-target FBO texture), `clear`, `enabled`.
  - `struct alignas(4) RenderStats` (`20B`): `submittedQuads`, `culledQuads`, `droppedQuads`, `drawBatches`, `activeClipViews`, `radixPassesRun`, `monotonicFastPath`.
  - `struct alignas(64) NineSliceDesc` (`64B`, 1 L1 cache line exact): `outerRect`, `color`, `borderLeft/Right/Top/Bottom`, `uvBorder`, `cornerScale`, `texture`, `depth`, `blend`, `drawCenter`.

## 3. Public Interfaces & Signatures
### `RenderList` (`renderer/RenderList.h` — Pure C++23 Command Buffer & Batch Compiler)
- `constexpr void Reset(ViewportExtent viewportSize = {1280, 720}) noexcept`
- `constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept`
- `constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept`
- `constexpr void SetScissor(u8 viewSlot, Rect normalizedScissor) noexcept`, `constexpr void SetPixelScissor(u8 viewSlot, Rect pixelScissor) noexcept`
- `constexpr u8 PushClipRect(Rect pixelClip, u8 baseViewSlot = kHudViewSlot) noexcept`, `constexpr void PopClipRect() noexcept`, `[[nodiscard]] constexpr u8 ActiveClipViewSlot() const noexcept`
- `constexpr void SetPixelSnap(bool enabled) noexcept`
- `void PushQuad(const QuadCmd& cmd) noexcept`, `void PushHitFlashQuad(const QuadCmd& baseCmd, f32 flashRatio, Color flashTint = {1, 1, 1, 1}) noexcept`
- `void PushRect(...)`, `void PushGradientRect(...)`, `void PushTiledRect(...)`, `void PushRectOutline(...)`
- `void PushLine(...)`, `void PushPolyline(...)`, `void PushCircleOutline(...)`, `void PushSlashArc(...)`, `void PushBeam(...)`, `void PushRing(...)`, `void PushPointLight(...)`
- `void PushNineSlice(const NineSliceDesc& desc, u8 passId = 0, u8 layer = 20, u8 viewSlot = 0) noexcept`
- `void PushProgressBar(...)`, `void PushText(...)`, `static constexpr Vec2 MeasureText(std::string_view text, f32 scale = 1.0f) noexcept`
- `static u32 ComputeShadowVisibilityPolygon(Vec2 lightPos, f32 lightRadius, std::span<const ShadowOccluder2D> occluders, std::span<Vec2> outPolygonVertices) noexcept`
- `void Compile() noexcept`, `void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept`
- `[[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept`
- `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `Renderer` (`renderer/Renderer.h` — 64B-Aligned Move-Only RAII Hardware Adapter & Asset Sink)
- `[[nodiscard]] Result<void, RendererErrc> Init(Window& window, const RendererConfig& cfg = {}) noexcept`
- `void Shutdown() noexcept`, `[[nodiscard]] constexpr bool IsValid() const noexcept`
- `constexpr void ResizeViewport(ViewportExtent pixelSize) noexcept`, `[[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept`
- `[[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept`
- `[[nodiscard]] Result<TextureHandle, RendererErrc> CreateRenderTarget(u32 width, u32 height, TextureFilter filter = TextureFilter::Linear) noexcept`
- `[[nodiscard]] Result<void, RendererErrc> UpdateTexture(TextureHandle handle, Rect region, std::span<const u8> pixels, u32 pitch = 0) noexcept`
- `void DestroyTexture(TextureHandle handle) noexcept`, `[[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept`
- `[[nodiscard]] RenderList& BeginFrame(ViewportExtent pixelSize = {}) noexcept`, `void Submit() noexcept`, `void Submit(RenderList& list) noexcept`
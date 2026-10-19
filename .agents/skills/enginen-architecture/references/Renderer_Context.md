# EngineN — Renderer Subsystem Context (`renderer/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()`, minimal 5-file footprint (`RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp`) + `tests/RendererTests.cpp`, two-layer pure C++23 separation (`namespace en::renderer`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Includes:**
  - **Layer 1 (`RendererTypes.h`, `RenderList.h/.cpp`):** Pure C++23 command recorder, view-space frustum culler, O(1) XOR-variance 6-byte LSD radix sorter, single-pass batch coalescer, and half-op corner-offset vertex expander. Depends solely on `"core/Result.h"` and `"core/Types.h"` — zero OS, `<SDL3/SDL_*.h>`, or `platform/` includes. 100% deterministic in headless CI via 64-bit word-mixed `RenderList::ComputeAuditHash()`.
  - **Layer 2 (`Renderer.h/.cpp`):** Move-only RAII SDL3 hardware presentation coordinator and generational **Asset Sink**. Owns `SDL_Renderer` and a 16-byte aligned `SDL_Texture` slot pool (`kMaxTextures = 4096`). `Renderer.h` has zero `platform/` includes (using `ViewportExtent` implicit structural conversion and forward-declared `platform::Window`); `<SDL3/SDL_*.h>` and `"platform/Window.h"` are strictly confined to `Renderer.cpp`.
- **L2/L3-Resident Footprint (`sizeof(RenderList) == 1,328,256 B ≈ 1.26 MB`, 75% Smaller) & L1 Cache Line 0 Partitioning:**
  - **Right-Sized 16-Bit Hardware Vertex Capacity (`kMaxQuads = 16384`, `kMaxBatches = 1024`, `kChunkQuads = 2048`):** Sized directly to the 16-bit GPU index limit (`16,384` quads = `65,536` vertices), cutting `sizeof(RenderList)` by 4x (from `5.06 MB` to `1.26 MB`) and `m_vertexScratch` by 2x (from `512 KB` to `256 KB`) so the entire working set fits inside modern CPU L2/L3 caches.
  - **`RenderList` Cache Line 0 (`offset 0..63`, `alignas(64)`):** Packs all hot recording state (`m_prevKey`, `m_varyingBits`, `m_stats`, `m_enabledPassMask`, `m_clearPassMask`, `m_isMonotonic`, `m_compiled`) into Cache Line 0 with zero duplicate counters (`m_stats.submittedQuads` and `m_stats.drawBatches` are the single source of truth).
  - **`PreparedQuad` (`sizeof == 64`, `alignas(64)`, zero implicit padding):** Stores pre-summed corner offset vectors (`d0 = hx + hy`, `d1 = hx - hy`) so `ExpandVertices()` computes each vertex coordinate with a single add/subtract (`8` FP ops/quad instead of `16`).
  - **`DrawBatch` (`sizeof == 16`, `alignas(16)`, 4 batches per 64B cache line, zero padding)** & **`Renderer::TextureSlot` (`sizeof == 16`, `alignas(16)`, 4 slots per 64B cache line, zero straddling):** While a texture slot is free (`texture == nullptr`), `slot.width` stores the intrusive `nextFree` index, shrinking the 4096-slot texture pool from `96 KB` (`24B/slot`) to `64 KB` (`16B/slot`).
- **Single-Pass Active-Byte Histogram Radix Sort (`Compile()`):**
  - `PushQuad()` tracks byte variance in a single CPU register instruction (`m_varyingBits |= (key ^ m_prevKey)`), completely eliminating the `6 KB` `m_histograms` member, per-frame `memset`, and 6-step per-quad histogram loop.
  - Monotonic frames (`m_isMonotonic == true`) bypass Stage 1 sorting completely (`0` histogram scans, `0` radix passes). Non-monotonic frames extract only the `numPasses` (`1..6`) byte shifts where `(m_varyingBits >> shift) & 0xFF != 0`, accumulate all active histograms in **a single linear scan** over `m_keys[0..quadCount)`, and scatter only active passes.
- **Lazy High-Water Texture Pool & State-Diffed Hardware Submission:**
  - `Renderer` uses a lazy high-water mark (`m_highWaterSlot`) + intrusive freelist (`m_freeHead`), eliminating the 4096-slot initialization loop in `Init()` and scanning only `[0 .. m_highWaterSlot)` in `Shutdown()`.
  - `Renderer::Submit()` clears enabled passes via `std::countr_zero(pendingClearMask)` and caches `activeViewport` and `activeScissor` (`FRect`) across view-slot transitions, avoiding redundant `SDL_SetRenderViewport` / `SDL_SetRenderClipRect` calls that would flush SDL3's internal D3D11/D3D12/Metal/Vulkan batcher.

## 2. Core Types (`renderer/RendererTypes.h`)
- **Viewport & Handles (Zero Padding):**
  - `struct alignas(8) ViewportExtent` (`sizeof == 8`): `{i32 w, h}`, `IsValid()`, implicit `constexpr` conversion from any `{.w, .h}` extent (such as `platform::Extent2i`).
  - `struct alignas(4) TextureHandle` (`sizeof == 4`): `index` (`u16`), `generation` (`u16`), `IsValid()`. `kWhiteTexture = {0, 1}`.
  - `struct TextureDesc`: `width`, `height`, `format` (`RGBA8`, `BGRA8`), `filter` (`Nearest`, `Linear`), `pixels` (`std::span<const u8>`), `pitch`.
  - `enum class RendererErrc : u8`: `DeviceInitFailed`, `TextureCreateFailed`, `TextureUpdateFailed`, `InvalidHandle`, `PoolExhausted`, `InvalidDimension`.
- **Rendering Primitives (Zero Implicit Padding):**
  - `struct Color` (`sizeof == 16`, `alignof == 4`): Premultiplied linear float `{r, g, b, a}` matching `SDL_FColor`. Helpers: `Color::Premultiplied()`, `Color::FromRGBA8()`.
  - `struct FRect` (`sizeof == 16`, `alignof == 4`): `{x, y, w, h}`, `IsEmpty()`.
  - `struct alignas(16) Vertex2D` (`sizeof == 32`): `{x, y, Color, u, v}` matching `SDL_Vertex`.
  - `struct QuadCmd`: World/screen center `(x, y)`, size `(w, h)`, precomputed `(cosRot, sinRot)`, `uv`, `color`, `texture`, `depth`, `passId`, `layer`, `viewSlot`, `blend`, `flipX`, `flipY`.
  - `struct SortKey`: 64-bit key `[Pass:4][Layer:8][Depth:16][Blend:2][View:4][TextureIdx:14][QuadIdx:16]`, `kBatchStateMask`, `Pack(cmd, quadIdx)`, `QuadIndex()`, `TextureIndex()`, `ViewSlot()`, `Blend()`, `PassId()`.
  - `struct alignas(16) DrawBatch` (`sizeof == 16`): `firstQuad`, `quadCount`, `texture`, `passId`, `viewSlot`, `blend`, `reserved`.
  - `struct alignas(16) Camera2D` (`sizeof == 48`): `worldCenterX`, `worldCenterY`, `zoom`, `viewport`, `scissor`, `screenSpace`, `reserved[3]` (dead `rotationRad` removed).
  - `struct RenderPassDesc` (`sizeof == 20`): `clearColor`, `clear`, `enabled`, `reserved`.
  - `struct RenderStats` (`sizeof == 24`): `submittedQuads`, `culledQuads`, `droppedQuads`, `drawBatches`, `radixPassesRun`, `monotonicFastPath`, `reserved[3]`.

## 3. Public Interfaces & Signatures
### `RenderList` (`renderer/RenderList.h` — Pure C++23 Command Buffer & Batch Compiler)
- `void Reset(ViewportExtent size) noexcept`
- `constexpr void SetPass(u8 passId, const RenderPassDesc& desc) noexcept`
- `constexpr void SetView(u8 viewSlot, const Camera2D& cam) noexcept`
- `void PushQuad(const QuadCmd& cmd) noexcept`
- `void PushRect(FRect rect, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept`
- `void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept`
- `void Compile() noexcept`
- `void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept`
- `[[nodiscard]] u64 ComputeAuditHash() const noexcept`
- `[[nodiscard]] static std::span<const u16> SharedQuadIndices(u32 quadCount) noexcept`
- Accessors: `Batches()`, `SortedKeys()`, `ClearPassMask()`, `Pass(u8)`, `View(u8)`, `Stats()`.

### `Renderer` (`renderer/Renderer.h` — 64B-Aligned Move-Only RAII Hardware Adapter & Asset Sink)
- Lifecycle & Viewport:
  - `[[nodiscard]] Result<void, RendererErrc> Init(platform::Window& window, const RendererConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`, `[[nodiscard]] constexpr bool IsValid() const noexcept`
  - `constexpr void ResizeViewport(ViewportExtent pixelSize) noexcept`
  - `[[nodiscard]] Result<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept`
- Asset Sink API (For `AssetSystem` & `DebugSystem` Integration):
  - `[[nodiscard]] Result<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept`
  - `[[nodiscard]] Result<void, RendererErrc> UpdateTexture(TextureHandle handle, FRect region, std::span<const u8> pixels, u32 pitch = 0) noexcept`
  - `void DestroyTexture(TextureHandle handle) noexcept`
  - `[[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept`
- Frame Execution:
  - `[[nodiscard]] RenderList& BeginFrame(ViewportExtent pixelSize = {}) noexcept` (single-call optional viewport sync + `RenderList::Reset`)
  - `void Submit() noexcept`, `void Submit(RenderList& list) noexcept`
  - `[[nodiscard]] constexpr const RenderStats& LastFrameStats() const noexcept`

## 4. Canonical Main Loop Usage
```cpp
if (m_window.State().IsPresentable()) {
    en::renderer::RenderList& list = m_renderer.BeginFrame(m_window.State().pixelSize);
    en::world::WorldExtractor::Extract(m_world, m_assets.Bank(), renderTime, list);
    m_sceneDirector.DispatchRenderExtract(renderCtx);
    m_debug.EmitOverlays(m_world, m_gameplay, m_audio, m_assets.Bank(), renderTime, frameInput, m_window.State().pixelSize, list);
    m_renderer.Submit();
}
```
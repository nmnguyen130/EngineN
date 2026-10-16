# EngineN — Renderer Subsystem Context (`renderer/`)

## 1. Architecture & Invariants
- **Standard:** C++23, zero heap allocation in hot paths, minimal 5-file footprint (`RendererTypes.h`, `RenderList.h/.cpp`, `Renderer.h/.cpp`), two-layer separation (Pure C++ Core + SDL3 Hardware Adapter).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`RendererTypes.h`, `RenderList.h/.cpp`):** Pure C++23 command recorder, view-space frustum culler, 6-byte LSD radix sorter, state coalescer, and SIMD-friendly vertex expander. Zero OS or SDL dependencies; 100% testable in headless CI via `RenderList::ComputeAuditHash()`.
  - **Layer 2 (`Renderer.h/.cpp`):** Thin SDL3 hardware presentation coordinator and **Asset Sink**. Owns `SDL_Renderer` and a generational `SDL_Texture` slot pool (`kMaxTextures = 4096`). `<SDL3/SDL_*.h>` headers are strictly confined to `Renderer.cpp`.
- **Self-Contained 64-Bit `SortKey` (In-Register Sorting):**
  - Layout: `Pass (4b) > Layer (8b) > Depth (16b) > Blend (2b) > View (4b) > TextureIdx (14b) > QuadIndex (16b)`.
  - Embedding `QuadIndex` in the lowest 16 bits eliminates separate `SortEntry` structs (50% memory reduction), preserves strict submission order as a tie-breaker, and restricts LSD Radix Sort to bytes `[2..7]` (at most 6 passes).
- **O(1) Histogram Accumulation & Pass Skipping:** Byte histograms (`m_histograms[6][256]`) are accumulated on insertion during `PushQuad`. During `Compile()`, monotonic frames bypass sorting entirely, and any radix byte where `hist[firstByte] == m_quadCount` is skipped in $O(1)$.
- **Early View-Space Frustum Culling:** `RenderList::PushQuad` transforms quads into view-space and performs a branchless $L_1$ bounding-radius AABB check against the active `Camera2D::viewport` prior to allocation. Culled quads incur zero sorting or vertex expansion cost.
- **Zero-Conversion Hardware Submission:** `Vertex2D` (32B) and `Color` (16B premultiplied float) match `SDL_Vertex` and `SDL_FColor` binary layouts 1:1. `Renderer::Submit` streams chunked vertices (`4096` quads/chunk) and a `consteval` `u16` static index buffer (`SharedQuadIndices`) directly into `SDL_RenderGeometryRaw(..., size_indices = 2)`.
- **Generational Asset Sink & Slot 0 White Texture:** Slot `0` (`kWhiteTexture = {0, 1}`) is permanently initialized as a `1x1` opaque white texture. Unloaded or stale `TextureHandle` references automatically fall back to Slot `0` without crashing.

## 2. Core Types (`renderer/RendererTypes.h`)
- **Handles & Descriptors:**
  - `struct TextureHandle { u16 index{0}; u16 generation{0}; };` (`kWhiteTexture = {0, 1}`).
  - `struct TextureDesc`: `width`, `height`, `format` (`RGBA8`, `BGRA8`), `filter` (`Nearest`, `Linear`), `pixels` (`std::span<const u8>`), `pitch`.
  - `enum class RendererErrc : u8`: `DeviceInitFailed`, `TextureCreateFailed`, `TextureUpdateFailed`, `InvalidHandle`, `PoolExhausted`, `InvalidDimension`.
- **Rendering Primitives:**
  - `struct alignas(16) Color`: Premultiplied linear float `{r, g, b, a}` (`sizeof == 16`). Helpers: `Color::Premultiplied()`, `Color::FromRGBA8()`.
  - `struct alignas(16) Vertex2D`: `{x, y, Color, u, v}` (`sizeof == 32`).
  - `struct QuadCmd`: World/screen center `(x, y)`, size `(w, h)`, precomputed `(cosRot, sinRot)`, `uv`, `color`, `texture`, `depth`, `passId`, `layer`, `viewSlot`, `blend`, `flipX`, `flipY`.
  - `struct Camera2D`: `worldCenterX`, `worldCenterY`, `zoom`, `viewport`, `scissor`, `screenSpace`.

## 3. Public Interfaces & Signatures
### `RenderList` (`renderer/RenderList.h` — Pure C++ Command Buffer & Batch Compiler)
- `void Reset(platform::Extent2i size) noexcept`
- `void SetPass(u8 passId, const RenderPassDesc& desc) noexcept`, `void SetView(u8 viewSlot, const Camera2D& cam) noexcept`
- `void PushQuad(const QuadCmd& cmd) noexcept`
- `void PushRect(FRect rect, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept`
- `void PushLine(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color, u8 passId = 0, u8 layer = 0, u16 depth = 32768, u8 viewSlot = 0) noexcept`
- `void Compile() noexcept`
- `void ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept`
- `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `Renderer` (`renderer/Renderer.h` — Move-Only RAII Hardware Adapter & Asset Sink)
- Lifecycle & Viewport:
  - `[[nodiscard]] std::expected<void, RendererErrc> Init(platform::Window& window, const RendererConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`
  - `void ResizeViewport(platform::Extent2i pixelSize) noexcept`
  - `[[nodiscard]] std::expected<void, RendererErrc> SetPresentMode(PresentMode mode) noexcept`
- Asset Sink API (For Asset Manager Integration):
  - `[[nodiscard]] std::expected<TextureHandle, RendererErrc> CreateTexture(const TextureDesc& desc) noexcept`
  - `[[nodiscard]] std::expected<void, RendererErrc> UpdateTexture(TextureHandle handle, FRect region, std::span<const u8> pixels, u32 pitch = 0) noexcept`
  - `void DestroyTexture(TextureHandle handle) noexcept`
  - `[[nodiscard]] constexpr bool IsTextureValid(TextureHandle handle) const noexcept`
- Frame Execution:
  - `[[nodiscard]] RenderList& BeginFrame() noexcept`
  - `void Submit() noexcept`, `void Submit(RenderList& list) noexcept`
# EngineN — Asset Subsystem Context (`assets/`)

## 1. Architecture & Invariants
- **Standard:** C++23, zero heap allocation in hot paths, minimal 5-file footprint (`AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp`), two-layer separation (Pure C++ Core + SDL3 Hardware/IO Adapter).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`AssetTypes.h`, `AssetBank.h/.cpp`):** Pure C++23 data-oriented asset storage, power-of-2 flat Robin Hood hash index, self-contained LZ4 block decompressor, 32-bit bitmask scope manager, and LRU VRAM eviction engine. Zero OS or SDL dependencies; 100% testable in headless CI via `AssetBank::ComputeAuditHash()`.
  - **Layer 2 (`AssetSystem.h/.cpp`):** Thin SDL3 asynchronous I/O and hardware upload coordinator. Owns `SDL_AsyncIOQueue`, virtual `.enpak` mount overlays (memory-mapped or async file handles), a contiguous bipartite staging ring buffer, and bridges completed texture payloads directly to `renderer::Renderer`. `<SDL3/SDL_*.h>` headers are strictly confined to `AssetSystem.cpp`.
- **1-Hop Flattened `SpriteFrame` Table (Zero Pointer Chasing):**
  - All atlas frames are flattened into a single global array (`m_sprites[]`) of 32-byte aligned `SpriteFrame` records (2 sprites per 64B cache line).
  - Each `SpriteFrame` embeds the resolved `renderer::TextureHandle` (`gpuTexture`) and pre-baked `renderer::FRect uv` directly. Calling `GetSprite()` or `GetAtlasFrame()` performs a single branchless array read (`O(1)`, zero indirection through texture tables) ready for immediate `RenderList::PushQuad` submission.
- **Bitmask Scope Pinning & Zero-List Lifetime Management:**
  - Replaces per-handle reference counting and per-scope entry arrays with a 32-bit `scopeMask` embedded inside each 16-byte `SlotMeta` (`kMaxScopes = 32`).
  - `BeginScope()` allocates a scope bit in `O(1)` via `std::countr_one()`. Pinning is 100% idempotent (`scopeMask |= (1u << scope)`). `EndScope()` clears the scope bit across slots and stamps `lastUsedFrame`. Unpinned textures (`scopeMask == 0`) are lazily evicted in LRU order only when `CurrentVramBytes()` exceeds `vramBudgetBytes` past `evictGraceFrames`.
- **O(1) Multiplicative Fibonacci Robin Hood Hash Index:**
  - Both runtime `AssetId -> Slot` lookup in `AssetBank` and `.enpak` TOC lookup in `AssetSystem` use power-of-2 flat Robin Hood hash tables with 64-bit Fibonacci hashing (`11400714819323198485ULL`) and probe sequence length (PSL) early-out.
- **Bipartite Contiguous Staging Ring & Zero-Copy `mmap` Fast Path:**
  - Uncompressed `Blob` and `Audio` payloads inside memory-mapped `.enpak` mounts bypass staging and resolve synchronously via zero-copy pointers into the mapped view.
  - Async file reads and LZ4 decompressions stream through a single pre-allocated bipartite ring buffer (`m_stagingRing`), eliminating fixed-block internal fragmentation.
- **Deterministic Single-Mutation `Pump()` & Permanent Slot 0 Fallbacks:**
  - Slot `0` of every asset table (`Texture`, `Atlas`, `SpriteFrame`, `Blob`, `Audio`) is permanently initialized to a valid fallback (`renderer::kWhiteTexture`, unit UV `0..1`, zeroed bytes, silence PCM). Stale, null, or in-flight handles resolve branchlessly to fallback data without readiness checks.
  - All state transitions, GPU uploads (`Renderer::CreateTexture`), and VRAM evictions (`Renderer::DestroyTexture`) occur exclusively on the main thread inside `AssetSystem::Pump()` sorted strictly by request sequence number (`sequence`), guaranteeing deterministic simulation (`Residency::Sim` gated via `WaitSimReady()`) regardless of OS async I/O completion order.

## 2. Core Types (`assets/AssetTypes.h`)
- **Identifiers & Generational Handles:**
  - `struct AssetId { u64 value{0}; };` — Normalized 64-bit FNV-1a path hash (`\` -> `/`, lowercase ASCII) with `consteval` literal `operator""_aid` and `constexpr AssetId::Hash(std::string_view)`.
  - `template <typename Tag> struct Handle { u16 index{0}; u16 generation{0}; };` — 4-byte value handles: `TextureAssetHandle`, `AtlasHandle`, `BlobHandle`, `AudioHandle`.
  - `struct SpriteHandle { u16 spriteIndex{0}; u16 generation{0}; };` — Direct 1-hop index into `AssetBank`'s flat `SpriteFrame` array.
  - `using ScopeId = u8;` (`kInvalidScope = 0xFF`, `kMaxScopes = 32`).
  - `enum class AssetErrc : u8`: `NotFound`, `PoolExhausted`, `ScopeExhausted`, `CorruptedHeader`, `CorruptedPayload`, `HashMismatch`, `IoFailure`, `StagingOverflow`, `DeviceUploadFailed`, `InvalidMount`.
  - `enum class AssetKind : u8`: `None`, `Texture`, `Atlas`, `Blob`, `Audio`.
  - `enum class Residency : u8`: `Presentation` (async, fallback until ready), `Sim` (blocking gate, deterministic).
- **Runtime Hot Records & Metadata:**
  - `struct alignas(32) SpriteFrame`: `uv` (`renderer::FRect`), `width`, `height`, `pivotX`, `pivotY`, `gpuTexture` (`renderer::TextureHandle`, defaults to `renderer::kWhiteTexture`) (`sizeof == 32`).
  - `struct TextureRecord`: `gpu` (`renderer::TextureHandle`), `width`, `height`.
  - `struct AtlasRecord`: `firstSprite`, `spriteCount`, `textureSlot`, `reserved`.
  - `struct alignas(16) BlobRecord`: `data` (`const u8*`), `size`, `schemaHash`.
  - `struct alignas(16) AudioRecord`: `pcm` (`const i16*`), `frameCount`, `sampleRate`, `channels`.
  - `struct alignas(16) SlotMeta`: `scopeMask` (`u32`), `lastUsedFrame` (`u32`), `generation` (`u16`), `version` (`u16`), `payloadSlot` (`u16`), `kind` (`AssetKind`), `state` (`AssetState`) (`sizeof == 16`).
- **Cooked Binary Formats (`.enpak`):**
  - `struct alignas(32) PakHeader`: `magic` (`'ENPK'`), `version` (`1`), `entryCount`, `flags`, `tocOffset`, `contentHash`.
  - `struct alignas(32) PakEntry`: `assetId`, `offset`, `storedSize`, `rawSize`, `metaA`, `metaB`, `kind`, `codec` (`Raw`, `Lz4`).
  - `struct CookedAtlasHeader` (`16B`) & `struct CookedFrameName` (`8B`).

## 3. Public Interfaces & Signatures
### `AssetBank` (`assets/AssetBank.h` — Pure C++23 Storage, Codec & Scope Engine)
- Lifecycle & Scopes:
  - `[[nodiscard]] std::expected<void, AssetErrc> Init(const AssetBankConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`
  - `[[nodiscard]] std::expected<ScopeId, AssetErrc> BeginScope() noexcept`
  - `void EndScope(ScopeId scope) noexcept`, `void AdvanceFrame(u32 frameIndex) noexcept`
- Branchless Hot-Path Queries (Zero Allocation, O(1)):
  - `[[nodiscard]] constexpr const SpriteFrame& GetSprite(SpriteHandle h) const noexcept`
  - `[[nodiscard]] constexpr const SpriteFrame& GetAtlasFrame(AtlasHandle h, u16 frameOffset) const noexcept`
  - `[[nodiscard]] constexpr SpriteHandle GetSpriteHandle(AtlasHandle h, u16 frameOffset) const noexcept`
  - `[[nodiscard]] constexpr const TextureRecord& GetTexture(TextureAssetHandle h) const noexcept`
  - `[[nodiscard]] constexpr const BlobRecord& GetBlob(BlobHandle h) const noexcept`
  - `[[nodiscard]] constexpr const AudioRecord& GetAudio(AudioHandle h) const noexcept`
  - `[[nodiscard]] u16 FindNamedFrame(AtlasHandle h, u32 nameHash) const noexcept`
- Mutation, Codec & Determinism Audit:
  - `[[nodiscard]] std::expected<u16, AssetErrc> AcquireSlot(AssetId id, AssetKind kind, ScopeId scope, Residency residency, bool& outNeedsLoad) noexcept`
  - `std::expected<void, AssetErrc> CommitTexture(u16 slot, renderer::TextureHandle gpuHandle, u16 width, u16 height, u32 vramBytes) noexcept`
  - `std::expected<void, AssetErrc> CommitAtlas(u16 slot, u16 backingTextureSlot, std::span<const u8> rawPayload) noexcept`
  - `std::expected<void, AssetErrc> CommitBlob(u16 slot, std::span<const u8> rawPayload, u32 schemaHash, bool isMemoryMapped) noexcept`
  - `std::expected<void, AssetErrc> CommitAudio(u16 slot, std::span<const u8> rawPayload, u16 sampleRate, u8 channels, bool isMemoryMapped) noexcept`
  - `[[nodiscard]] u32 EvictUnpinned(std::span<renderer::TextureHandle> outDestroyedTextures) noexcept`
  - `[[nodiscard]] static std::expected<u32, AssetErrc> DecompressLz4(std::span<const u8> src, std::span<u8> dst) noexcept`
  - `[[nodiscard]] u64 ComputeAuditHash() const noexcept`

### `AssetSystem` (`assets/AssetSystem.h` — Move-Only RAII SDL3 Async I/O & Renderer Coordinator)
- Lifecycle & Virtual Mounts:
  - `[[nodiscard]] std::expected<void, AssetErrc> Init(const AssetSystemConfig& cfg = {}) noexcept`
  - `void Shutdown() noexcept`
  - `[[nodiscard]] std::expected<void, AssetErrc> MountPackMemory(std::span<const u8> mappedPak, u8 priority = 0) noexcept`
  - `[[nodiscard]] std::expected<void, AssetErrc> MountPackFile(std::string_view filePath, u8 priority = 0) noexcept`
- Scope Pinning & Streaming Requests:
  - `[[nodiscard]] std::expected<ScopeId, AssetErrc> BeginScope() noexcept`, `void EndScope(ScopeId scope) noexcept`
  - `[[nodiscard]] std::expected<TextureAssetHandle, AssetErrc> PinTexture(ScopeId scope, AssetId id, Residency residency = Residency::Presentation) noexcept`
  - `[[nodiscard]] std::expected<AtlasHandle, AssetErrc> PinAtlas(ScopeId scope, AssetId id, Residency residency = Residency::Presentation) noexcept`
  - `[[nodiscard]] std::expected<BlobHandle, AssetErrc> PinBlob(ScopeId scope, AssetId id, Residency residency = Residency::Sim) noexcept`
  - `[[nodiscard]] std::expected<AudioHandle, AssetErrc> PinAudio(ScopeId scope, AssetId id, Residency residency = Residency::Presentation) noexcept`
- Frame Synchronization & Hardware Upload:
  - `void Pump(renderer::Renderer& renderer, const PumpBudget& budget = {}) noexcept`
  - `void PumpHeadless(const PumpBudget& budget = {}) noexcept`
  - `[[nodiscard]] std::expected<void, AssetErrc> WaitSimReady(ScopeId scope, renderer::Renderer* optionalRenderer = nullptr) noexcept`
  - `[[nodiscard]] constexpr const AssetBank& Bank() const noexcept`
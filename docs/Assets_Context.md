# EngineN — Asset Subsystem Context (`engine/assets/`)

## 1. Architecture & Invariants
- **Core:** C++23, zero heap allocation in hot paths, 5-file footprint (`AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp`), two-layer design (Pure C++ Storage/Codec + SDL3 Hardware/IO Adapter).
- **Two-Layer Zero-Virtual Seam:**
  - **Layer 1 (`AssetBank`):** Data-oriented asset tables, flat Robin Hood hash index, built-in LZ4 block decompressor, 32-bit bitmask scope manager, and LRU VRAM eviction. Zero OS or SDL dependencies; 100% testable headless via `ComputeAuditHash()`.
  - **Layer 2 (`AssetSystem`):** SDL3 asynchronous I/O and GPU upload coordinator. Owns `SDL_AsyncIOQueue`, `.enpak` archive overlays (mmap or async file handles), bipartite staging ring buffer, and bridges completed textures into `renderer::Renderer`. `<SDL3/...>` headers are strictly confined to `AssetSystem.cpp`.
- **1-Hop Flattened `SpriteFrame` Array:** All atlas frames are flattened into a contiguous array of 32-byte records (`SpriteFrame`). Each frame embeds the resolved `renderer::TextureHandle` and `FRect uv`. Calling `GetSprite()` or `GetAtlasFrame()` performs a branchless $O(1)$ read with zero pointer chasing.
- **Bitmask Scope Pinning (Zero-List Lifetimes):** Replaces refcounting with a 32-bit `scopeMask` in each 16-byte `SlotMeta` (`kMaxScopes = 32`). `BeginScope()` allocates via `std::countr_one()`. Unpinned assets (`scopeMask == 0`) are lazily evicted in LRU order when exceeding `vramBudgetBytes` past `evictGraceFrames`.
- **$O(1)$ Robin Hood Hash Index:** 64-bit Fibonacci hashing (`11400714819323198485ULL`) with PSL early-out for both runtime `AssetId -> Slot` lookup and `.enpak` TOC indexing.
- **Permanent Slot 0 Fallback:** Slot `0` of every table (`Texture`, `Atlas`, `SpriteFrame`, `Blob`, `Audio`) is permanently bound to valid fallback data (`kWhiteTexture`, unit UV, zeroed bytes, silence PCM). Unloaded or stale handles resolve branchlessly without crashing.
- **Deterministic Main-Thread Mutation:** Async I/O completions, LZ4 decompressions, GPU uploads, and evictions are executed on the main thread in `AssetSystem::Pump()` sorted by sequence number.

## 2. Core Layouts & Types (`AssetTypes.h`)
- `AssetId`: 64-bit normalized FNV-1a path hash (case-folded, `/` normalized). Literal: `""_aid`, `AssetId::Hash()`.
- Generational Handles: `TextureAssetHandle`, `AtlasHandle`, `BlobHandle`, `AudioHandle` (all `Handle<Tag>`: `{u16 index, u16 generation}`).
- `SpriteHandle`: `{u16 spriteIndex, u16 generation}` (1-hop index into `SpriteFrame` table).
- `AssetErrc`: `NotFound`, `PoolExhausted`, `ScopeExhausted`, `CorruptedHeader`, `CorruptedPayload`, `HashMismatch`, `IoFailure`, `StagingOverflow`, `DeviceUploadFailed`, `InvalidMount`.
- `Residency`: `Presentation` (async streaming, fallback while loading), `Sim` (blocking gate via `WaitSimReady()`).
- `SpriteFrame` (`alignas(32)`, 32B): `{uv, width, height, pivotX, pivotY, gpuTexture}`.
- `TextureRecord`: `{gpuTexture, width, height}`.
- `AtlasRecord`: `{firstSprite, spriteCount, textureSlot, reserved}`.
- `BlobRecord` (`16B`): `{const u8* data, u32 size, u32 schemaHash}`.
- `AudioRecord` (`16B`): `{const i16* pcm, u32 frameCount, u16 sampleRate, u8 channels}`.
- `SlotMeta` (`alignas(16)`, 16B): `{scopeMask, lastUsedFrame, generation, version, payloadSlot, kind, state}`.

## 3. Subsystem APIs
### `AssetBank` (`engine/assets/AssetBank.h` — Storage, Codec & Scope Engine)
- **Lifecycle & Scopes:**
  - `Init(AssetBankConfig)` $\to$ `Result<void, AssetErrc>`, `Shutdown()`
  - `BeginScope()` $\to$ `Result<ScopeId, AssetErrc>`, `EndScope(ScopeId)`
  - `AdvanceFrame(frameIndex)`, `EvictUnpinned(span<TextureHandle>)` $\to$ `u32`
- **$O(1)$ Hot-Path Accessors:**
  - `GetSprite(SpriteHandle)` $\to$ `const SpriteFrame&`
  - `GetAtlasFrame(AtlasHandle, frameOffset)` $\to$ `const SpriteFrame&`
  - `GetSpriteHandle(AtlasHandle, frameOffset)` $\to$ `SpriteHandle`
  - `GetTexture(TextureAssetHandle)` $\to$ `const TextureRecord&`
  - `GetBlob(BlobHandle)` $\to$ `const BlobRecord&`
  - `GetAudio(AudioHandle)` $\to$ `const AudioRecord&`
  - `FindNamedFrame(AtlasHandle, nameHash)` $\to$ `u16`
  - `FindSlot(AssetId)` $\to$ `u16`, `ToTextureHandle(slot)`, `ToAtlasHandle(slot)`, etc.
- **Commit & Utilities:**
  - `CommitTexture(slot, gpuHandle, w, h, vramBytes)`, `CommitAtlas(...)`, `CommitBlob(...)`, `CommitAudio(...)`
  - `DecompressLz4(src, dst)` $\to$ `Result<u32, AssetErrc>`, `ComputeAuditHash()` $\to$ `u64`.

### `AssetSystem` (`engine/assets/AssetSystem.h` — Async I/O & GPU Coordinator)
- **Mounts & Scopes:**
  - `Init(AssetSystemConfig)` $\to$ `Result<void, AssetErrc>`, `Shutdown()`
  - `MountPackMemory(span<const u8>, priority)`, `MountPackFile(filePath, priority)` $\to$ `Result<void, AssetErrc>`
  - `BeginScope()`, `EndScope(scope)`
- **Pinning (Streaming Requests):**
  - `PinTexture(scope, id, residency)` $\to$ `Result<TextureAssetHandle, AssetErrc>`
  - `PinAtlas(scope, id, residency)` $\to$ `Result<AtlasHandle, AssetErrc>`
  - `PinBlob(scope, id, residency)` $\to$ `Result<BlobHandle, AssetErrc>`
  - `PinAudio(scope, id, residency)` $\to$ `Result<AudioHandle, AssetErrc>`
- **Frame Pump & Synchronization:**
  - `Pump(Renderer&, budget)`, `PumpHeadless(budget)`
  - `WaitSimReady(scope, optionalRenderer)` $\to$ `Result<void, AssetErrc>`
  - `Bank()`: Accesses internal `AssetBank`.

## 4. Main Loop Integration
```cpp
// 1. Frame pumping: process async I/O completions & upload textures to GPU
m_assetSystem.Pump(m_renderer);

// 2. Gameplay/Rendering access via 1-hop branchless SpriteFrame
const auto& sprite = m_assetSystem.Bank().GetSprite(m_playerSprite);
renderList.PushQuad({.x = pos.x, .y = pos.y, .w = sprite.width, .h = sprite.height,
                     .uv = sprite.uv, .texture = sprite.gpuTexture});
```
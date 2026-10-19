# EngineN — Asset Subsystem Context (`assets/`)

## 1. Architecture & Invariants
- **Standard:** C++23 (`-fno-exceptions`, `-fno-rtti`), strictly zero heap allocation after `Init()`, minimal 5-file footprint (`AssetTypes.h`, `AssetBank.h/.cpp`, `AssetSystem.h/.cpp`) + `tests/AssetsTests.cpp`, two-layer pure C++23 separation (`namespace en::assets`).
- **Two-Layer Zero-Virtual Seam & Zero Cross-Subsystem Header Pollution:**
  - **Layer 1 (`AssetTypes.h`, `AssetBank.h/.cpp`):** Pure C++23 data-oriented asset storage, power-of-2 flat Robin Hood hash index, self-contained bounds-safe LZ4 block decompressor, 32-bit bitmask scope manager, `constexpr` $O(1)$ per-scope `Residency::Sim` readiness tracker (`m_scopeSimPending[32]`), and LRU VRAM eviction engine. Depends solely on `"core/Result.h"`, `"core/Types.h"`, and Layer 1 `"renderer/RendererTypes.h"` — zero OS, `<SDL3/SDL_*.h>`, or `platform/` dependencies; 100% testable in headless CI via `AssetBank::ComputeAuditHash()`.
  - **Layer 2 (`AssetSystem.h/.cpp`):** Move-only RAII SDL3 asynchronous I/O and hardware upload coordinator. Owns `SDL_AsyncIOQueue`, virtual `.enpak` mount overlays (`PakMount`, `32B`), a single unified VFS Robin Hood hash index (`VfsTocSlot`, `16B`), a 64B-aligned intrusive FIFO `PendingJob` queue, and a bipartite staging ring buffer. `<SDL3/SDL_*.h>` and `"renderer/Renderer.h"` are strictly confined to `AssetSystem.cpp` (zero `platform/` includes anywhere in `assets/`).
- **Single Contiguous 64B-Aligned Arena Slabs & 10 Fewer Arrays in `AssetBank`:**
  - `AssetBank::Init()` allocates **1 single 64-byte aligned arena slab (`m_arena`)** and carves only 14 cache-line-aligned tables.
  - `AssetSystem::Init()` allocates **1 single 64-byte aligned arena slab (`m_arena`)** containing `m_mounts`, `m_jobs`, `m_tocEntriesPool`, `m_vfsHash`, and `m_stagingRing`. `MountPackMemory()` and `MountPackFile()` register TOC entries in $O(1)$ with **zero heap allocations after `Init()`**.
- **L1 Cache Line 0 Partitioning, Embedded Generation & Intrusive Freelists:**
  - **`AssetBank` Cache Line 0 (`offset 0..63`, `alignas(64)`):** Packs all hot 120 Hz / 240 Hz read-only table pointers and active high-water bounds (`m_sprites`, `m_spriteGens`, `m_atlases`, `m_textures`, `m_blobs`, `m_audio`, `m_spriteCount`, `m_activeScopesMask`, `m_highWaterAtlas`, `m_highWaterTex`, `m_highWaterBlob`, `m_highWaterAudio`).
  - **Single-Array 1-Hop Validation:** `AtlasRecord` (`8B`) embeds its `u16 generation` directly; `GetBlob()` and `GetAudio()` validate `h.index < m_highWater* && h.generation == 1u` in a single array read; `GetTexture()` validates against `m_slots[m_texMasterSlot[h.index]].generation`.
  - **Intrusive Freelists (`m_freeSlotHead`, `m_freeTexHead`, `m_freeAtlasHead`):** Evicted `SlotMeta`, `TextureRecord`, and `AtlasRecord` slots thread their `nextFree` index intrusively inside unused 16-bit fields (`payloadSlot`, `width`, `textureSlot`), eliminating separate freelist arrays and startup initialization loops.
- **`constexpr` O(1) `IsScopeSimReady(scope)` via Dual-Use `SlotMeta::lastUsedFrame`:**
  - While `SlotMeta::state == AssetState::Loading`, `SlotMeta::lastUsedFrame` stores the 32-bit `simScopeMask` of scopes waiting on that slot as `Residency::Sim`, maintaining `u16 m_scopeSimPending[32]` (Cache Line 2).
  - `CompleteSlot(meta, finalState)` decrements `m_scopeSimPending` for every bit in `meta.lastUsedFrame` and stamps `meta.lastUsedFrame = m_currentFrame`. `IsScopeSimReady(scope)` is a `constexpr` 1-instruction check (`m_scopeSimPending[scope] == 0`) with zero loops.
- **Unified Single-Probe VFS Table (`VfsTocSlot`) & Intrusive 64B FIFO `PendingJob` Queue:**
  - Replaces per-mount hash tables and multi-mount probe loops with a single unified `VfsTocSlot` (`16B`) Robin Hood table that resolves `(id) -> (mountIndex, entryIndex)` in **1 probe** while enforcing mount `priority` on insertion (`RegisterVfsEntries`).
  - `PendingJob` (`64B`, `alignas(64)`) forms an intrusive doubly-linked FIFO queue (`m_activeJobHead .. m_activeJobTail`) in arrival order, eliminating `u64 sequence` and $O(K \times N)$ selection loops. LZ4 `Blob` and `Audio` payloads decode directly into `AssetBank`'s `m_blobArena` (`AllocateArenaSlice`).

## 2. Core Types (`assets/AssetTypes.h`)
- **Shared `constexpr` Utilities:** `NextPow2(u32)`, `FibonacciHash(u64, u32)`, `Align64(usize)`, `CarveAligned64<T>(u8*&, usize)`.
- **Identifiers & Generational Handles (`Zero Padding`):**
  - `struct alignas(8) AssetId { u64 value{0}; };` (`sizeof == 8`) — Normalized 64-bit FNV-1a path hash (`\` -> `/`, lowercase ASCII) with `consteval operator""_aid` and `constexpr AssetId::Hash(std::string_view)`.
  - `template <typename Tag> struct alignas(4) Handle { u16 index{0}; u16 generation{0}; };` (`sizeof == 4`): `TextureAssetHandle`, `AtlasHandle`, `BlobHandle`, `AudioHandle`.
  - `struct alignas(4) SpriteHandle { u16 spriteIndex{0}; u16 generation{0}; };` (`sizeof == 4`).
  - `using ScopeId = u8;` (`kInvalidScope = 0xFF`, `kMaxScopes = 32`).
- **Runtime Hot Records & Metadata (`Zero Padding`):**
  - `struct alignas(32) SpriteFrame` (`sizeof == 32`): `uv`, `width`, `height`, `pivotX`, `pivotY`, `gpuTexture`.
  - `struct alignas(8) TextureRecord` (`sizeof == 8`): `gpu`, `width`, `height`.
  - `struct alignas(8) AtlasRecord` (`sizeof == 8`): `firstSprite`, `spriteCount`, `textureSlot`, `generation`.
  - `struct alignas(16) BlobRecord` (`sizeof == 16`): `data`, `size`, `schemaHash`.
  - `struct alignas(16) AudioRecord` (`sizeof == 16`): `pcm`, `frameCount`, `sampleRate`, `channels`, `reserved`.
  - `struct alignas(16) SlotMeta` (`sizeof == 16`): `scopeMask`, `lastUsedFrame` (stores `simScopeMask` while `Loading`), `generation`, `version`, `payloadSlot`, `kind`, `state`.

## 3. Public Interfaces & Signatures
### `AssetBank` (`assets/AssetBank.h` — 64B-Aligned Pure C++23 Storage, Codec & Scope Engine)
- Lifecycle & Scopes: `Init(cfg)`, `Shutdown()`, `BeginScope()`, `EndScope(scope)`, `AdvanceFrame(frameIndex)`.
- Branchless Hot-Path Queries (`constexpr`, O(1)): `GetSprite(h)`, `GetAtlasFrame(h, frameOffset)`, `GetSpriteHandle(h, frameOffset)`, `GetTexture(h)`, `GetBlob(h)`, `GetAudio(h)`, `FindNamedFrame(h, nameHash)`.
- Mutation, Arena Slice, Codec & Audit: `AcquireSlot(...)`, `AllocateArenaSlice(sizeBytes)`, `RollbackArenaSlice(prevHead)`, `CommitTexture(...)`, `CommitAtlas(...)`, `CommitBlob(...)`, `CommitAudio(...)`, `MarkFailed(slot)`, `EvictUnpinned(outDestroyedTextures)`, `IsScopeSimReady(scope)`, `IsOverVramBudget()`, `CurrentVramBytes()`, `DecompressLz4(src, dst)`, `ComputeAuditHash()`.

### `AssetSystem` (`assets/AssetSystem.h` — 64B-Aligned Move-Only RAII SDL3 Async I/O & Renderer Coordinator)
- Lifecycle & Virtual Mounts: `Init(cfg)`, `Shutdown()`, `MountPackMemory(mappedPak, priority)`, `MountPackFile(filePath, priority)`.
- Scope Pinning & Streaming: `BeginScope()`, `EndScope(scope)`, `Pin(scope, id, kind, residency)`, `PinTexture(...)`, `PinAtlas(...)`, `PinBlob(...)`, `PinAudio(...)`.
- Frame Sync & Upload: `Pump(renderer, budget)`, `PumpHeadless(budget)`, `WaitSimReady(scope, optionalRenderer)`, `Bank()`.
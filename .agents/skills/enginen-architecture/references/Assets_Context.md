# EngineN Phase 6 — `assets/` Subsystem Context (LOCKED — VERIFIED v2)

## 1. Architectural Overview & Strict 2-Layer Separation
- **Layer 1 (`assets/AssetTypes.h`, `assets/AssetBank.h`, `assets/AssetBank.cpp`):**
  - 100% platform-agnostic C++23 (`-std=c++23 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror -Wstringop-overflow=2`).
  - Zero `platform/` header coupling (depends only on `"renderer/RendererTypes.h"` and `"core/Containers.h"`).
  - Zero compiler padding holes (`0B` padding across every struct) and **zero `using` type aliases** (`AssetId`, `SpriteHandle`, `TextureAssetHandle`, `AtlasHandle`, `BlobHandle`, `AudioHandle` are all concrete structs; scopes use `u8` directly).
  - Headless-testable deterministic audit hashing via `AssetBank::ComputeAuditHash()`.
- **Layer 2 (`assets/AssetSystem.h`, `assets/AssetSystem.cpp`):**
  - Thin VFS `.enpak` archive mount coordinator, on-demand `SDL_IOStream` range-read streamer (`m_packStreams[16]`), time-sliced background streaming queue (`RingBuffer<PendingJob, 128>`), automatic Atlas-to-Texture dependency resolver, and GPU texture uploader (`Renderer* optionalRenderer`).
  - `<SDL3/SDL.h>` (`SDL_IOStream`) is strictly confined to `AssetSystem.cpp` (`struct SDL_IOStream;` forward-declared in `AssetSystem.h`).

## 2. Exact Struct Byte Layouts (`0B` Padding, `0` Type Aliases)
- **`AssetId` (`8B`, `alignas(8)`):** `u64 value{0}` with implicit `StringId` interoperability, `AssetId::Hash(std::string_view)`, `IsValid()`, and `operator""_aid`.
- **`SpriteHandle`, `TextureAssetHandle`, `AtlasHandle`, `BlobHandle`, `AudioHandle` (`4B` each, `alignas(4)`):** Concrete generational handle structs with `u16 index{0}` and `u16 generation{0}`.
- **`UnitPivotU16` (`2B`, `alignas(2)`):** Normalized `[0.0f .. 1.0f]` UNORM16 pivot coordinate allowing both `SpriteFrame` and `CookedAtlasSpriteEntry` to fit in `32B` exact.
- **`SpriteFrame` (`32B`, `alignas(32)`, 2 per 64B cache line, `0B` padding):** `uv` (`Rect`, `16B`), `width` (`4B`), `height` (`4B`), `pivotX` (`UnitPivotU16`, `2B`), `pivotY` (`UnitPivotU16`, `2B`), `gpuTexture` (`TextureHandle`, `4B`), `Pivot()`.
- **`TextureRecord` (`16B`, `alignas(16)`, 4 per 64B cache line, `0B` padding):** `pixels` (`const u8*`, `8B`), `gpu` (`TextureHandle`, `4B`), `width` (`u16`, `2B` — stores `nextFree` when on intrusive freelist), `height` (`u16`, `2B` — stores retired `generation` when on intrusive freelist).
- **`AtlasRecord` (`8B`, `alignas(8)`, 8 per 64B cache line, `0B` padding):** `firstSprite` (`u16`, `2B` — stores retired `generation` when on intrusive freelist), `spriteCount` (`u16`, `2B`), `textureSlot` (`u16`, `2B` — stores `nextFree` when on intrusive freelist), `generation` (`u16`, `2B`).
- **`BlobRecord` (`16B`, `alignas(16)`, 4 per 64B cache line, `0B` padding):** `data` (`const u8*`, `8B`), `size` (`u32`, `4B` — stores `nextFree` when on intrusive freelist), `generation` (`u16`, `2B` — embedded ABA generation eliminating `m_blobGens`), `version` (`u16`, `2B` — stores live hot-reload version when active and retired `generation` when on intrusive freelist).
- **`AudioRecord` (`16B`, `alignas(16)`, 4 per 64B cache line, `0B` padding):** `pcm` (`const f32*`, `8B`), `frameCount` (`u32`, `4B` — stores retired `generation` when on intrusive freelist), `sampleRate` (`u16`, `2B` — stores `nextFree` when on intrusive freelist), `channels` (`u8`, `1B`), `looping` (`bool`, `1B`).
- **`SlotMeta` (`16B`, `alignas(16)`, 4 per 64B cache line, `0B` padding):** `scopeMask` (`u32`, `4B`), dual-use `lastUsedFrame` (`u32`, `4B` — stores 32-bit `simScopeMask` while `state == AssetState::Loading` and frame timestamp once `Ready`), `generation` (`u16`, `2B`), `version` (`u16`, `2B`), `payloadSlot` (`u16`, `2B` — stores `nextFree` when `Unloaded`), `kind` (`AssetKind`, `1B`), `state` (`AssetState`, `1B`).
- **`EnPakHeader` (`16B`, `alignas(16)`), `EnPakEntry` (`32B`, `alignas(8)` with `SetBackingTexture(AssetId)` / `BackingTexture()`), `CookedAtlasSpriteEntry` (`32B`, `alignas(8)`).**
- **`AssetBank::FreePayloadBlock` (`16B`, `alignas(16)`), `AssetSystem::VfsTocSlot` (`48B`, `alignas(8)`), `AssetSystem::PendingJob` (`16B`, `alignas(16)`).**

## 3. Core Mechanisms & Performance Contracts
- **4-Cache-Line `AssetBank` (`256B`, `alignas(64)`):**
  - **Cache Line 0 (`0..63`):** Hot 120 Hz / 240 Hz read-only table pointers (`m_sprites`, `m_spriteGens`, `m_atlases`, `m_textures`, `m_blobs`, `m_audio`), `m_activeScopesMask`, and 6 `u16` high-water bounds. `GetBlob()` validates `m_blobs[h.index].generation == h.generation` directly in Cache Line 0 (`m_blobGens` eliminated).
  - **Cache Line 1 (`64..127`):** `m_scopeSimPending[32]` (`64B`) for `constexpr` $O(1)$ `IsScopeSimReady(scope)` without scanning slots.
  - **Cache Line 2 (`128..191`):** `m_slots` (carved first at offset `0` of `m_arena`), `m_spriteNameHashes` (cold array for `FindNamedFrame`), `m_idToSlot` (`RobinHoodMap<u16>`), `m_arena` (`AlignedArena`), `m_currentFrame`, `m_currentVramBytes`, `m_vramBudgetBytes`, `m_maxSlots`, `m_freeSlotHead`.
  - **Cache Line 3 (`192..255`):** `m_texGens`, `m_audioGens`, `m_slotAssetIds`, `m_freePayloadHead` (`FreePayloadBlock*`), capacities, intrusive freelist heads (`m_freeTexHead`, `m_freeAtlasHead`, `m_freeBlobHead`, `m_freeAudioHead`), `m_pendingGpuTexCount`, `m_payloadBaseMarker`, `m_evictedCount`, `m_hotReloadCount`.
- **Coalescing Intrusive Payload Sub-Allocator & Top-of-Arena Tail Rewind (`AllocatePayloadSlice` / `FreePayloadSlice`):**
  - Evicting textures, blobs, audio clips, or hot-reloading blobs via `HotReloadBlob()` reclaims their 64B-aligned payload slices inside `m_arena`.
  - If a freed slice touches `m_arena.UsedBytes()`, `FreePayloadSlice` rewinds `m_arena` immediately (`RollbackToMarker`) and peels off any adjacent tail free block; otherwise it inserts the slice into the address-sorted intrusive `m_freePayloadHead` list and coalesces contiguous blocks.
  - Trailing evicted sprite ranges in `m_sprites` automatically rewind `m_highWaterSprite`.
- **Deduplicated `EnsurePayloadSlot`, Upfront Streaming Handle Reservation & Branchless Slot 0 Fallbacks:**
  - `AcquireSlot()` verifies master slot availability before calling the unified `EnsurePayloadSlot()` helper, reserving `payloadSlot` and per-pool ABA `generation` upfront while keeping the typed pool's active generation at `0U` during `AssetState::Loading`.
  - `PinTexture`, `PinAtlas`, `PinBlob`, and `PinAudio` immediately return the final generational handle even for `AssetResidency::Stream`, resolving to Slot 0 fallbacks while `Loading` and switching to the live asset as soon as `Pump()` commits.
- **On-Demand `SDL_IOStream` Range Streaming, Automatic Atlas Dependency Resolution & Procedural Grid Slicer:**
  - `MountPackFile()` validates only the `16B` `EnPakHeader` and TOC using temporary `ArenaTempScope` scratch (`0` persistent bytes in `m_vfsArena`), keeping `SDL_IOStream*` open in `m_packStreams` and streaming individual asset payloads on demand via `SDL_SeekIO` + `SDL_ReadIO` (reading uncompressed textures/blobs directly into `m_bank`'s arena with zero intermediate copy).
  - `EnPakEntry::SetBackingTexture` / `BackingTexture` encodes a 64-bit texture dependency on `AssetKind::Atlas` entries so `SceneDirector::Pin(scope, id, AssetKind::Atlas, residency)` and `PinAtlas(scope, id, {}, residency)` automatically resolve and pin the backing texture.
  - `AssetBank::CommitGridAtlas` (and `.enpak` grid-atlas entries with `uncompressedSize == 0`) slices uniform `cols x rows` sprite sheets into `SpriteFrame`s with zero external frame table overhead.
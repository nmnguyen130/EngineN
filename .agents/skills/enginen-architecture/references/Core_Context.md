# `engine/core/` — Contracts & Invariants

## Files
- `engine/core/Types.h`
- `engine/core/Math.h`
- `engine/core/Memory.h`
- `engine/core/Containers.h`
- `engine/core/Diagnostics.h`, `engine/core/Diagnostics.cpp`
- `engine/core/Jobs.h`, `engine/core/Jobs.cpp`

## Contracts & Invariants

### 1. Primitives, Strings, and Checksums (`Types.h`)
- **Fixed-Width Types:** `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `usize`, `f32`, `f64`, `Result<T, E> = std::expected<T, E>`.
- **`StringId` (`8B`, `alignas(8)`):** Case-insensitive, slash-normalized (`'\\'` -> `'/'`) 64-bit FNV-1a hash. Empty string hashes to `0`.
- **`Handle<Tag>` (`4B`, `alignas(4)`):** Generational index (`u16 index`, `u16 generation`). `IsValid()` holds iff `generation != 0`.
- **`FixedString<N>` (`NB`, `alignas(2)`):** Fixed-capacity stack string (`kMaxLength = N - 3`). Null-terminated at `m_buf[m_len]`. `Resolve(rel, out)` rejects empty segments, `.` or `..` traversal, control characters, and drive colons (`:`), leaving `out` unchanged on failure.
- **`ComputeCrc32`:** IEEE 802.3 CRC32 checksum supporting incremental chaining.
- **`Rng64` (`8B`, `alignas(8)`):** SplitMix64 generator with Lemire unbiased bounded sampling (`m_state != 0`).

### 2. Arena & Little-Endian Serialization (`Memory.h`)
- **`AlignedArena` (`16B`, `alignas(16)`):** Move-only linear slab allocator with 64-byte cache-line alignment.
  - **Invariant:** `m_offset <= m_capacity <= kMaxArenaBytes` (512 MB), both are multiples of 64, and unallocated/rewound regions `[m_offset, m_capacity)` are zeroed.
- **`NormalizeLittleEndian(T)`:** Byte-swaps arithmetic types (`std::is_arithmetic_v<T>`, including `f32`/`f64` via `std::bit_cast` and integrals via `std::byteswap`) when `std::endian::native == std::endian::big`.
- **`BinaryWriter` (`24B`, `alignas(8)`) & `BinaryReader` (`24B`, `alignas(8)`):**
  - **Contract:** Bounds-checked little-endian stream views over caller-owned spans.
  - **Invariant:** Once `m_ok` becomes `false`, subsequent operations fail without modifying the buffer or output value.

### 3. Hash Map & Ring Buffer (`Containers.h`)
- **`RobinHoodMap<ValueT>` (`16B`, `alignas(8)`):** Open-addressed power-of-two table backed by an `AlignedArena` slice with backward-shift deletion.
  - **Invariant:** `key == 0` is reserved as empty (`dist == 0`); max load factor is 87.5% (`m_capacity - (m_capacity >> 3)`).
- **`RingBuffer<T, CapacityPow2>`:** Fixed-capacity power-of-two FIFO queue with wrapping `u32` head/tail indices.
  - **Invariant:** `(m_tail - m_head) <= CapacityPow2`.

### 4. Logging, Assertions, and Crash Diagnostics (`Diagnostics.h/.cpp`)
- **`LogRecord` (`128B`, `alignas(64)`):** Sequence number, `StringId` category, source location, `LogLevel`, and `FixedString<90>` message.
- **`Logger`:** Thread-safe 64-entry circular log history protected by `g_logMutex`. Messages below `MinLevel()` are filtered before locking.
- **`CrashHandler`:** Thread-local call-stack breadcrumb tracker (`kMaxTraceDepth = 32`) and fatal signal handler (`SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`).
  - **Signal-Safety & Synchronization Invariant:** `SignalHandlerCallback` invokes only async-signal-safe `std::_Exit(128 + sig)` without touching `FILE*` streams or locks. `FormatCrashReport` reads recent log entries through `Logger::RecentRecords` (synchronized via `g_logMutex`), and `WriteCrashDump` writes crash dumps during assertion failures before aborting.

### 5. Work-Stealing Job System (`Jobs.h/.cpp`)
- **`WorkStealingDeque<T, CapacityPow2>`:** Chase-Lev single-producer multi-consumer circular deque with `m_top` and `m_bottom` isolated on separate 64-byte cache lines.
- **`JobSystem` (`alignas(64)`):**
  - **Contract:** `Init(workerThreads)` spawns up to `kMaxWorkers` (16) worker threads and catches any thread creation failure when exceptions are enabled so `noexcept` is never violated.
  - **Batch Math Invariant:** `ParallelFor` computes `totalBatches = ((itemCount - 1U) / batchSize) + 1U` and `end = (itemCount - start > batchSize) ? (start + batchSize) : itemCount`, preventing `u32` addition overflow.
  - **Callable Forwarding Invariant:** `ParallelForRange` decays `Callable&&` via `std::remove_reference_t<Callable>` and `std::addressof(callable)` so both lvalue and rvalue callables compile cleanly.

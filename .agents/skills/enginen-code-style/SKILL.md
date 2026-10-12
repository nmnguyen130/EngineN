---
name: enginen-code-style
description: Standards for writing clean, human-like C++20 code in EngineN, featuring concise inline comments, class docstrings, and zero AI banner bloat.
---

# EngineN — C++20 Clean Code & Architecture Guide

Standards for writing clean, human-like, modern C++20 code in EngineN with zero boilerplate and zero AI markers.

---

## 1. Comment Philosophy ("Human Code")

### Zero AI Banners & Decorative Bloat
- **Forbidden:** Never use horizontal divider bars: `// ====`, `// ----`, `// ****`, `// ────`.
- Use natural whitespace: 1 blank line between functions, 2 between class definitions.
- Avoid redundant section comments (`// ClassName`, `// Methods`, `// Private`).

### Concise Docstrings for Classes & Major Systems
- Write 2–4 lines before significant classes/subsystems, focusing on **responsibility, ownership, and invariants**:
```cpp
/**
 * TimeManager: Central deterministic 120Hz fixed-timestep coordinator.
 * Integer rational accumulator (ns * kSimHz) ensures bitwise zero-drift.
 * Channel-agnostic, supporting generic indexing and user-defined enums.
 */
class TimeManager { ... };
```

### Inline Comments: "The Why, Not The What"
- Explain **technical rationale, edge cases, math constraints, or physics workarounds**. Never repeat syntax.
```cpp
// Merge-by-max: retain longest freeze to prevent cancellation
m_ticks[ch] = std::max(m_ticks[ch], ticks);

// Discard whole backlog, preserve sub-tick remainder phase to prevent judder
m_droppedTicks += m_accUnits / kNsPerSecond;
m_accUnits %= kNsPerSecond;
```

---

## 2. Naming & Type Standards

- **Core Type Aliases (`core/Types.h`)**:
  - Always use: `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`.
  - For containers, sizes, and pointer offsets: use **`usize`** (`std::size_t`) and **`isize`** (`std::ptrdiff_t`).
  - Avoid verbose `std::uint32_t`, `std::size_t`.
- **Naming Conventions**:
  - Types / Classes / Structs / Enums: `PascalCase` (`TimeManager`, `HitstopEngine`).
  - Functions / Methods: `PascalCase` (`BeginFrame()`, `ConsumeTick()`, `GetScaledDt()`).
  - Private member variables: `m_camelCase` (`m_accUnits`, `m_simTickCount`, `m_tickOpen`).
  - Constants: `kPascalCase` (`kSimHz`, `kNsPerSecond`, `kFixedDtSec`).

---

## 3. C++20 Modern Idioms & Anti-Bloat Rules

### Proper Scope for `constexpr` vs `noexcept`
- **Use `constexpr`**: Compile-time constants (`kFixedDtSec`, `kCapacity`), user-defined literals (`operator""_f60`).
- **Avoid `constexpr` on Runtime Containers & Mutators**: State machines and frame buffers (`Push()`, `Clear()`, `AdvanceTick()`, `Request()`) must use plain **`noexcept`** (compiler automatically inlines from headers).

### Clean Generic Channel Overloads
- Use default template parameters `template <typename C = u8>` so a single function cleanly handles both integer `u8` and strongly typed `enum class` without boilerplate overloads:
```cpp
template <typename C = u8>
[[nodiscard]] f32 GetScaledDt(C channel = kDefaultTimeChannel) const noexcept {
    const u8 ch = static_cast<u8>(channel);
    if (ch >= kMaxTimeChannels) return 0.0f;
    return kFixedDtSec * m_channelScales[ch] * m_hitstop.GetModifier(ch);
}
```

### Isolate Heavy Dependencies from Headers
- Keep `.h` header files lean. Only include types needed for declarations.
- Push heavy standard headers (`<cmath>`, `<algorithm>`, `<SDL3/SDL.h>`) into implementation files (`.cpp`).

### Ergonomic `[[nodiscard]]` Rules
- **Mark with `[[nodiscard]]`**: Pure queries, state getters, and tokens/locks (`GetAlpha()`, `ConsumeTick()`, `View()`).
- **Do NOT mark `[[nodiscard]]`**: Fire-and-forget event emission or action queues (`Emit()`, `Push()`, `RequestHitstop()`).

### Cache-Friendly Structs & Zero Redundant Arrays
- Calculate values directly in CPU registers (1–2 operations, like `GetScaledDt`) rather than caching redundant arrays and looping every tick.
- Keep critical single-instance coordinators (like `TimeManager`) compact within 1–2 cache lines (~64–128 bytes).

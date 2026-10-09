---
name: enginen-code-style
description: Standards for writing clean, human-like C++20 code in EngineN, featuring concise inline comments, class docstrings, and zero AI banner bloat.
---

# EngineN — Human Code & Style Guide

This skill defines the coding style, comment philosophy, and architectural aesthetics for the **EngineN** codebase. Follow these rules to keep all code natural, clean, professional, and free from AI boilerplate.

---

## 1. Comment Philosophy ("Human Code")

### Rule 1: Zero AI Banners & Decorative Bloat
- **FORBIDDEN:** Never write horizontal divider bars or decorative banners:
  ```cpp
  // ───────────────────────────────────────────────────────── // BAD
  // ========================================================= // BAD
  // ********************************************************* // BAD
  ```
- Use clean, empty lines (1 blank line between functions, 2 between class definitions) for visual rhythm.

### Rule 2: Multi-line Docstrings for Major Classes & Complex Systems
- Write compact, informative docstrings (2–4 lines) before significant classes, interfaces, or major algorithms.
- Focus on **responsibility, ownership, and invariants**, not describing what each line does.
- Example:
  ```cpp
  /**
   * TimeManager: Central timeloop coordinator for deterministic 120Hz fixed-timestep.
   * Accumulator is tracked in integer ns x Hz to ensure bitwise zero-drift over long runs.
   */
  class TimeManager { ... };
  ```

### Rule 3: Concise 1-Line Inline Comments ("The Why, Not The What")
- Explain **rationale, edge cases, math constraints, or physics workarounds**.
- Never repeat syntax or state the obvious.
- **Good:**
  ```cpp
  // Non-stacking merge-by-max: retains longest freeze to prevent infinite stun locks
  m_remainingSec[idx] = std::max(m_remainingSec[idx], req.durationSec);

  // Discard whole backlog, preserve sub-tick phase to avoid visual judder
  m_accNsHz %= kNsPerSec;

  // 1e-9 threshold prevents IEEE 754 precision residual from holding freeze open
  return (idx < m_remainingSec.size()) && (m_remainingSec[idx] > 1e-9);
  ```
- **Bad:**
  ```cpp
  // Set the max remaining sec
  m_remainingSec[idx] = std::max(...);

  // Return true if frozen
  return isFrozen;
  ```

---

## 2. C++20 Naming & Types Standards

- **Types / Classes / Structs / Enums:** `PascalCase`
  - `TimeManager`, `HitstopEngine`, `TimeChannel`
  - `enum class ChannelId : std::uint8_t { Real, World, Player, Enemy, Count };`
- **Functions / Methods:** `PascalCase`
  - `BeginFrame()`, `ConsumeTick()`, `EndTick()`, `SetTimeScale()`
- **Private Member Variables:** `m_camelCase`
  - `m_accNsHz`, `m_simTickCount`, `m_tickOpen`, `m_isPaused`
- **Constants:** `kPascalCase`
  - `kDefaultSimHz`, `kNsPerSec`, `kMaxCatchupTicks`
- **Standard Types:**
  - Prefer explicit fixed-width integer types from `<cstdint>`: `std::uint64_t`, `std::uint32_t`, `std::size_t`.
  - Use `double` for high-precision time calculations, `float` for vector math / rendering transforms.

---

## 3. API Ergonomics & Golden Boundaries

1. **State Machine Guards (`m_tickOpen`):**
   - Enforce lifecycle symmetry on tick consumers:
     ```cpp
     bool TimeManager::ConsumeTick() {
         if (m_paused || m_tickOpen || m_accNsHz < kNsPerSec) return false;
         m_tickOpen = true;
         return true;
     }
     void TimeManager::EndTick() {
         if (!m_tickOpen) return;
         // ... advance sim ...
         m_tickOpen = false;
     }
     ```
2. **Const-Correctness on System Clocks:**
   - `const TimeChannel& RealTime() const noexcept;`
   - Gameplay code must never be able to mutate or scale the wall clock.
3. **Defensive Numerical Checks:**
   - Always check `std::isfinite(val)` on external floats/doubles before assigning or clamping.
   - Use epsilon thresholds (`1e-9`) instead of exact `> 0.0` for floating-point countdown timers.
4. **Zero-Drift Accumulator Math:**
   - Store simulation time as integer ticks or integer scaled units (`ns * Hz`), never raw float `time += dt`.
   - 1 sim tick costs exactly $10^9$ units when accumulated as $ns \times Hz$.

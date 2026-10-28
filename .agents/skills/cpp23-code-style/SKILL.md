---
name: cpp23-code-style
description: Write, refactor, and review C++23 code with clear contracts and invariants. Trigger when the user requests C++ or C++23 code generation, refactoring, architecture design, or code reviews.
---
# C++23 Code Style

Write direct, verifiable C++23 systems code with clear contracts, ownership rules, and memory safety.

## When to Use

- Writing new C++23 headers or implementation files.
- Refactoring C++ code for memory safety, ABI layout, or modern standards.
- Reviewing C++ interfaces, ownership models, and struct invariants.

## Core Rules

### 1. Documentation and Comments (English Only)

- **No Decorative Dividers:** Do not use ASCII banner comments (`// ---`, `// ===`, `// ***`). Use class access specifiers and clean file structure instead.
- **No Meta-Narration:** Never write comments stating what the syntax already says (`// Constructor`, `// Loop over items`, `// Added fix`).
- **Type Docstrings (Contracts & Invariants):** Before non-trivial classes or structs, write 2 to 3 lines stating:
  1. **Responsibility:** What concrete role the type fulfills.
  2. **Ownership:** Lifetime, memory ownership, or thread-safety contract (e.g., value type, non-owning view, move-only handle).
  3. **Invariant:** Conditions that always hold for valid instances (including size/alignment guarantees where applicable).
- **Inline Comments (Why, Not What):** Comment only to explain hardware or OS constraints, ABI/alignment rules, overflow guards, or non-obvious edge cases.

### 2. Qualifiers and Type Safety

- **`constexpr` and `consteval`:** Mark pure math, value-type operations, bit manipulation, and deterministic state transitions `constexpr`. Use `consteval` only when compile-time evaluation is required (such as literal operators or static lookup tables). Do not mark OS I/O or thread synchronization functions `constexpr`.
- **`[[nodiscard]]`:** Apply to pure queries, factory functions, bounds-checked readers/writers, and functions returning `std::expected` / `Result`.
- **`noexcept`:** Apply to functions in `-fno-exceptions` targets and to move operations, destructors, and non-throwing queries. If a `noexcept` boundary wraps a standard library call that can throw when exceptions are enabled (e.g., `std::thread`), guard it so failure returns an error code instead of calling `std::terminate`.
- **C++23 Standard Types:**
  - Use `std::expected<T, E>` (`Result<T, E>`) for fallible operations.
  - Use `std::span<T>` / `std::span<const T>` and `std::string_view` for non-owning buffer and string views.
  - Use `std::bit_cast` for safe value-representation reinterpretation and `std::to_underlying` for enums.
  - Decay forwarding references with `std::remove_reference_t<T>` and `std::addressof` when storing callable pointers in thunks.

### 3. Memory Safety and ABI Discipline (KISS)

- **Zero Undefined Behavior:** Guard against integer overflow in index and batch math (use `((count - 1U) / batch) + 1U`), validate enum/index bounds before bit-shifts or array indexing, and use `u8` (sanitized to `0` or `1`) instead of `bool` inside `std::memcpy`-serialized structs.
- **Explicit Layout Verification:** Verify cache-critical or serialized structs with `static_assert(sizeof(T) == ExpectedBytes)`. Avoid bloating hot structs with unnecessary fields.
- **Rule of Zero / Rule of Five:** Use value types by default. When wrapping OS handles (`SDL_Window*`, threads, arena slabs), delete copy operations and implement `noexcept` move or explicit `Init`/`Shutdown` lifecycle methods.

## Gotchas

- Never mutate cached state before verifying that the underlying OS or hardware call succeeded.
- Never call non-async-signal-safe routines (`std::fopen`, `malloc`, blocking mutex locks) inside a POSIX/C signal handler.
- Keep headers self-contained and independent of inclusion order.

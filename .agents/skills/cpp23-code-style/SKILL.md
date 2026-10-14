---
name: cpp23-code-style
description: Write, refactor, and review modern C++23 code following clean, human-authored engineering standards. Trigger whenever the user requests C++ or C++23 code generation, refactoring, architecture design, or code reviews. Enforces strict comment hygiene (English only, no ASCII section dividers, concise responsibility/ownership/invariant class docstrings, "why not what" inline comments), proper qualifier usage (constexpr, [[nodiscard]], noexcept), and zero redundant or bloated boilerplate.
---
# C++23 Code Style & Engineering Standards

Write idiomatic, production-grade C++23 code that reads like it was crafted by a senior systems/engine programmer, avoiding AI-generated verbosity, decorative comments, and redundant abstractions.

## When to Use

- Writing new C++ / C++23 headers, modules, or implementation files
- Refactoring existing C++ code for readability, performance, or modern standards
- Reviewing C++ architecture, memory ownership, or API design

## Steps & Core Rules

### 1. Comment & Documentation Hygiene (English Only)

All comments and docstrings must be written in clear, concise **English**.

- **Zero Decorative Dividers:** Never use ASCII banner comments or section separators such as `// ---`, `// ===`, `// ***`, or `// === Public API ===`. Let clean code structure and access specifiers (`public:`, `private:`) speak for themselves.
- **No AI-Style Narrration:** Never leave meta-comments like `// Here we implement...`, `// Added as requested`, `// Constructor`, or `// Destructor`.
- **Class & Subsystem Docstrings:** Write **2 to 4 lines** before significant classes, structs, or subsystems focusing strictly on three pillars:
  1. **Responsibility:** What concrete problem this type solves.
  2. **Ownership:** Lifetime expectations, memory ownership, or thread-safety guarantees (e.g., non-owning view, move-only resource handle).
  3. **Invariants:** Conditions that always hold true for valid instances.
  - *Skip docstrings* on trivial getters, self-explanatory structs, or standard overrides.
- **Inline Comments ("The Why, Not The What"):** Never explain *what* the syntax is doing. Only comment to explain hardware/ABI constraints, non-obvious algorithmic trade-offs, math edge cases, or intentional workarounds.

```cpp
// GOOD: Class docstring (Responsibility, Ownership, Invariants)
// Manages fixed-timestep simulation ticks decoupled from display framerate.
// Non-owning utility; caller owns the clock source.
// Invariant: step_delta_ is strictly positive and accumulator_ never exceeds max_frame_delta_.
class TimeManager { ... };

// GOOD: Inline comment (The Why)
// Clamp delta to prevent spiral-of-death after debugger pauses or OS window dragging.
const double frame_time = std::min(raw_delta, max_frame_delta_);
```

### 2. Modern C++23 Idioms & Qualifiers

Apply qualifiers deliberately based on semantics, not by slapping them onto every declaration blindly.

- **`constexpr` / `consteval`:**
  - Use `constexpr` for pure calculations, math utilities, small value types, lookup tables, and getters whose values can be evaluated at compile-time or help constant folding.
  - Use `consteval` only when compile-time evaluation is strictly required (e.g., compile-time hashes, format string checks).
  - Do *not* clutter heavy I/O, OS-wrapping, or stateful runtime subsystem methods with `constexpr`.
- **`[[nodiscard]]`:**
  - Apply `[[nodiscard]]` to functions where ignoring the return value is a logic bug, resource leak, or wasted computation: factory functions, `std::expected` / error-returning functions, pure query/math functions, and state checks (`empty()`, `is_valid()`).
  - Do *not* put `[[nodiscard]]` on mutators that return `*this` for optional chaining or functions with primary side effects.
- **`noexcept`:**
  - Mark move constructors, move assignment operators, `swap`, destructors, and simple non-allocating queries/math functions `noexcept`.
  - Avoid `noexcept` on functions that allocate dynamic memory or call external callbacks unless termination on failure is intentional.
- **C++23 Features:**
  - Prefer `std::expected<T, E>` for recoverable errors instead of exceptions or out-parameters.
  - Prefer `std::span<const T>` and `std::string_view` over raw pointer+length pairs or `const std::vector<T>&` / `const std::string&` parameters.
  - Use `std::print` / `std::println` instead of `std::cout` / `printf`.
  - Use Deduced `this` (explicit object parameters) to eliminate `const` / non-`const` accessor duplication when needed.
  - Use `std::ranges` and views when they improve clarity, or simple range-based `for` loops when ranges pipelines become unreadable.

### 3. Anti-Bloat & Clean Architecture (Zero Redundancy)

- **No Redundant Wrappers or Overloads:** Do not create convenience overloads that merely forward arguments with minor type conversions if a single signature with default parameters, `std::span`, or `std::string_view` suffices.
- **No Trivial Getters/Setters:** If a type is a passive data aggregate with no invariants, make it a `struct` with public members. Only make members `private` when maintaining a class invariant.
- **Rule of Zero by Default:** Rely on RAII members (`std::unique_ptr`, `std::vector`, custom handles) and omit custom destructors/copy/move operations. When wrapping raw C/OS resources (e.g., SDL handles, sockets, GPU buffers), enforce the **Rule of Five** explicitly (usually deleting copy and implementing `noexcept` move).
- **DRY Without Over-Abstraction:** Consolidate duplicated logic into a single private helper or template (or deduced `this` method), but do not invent deep inheritance hierarchies or single-use interface classes.

## Gotchas to Avoid

- **Over-commenting:** If the function name and parameter types already state the intent, write zero comments.
- **Getter/Setter Const Duplication:** Instead of writing identical `get()` and `get() const` bodies, use C++23 explicit object member functions (`auto&& self`) or keep data simple.
- **Speculative Boilerplate:** Never generate unrequested `ToString()`, `Serialize()`, or dummy constructors "just in case". Write only what is required for a clean, minimal, working design.

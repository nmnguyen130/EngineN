# `engine/input/` — Contracts & Invariants

## Files
- `engine/input/InputTypes.h` (Header-only `constexpr` descriptors, `InputMap`, `TickInput`, `FrameInput`)
- `engine/input/ActionBuffer.h` (Header-only `constexpr` pre-input press/release/coyote buffer)
- `engine/input/InputPipeline.h` (Header-only `constexpr` physical state tracker, replay stream, and tick/frame sampler)

## Contracts & Invariants

### 1. Input Descriptors & Bindings (`InputTypes.h`)
- **Zero OS/Platform Coupling:** `engine/input/` depends only on `core/Math.h` and never includes `platform/` or `<SDL3/...>` headers.
- **`InputSource` (`2B`, `alignas(2)`):** Packs 14-bit `code` and 2-bit `InputDevice` (`None`, `Keyboard`, `Mouse`, `Gamepad`).
  - **Validity Bounds (`IsValid()`):**
    - `Keyboard`: `code` in `[1..511]`
    - `Mouse`: `code` in `[1..7]`
    - `Gamepad`: `code` in `[0..31]` (`[0..19]` physical buttons, `[20..31]` virtual Schmitt-trigger axis buttons)
- **`AxisBinding` (`16B`, `alignas(4)`):** Primary and alternate digital negative/positive sources, `f32 deadzone`, `u8 gamepadAxis`, `u8 invert`, `u8 neutralSocd`, `u8 reserved`.
  - **Sanitization Invariant:** `Sanitize()` clears invalid `InputSource` members, clamps `deadzone` to `[0.0f, 0.90f]` (resetting out-of-range or `NaN` values to `0.15f`), normalizes `invert` and `neutralSocd` to `{0, 1}`, and zeroes `reserved`.
- **`InputMap` (`192B`, `alignas(16)`):** 16 actions x 4 bindings (`128B`) + 4 `AxisBinding` entries (`64B`).
  - **Safe Serialization Invariant:** Because `AxisBinding` uses `u8` instead of `bool` and explicitly zeroes `reserved`, `SerializeBindings` and `DeserializeBindings` use clean `std::memcpy` (`192B`) followed by `IsValid()` and `Sanitize()` normalization with zero undefined behavior.

### 2. Physical State Queries & Sampling (`InputPipeline.h`, `ActionBuffer.h`)
- **Bounds-Checked Physical Query Invariant:** `InputPipeline::IsPhysicallyDown(InputSource src)` validates `if (!src.IsValid()) return false;` before indexing `m_keyBits` or shifting bitmasks for `m_mouseBits` and `m_padButtons`, preventing out-of-bounds array reads and undefined bit-shifts on arbitrary public queries.
- **Multi-Binding Edge Deduplication:** Pressing a secondary key bound to an action already held by a primary key does not emit a false `pressed` or `retriggered` edge.
- **Schmitt-Trigger Analog Hysteresis:** `FeedGamepadAxis` drives virtual buttons `[20..31]` using `kAxisPressThreshold = 18000` and `kAxisReleaseThreshold = 12000`.
- **`ActionBuffer` (`36B`, `alignas(4)`):** Tracks press and release ages (`kMaxBufferTicks = 60`) across 16 actions with bitmask-accelerated aging, priority consumption (`ConsumeHighestPriority`), and coyote-time jump consumption (`TryConsumeJump`).

### 3. Struct Sizes (`static_assert` Locked)
- `InputSource`: `2B` (`alignas(2)`)
- `AxisBinding`: `16B` (`alignas(4)`)
- `InputMap`: `192B` (`alignas(16)`)
- `TickInput`: `64B` (`alignas(16)`)
- `FrameInput`: `64B` (`alignas(16)`)
- `ActionBuffer`: `36B` (`alignas(4)`)
- `InputReplayStream::Segment`: `12B` (`alignas(4)`)
- `InputReplayStream`: `3112B` (`alignas(4)`)
- `InputPipeline`: `384B` (`alignas(16)`)

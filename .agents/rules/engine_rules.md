# EngineN Contracts & Invariants

1. **Boot Sequence:** Initialize `Diagnostics` (`Logger` sink + `CrashHandler::Install`) -> `Platform::Init` (`SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD`) -> Mount `.enpak` & `JobSystem::Init` -> Load settings/save (verify CRC32).
2. **Fixed 120 Hz Simulation:** Run all gameplay, AI, combat, and physics inside `while (SimStepView step = time.PollTick())` using `step.Dt(channel)` and `step.IsFrozen(channel)`. `TimePipeline` keeps `m_bakedTick` stable for the duration of the current tick and advances channel modulation at the next tick boundary.
3. **Render Interpolation:** Skip rendering when `!window.State().IsPresentable()`. Interpolate world transforms using `RenderTimeView::Alpha(channel)` before submitting to `Renderer`.
4. **Persistence & Assets:** Validate CRC32 checksums on all save and snapshot buffers. Write files atomically via timestamped `.tmp` files and rename. Stream and evict `.enpak` assets within the configured memory budget.
5. **Memory & ABI Safety:** Zero UB, zero exceptions/RTTI, explicit bounds checks on public queries, `u8` boolean flags in `std::memcpy`-serialized structs, and `static_assert(sizeof(...))` on all cache-aligned structs.

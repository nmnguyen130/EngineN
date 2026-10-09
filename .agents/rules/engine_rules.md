# EngineN Coding & Architectural Rules

1. **Deterministic 120Hz Loop:** All gameplay logic and physics must run in the fixed simulation update at 120Hz.
2. **Nanosecond Time:** Always use `uint64_t` nanoseconds for time arithmetic internally.
3. **Decoupled Render Interpolation:** Render at display rate using `alpha` interpolation. Never snap rendering directly to un-interpolated physics ticks unless specifically toggled for debug.
4. **Motion Arbiter:** States must submit requests to `MotionArbiter` using priority tiers rather than directly overwriting velocities.
5. **No Box2D:** Use custom Kinematic Swept AABB.

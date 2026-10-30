#pragma once

#include "audio/AudioTypes.h"
#include "core/Log.h"
#include "core/Math.h"
#include "renderer/RenderList.h"
#include "time/TimeTypes.h"
#include "world/Components.h"

#ifndef EN_ENABLE_DEBUG_TOOLS
#if defined(EN_SHIPPING)
#define EN_ENABLE_DEBUG_TOOLS 0
#else
#define EN_ENABLE_DEBUG_TOOLS 1
#endif
#endif

namespace en {

class World;
class PhysicsSystem;
class InputSystem;
struct TickInput;
class ActionBuffer;

inline constexpr u8  kDebugWorldPass   = 2U;
inline constexpr u8  kDebugHudPass     = 3U;
inline constexpr u8  kDebugWorldView   = 0U;
inline constexpr u8  kDebugHudView     = RenderList::kHudViewSlot;
inline constexpr u16 kDebugDepth       = 65000U;
inline constexpr u32 kMaxGizmos        = 1024U;
inline constexpr u32 kProfileFrames    = 128U;
inline constexpr u32 kProfileBuckets   = 32U;
inline constexpr u32 kFlightTicks      = 64U;
inline constexpr u32 kMaxSymbols       = 128U;
inline constexpr u32 kMaxConsoleLogs   = 256U;
inline constexpr u32 kConsoleTextBytes = 8192U;

enum class DebugErrc : u8 {
    None = 0,
    OutOfMemory,
    SymbolFull,
    DuplicateSymbol,
    NotFound,
    BadArgs,
};

enum class Overlay : u32 {
    None       = 0U,
    Colliders  = 1U << 0,
    Velocity   = 1U << 1,
    Hitboxes   = 1U << 2,
    Broadphase = 1U << 3,
    Audio      = 1U << 4,
    Inspector  = 1U << 5,
    Profiler   = 1U << 6,
    Console    = 1U << 7,
};
consteval bool enable_bitmask_operators(Overlay) noexcept { return true; }

enum class ProfileScope : u8 {
    Frame = 0,
    Poll,
    Sim,
    Physics,
    Gameplay,
    Audio,
    Extract,
    Debug,
    Submit,
    Count,
};

// 8-byte frame loop control directives issued by DebugSystem hotkeys or console commands.
struct LoopDirectives {
    static constexpr u8 kTogglePause = 1U << 0;
    static constexpr u8 kForcePause  = 1U << 1;
    static constexpr u8 kReset       = 1U << 2;

    f32 timeScale{-1.0f};
    u16 stepTicks{0};
    u8  timeChannel{0};
    u8  flags{0};

    [[nodiscard]] constexpr bool TogglePause() const noexcept { return (flags & kTogglePause) != 0U; }
    [[nodiscard]] constexpr bool ForcePause() const noexcept  { return (flags & kForcePause) != 0U; }
    [[nodiscard]] constexpr bool Reset() const noexcept       { return (flags & kReset) != 0U; }
};
static_assert(sizeof(LoopDirectives) == 8);

// 32-byte transient or timed 2D debug gizmo primitive queued in ShapePool.
struct ShapeCmd {
    enum class Kind : u8 { Line, Box, Solid, Arrow, Circle, Cross, TextWorld, TextHud };

    f32 x0{0.0f};
    f32 y0{0.0f};
    union {
        struct { f32 x1, y1, aux; } g;
        char text[12];
    } u{};
    u32  rgba{0xFFFFFFFFU};
    u32  expire{0};
    u16  thickQ8{256};
    Kind kind{Kind::Line};
    u8   aux{0};
};
static_assert(sizeof(ShapeCmd) == 32);

// 32-byte per-frame timing and subsystem counter record stored in DebugProfiler.
struct FrameSample {
    std::array<u16, static_cast<usize>(ProfileScope::Count)> us{};
    u16 entities{0};
    u16 quads{0};
    u16 batches{0};
    u16 contacts{0};
    u8  voices{0};
    u8  worldScaleQ6{64};
    u8  playerScaleQ6{64};
    u8  pad[3]{};
};
static_assert(sizeof(FrameSample) == 32);

// 32-byte flight-recorder snapshot for a pinned entity in DebugInspector.
struct ActorSample {
    u32 tick{0};
    f32 x{0.0f};
    f32 y{0.0f};
    f32 vx{0.0f};
    f32 vy{0.0f};
    u16 entity{0};
    i16 hp{0};
    i8  ax{0};
    i8  ay{0};
    u8  held{0};
    u8  pressed{0};
    u8  buffer{0};
    u8  state{0};
    u8  iframe{0};
    u8  contacts{0};
};
static_assert(sizeof(ActorSample) == 32);

// 24-byte frame telemetry input passed to DebugProfiler::RecordTelemetry.
struct Telemetry {
    const f32* scales{nullptr};
    u16        entities{0};
    u16        quads{0};
    u16        batches{0};
    u16        contacts{0};
    u8         voices{0};
    u8         pad[3]{};
};
static_assert(sizeof(Telemetry) == 24);

// 48-byte read-only context passed to DebugSystem::Emit each frame.
struct FrameCtx {
    const World*                      world{nullptr};
    const PhysicsSystem*              physics{nullptr};
    std::span<const AudioEmitterView> emitters{};
    Viewport                          viewport{1280, 720};
    u32                               tick{0};
    u8                                worldView{0};
    u8                                pad[3]{};
};
static_assert(sizeof(FrameCtx) == 48);

// 16-byte initialization configuration for DebugSystem.
struct DebugConfig {
    u32     arenaBytes{65536};
    Overlay overlays{Overlay::Profiler};
    u16     keyConsole{53};
    u16     keyPause{72};
    u16     keyStep{67};
    u16     keyProfiler{60};
};
static_assert(sizeof(DebugConfig) == 16);

} // namespace en

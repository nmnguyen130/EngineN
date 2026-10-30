#pragma once

#include "audio/AudioTypes.h"
#include "core/Math.h"
#include "core/Types.h"
#include "renderer/RenderList.h"
#include "time/TimeTypes.h"
#include "world/WorldTypes.h"

#include <array>
#include <span>
#include <string_view>

#ifndef EN_ENABLE_DEBUG_TOOLS
#if defined(NDEBUG)
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

inline constexpr u8  kDebugWorldPass   = 14U;
inline constexpr u8  kDebugHudPass     = 15U;
inline constexpr u8  kDebugWorldView   = 0U;
inline constexpr u8  kDebugHudView     = RenderList::kHudViewSlot;
inline constexpr u16 kDebugDepth       = 65000U;
inline constexpr u32 kMaxShapes        = 1024U;
inline constexpr u32 kProfileFrames    = 128U;
inline constexpr u32 kProfileBuckets   = 32U;
inline constexpr u32 kFlightTicks      = 64U;
inline constexpr u32 kMaxSymbols       = 128U;
inline constexpr u32 kMaxLogs          = 256U;
inline constexpr u32 kLogTextBytes     = 8192U;

enum class DebugErrc : u8 {
    None = 0,
    OutOfMemory,
    ArenaExhausted,
    SymbolFull,
    DuplicateSymbol,
    NotFound,
    BadArgs,
};

enum class Overlay : u32 {
    None       = 0U,
    Colliders  = 1U << 0U,
    Velocity   = 1U << 1U,
    Hitboxes   = 1U << 2U,
    Contacts   = 1U << 3U,
    Broadphase = 1U << 4U,
    Inspector  = 1U << 5U,
    Profiler   = 1U << 6U,
    Console    = 1U << 7U,
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

enum class LogLevel : u8 { Info = 0, Warn, Error, Cmd };

struct alignas(8) LoopDirectives {
    static constexpr u8 kTogglePause = 1U << 0U;
    static constexpr u8 kForcePause  = 1U << 1U;
    static constexpr u8 kReset       = 1U << 2U;

    f32 timeScale{-1.0f};
    u16 stepTicks{0U};
    u8  timeChannel{0U};
    u8  flags{0U};

    [[nodiscard]] constexpr bool TogglePause() const noexcept { return (flags & kTogglePause) != 0U; }
    [[nodiscard]] constexpr bool ForcePause()  const noexcept { return (flags & kForcePause)  != 0U; }
    [[nodiscard]] constexpr bool Reset()       const noexcept { return (flags & kReset)       != 0U; }
};
static_assert(sizeof(LoopDirectives) == 8);

struct alignas(32) ShapeCmd {
    enum class Kind : u8 { Line, Box, Solid, Arrow, Circle, Capsule, TextWorld, TextHud };

    f32 x0{0.0f};
    f32 y0{0.0f};
    union {
        struct { f32 x1, y1, aux; } g;
        char text[12];
    } u{};
    u32  rgba{0xFFFFFFFFU};
    u32  expire{0U};
    u16  thickQ8{256U};
    Kind kind{Kind::Line};
    u8   aux{0U};
};
static_assert(sizeof(ShapeCmd) == 32);

struct alignas(32) FrameSample {
    std::array<u16, static_cast<usize>(ProfileScope::Count)> us{};
    u16 entities{0U};
    u16 quads{0U};
    u16 batches{0U};
    u16 contacts{0U};
    u8  voices{0U};
    u8  worldQ6{64U};
    u8  playerQ6{64U};
    u8  _pad{0U};
};
static_assert(sizeof(FrameSample) == 32);

struct alignas(32) ActorSample {
    u32 tick{0U};
    f32 x{0.0f};
    f32 y{0.0f};
    f32 vx{0.0f};
    f32 vy{0.0f};
    u16 entity{0U};
    i16 hp{0};
    i8  ax{0};
    i8  ay{0};
    u8  held{0U};
    u8  pressed{0U};
    u8  buffer{0U};
    u8  state{0U};
    u8  iframe{0U};
    u8  contacts{0U};
};
static_assert(sizeof(ActorSample) == 32);

struct alignas(8) Telemetry {
    const f32* scales{nullptr};
    u16 entities{0U};
    u16 quads{0U};
    u16 batches{0U};
    u16 contacts{0U};
    u8  voices{0U};
    u8  _pad[3]{};
};
static_assert(sizeof(Telemetry) == 24);

struct alignas(16) FrameCtx {
    const World*                      world{nullptr};
    const PhysicsSystem*              physics{nullptr};
    std::span<const AudioEmitterView> emitters{};
    Viewport                          viewport{1280, 720};
    u32                               tick{0U};
    u8                                worldView{0U};
};
static_assert(sizeof(FrameCtx) == 48);

struct alignas(8) DebugConfig {
    u32     arenaBytes{65536U};
    Overlay overlays{Overlay::Profiler};
    u16     keyConsole{53U};
    u16     keyPause{72U};
    u16     keyStep{67U};
    u16     keyProfiler{60U};
};
static_assert(sizeof(DebugConfig) == 16);

} // namespace en

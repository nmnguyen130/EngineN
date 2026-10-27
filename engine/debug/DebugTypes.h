#pragma once

#include "audio/AudioTypes.h"
#include "core/Math.h"
#include "core/Memory.h"
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
class InputPipeline;
struct TickInput;
class ActionBuffer;

inline constexpr u8 kDebugWorldPassId = 14U;
inline constexpr u8 kDebugHudPassId = 15U;
inline constexpr u8 kDebugWorldViewSlot = 0U;
inline constexpr u8 kDebugHudViewSlot = RenderList::kHudViewSlot;
inline constexpr u16 kDebugFixedDepth = 65000U;
inline constexpr TextureHandle kBuiltinFontTexture{1, 1};
inline constexpr u32 kMaxDebugShapes = 2048U;
inline constexpr u32 kProfileHistoryFrames = 256U;
inline constexpr u32 kProfileBucketCount = 48U;
inline constexpr u32 kActorFlightHistoryTicks = 64U;
inline constexpr u32 kMaxWatchPlots = 4U;
inline constexpr u32 kWatchPlotHistorySamples = 56U;

enum class DebugErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    ArenaExhausted,
    SymbolTableFull,
    DuplicateSymbol,
    SymbolNotFound,
    InvalidArguments,
    ValueOutOfRange,
};

enum class OverlayFlags : u32 {
    None               = 0U,
    SpatialTransforms  = 1U << 0U,
    PhysicsColliders   = 1U << 1U,
    VelocityVectors    = 1U << 2U,
    ContactManifolds   = 1U << 3U,
    SpatialBroadphase  = 1U << 4U,
    CombatHitboxes     = 1U << 5U,
    SafeAnchors        = 1U << 6U,
    CameraRigOverlay   = 1U << 7U,
    AudioEmitters      = 1U << 8U,
    EntityInspector    = 1U << 9U,
    FlightScrubber     = 1U << 10U,
    ProfilerHud        = 1U << 11U,
    TimeChannelHud     = 1U << 12U,
    InputViewerHud     = 1U << 13U,
    WatchPlotHud       = 1U << 14U,
    ConsoleOverlay     = 1U << 15U,
    AllWorld           = 0x000001FFU,
    AllHud             = 0x0000FE00U,
};

consteval bool enable_bitmask_operators(OverlayFlags) noexcept { return true; }

enum class ProfileScopeId : u8 {
    FrameTotal     = 0,
    PlatformPoll   = 1,
    SimTickTotal   = 2,
    WorldTransform = 3,
    PhysicsStep    = 4,
    GameplayStep   = 5,
    AudioUpdate    = 6,
    WorldExtract   = 7,
    DebugOverlay   = 8,
    RenderSubmit   = 9,
    Count          = 10,
};

enum class LogSeverity : u8 {
    Info    = 0,
    Warning = 1,
    Error   = 2,
    Command = 3,
};

struct alignas(8) DebugLoopDirectives {
    static constexpr u8 kFlagTogglePause = 1U << 0U;
    static constexpr u8 kFlagForcePause  = 1U << 1U;
    static constexpr u8 kFlagResetState  = 1U << 2U;

    f32 channelTimeScale{-1.0f};
    u16 stepTicks{0U};
    u8 targetTimeChannel{0U};
    u8 controlFlags{0U};

    [[nodiscard]] constexpr bool TogglePause() const noexcept { return (controlFlags & kFlagTogglePause) != 0U; }
    [[nodiscard]] constexpr bool ForcePause() const noexcept { return (controlFlags & kFlagForcePause) != 0U; }
    [[nodiscard]] constexpr bool ResetRequested() const noexcept { return (controlFlags & kFlagResetState) != 0U; }
};
static_assert(sizeof(DebugLoopDirectives) == 8);

struct alignas(32) DebugShapeCmd {
    enum class Kind : u8 {
        Line       = 0,
        Box        = 1,
        SolidBox   = 2,
        Arrow      = 3,
        Circle     = 4,
        CapsuleY   = 5,
        TextWorld  = 6,
        TextScreen = 7,
    };

    f32 x0{0.0f};
    f32 y0{0.0f};
    union {
        struct {
            f32 x1;
            f32 y1;
            f32 auxFloat;
        } geom;
        char text[12];
    } payload{};
    u32 rgba8{0xFFFFFFFFU};
    u32 expireTick{0U};
    u16 thicknessQ8{256U};
    Kind kind{Kind::Line};
    u8 auxByte{0U};
};
static_assert(sizeof(DebugShapeCmd) == 32);

// Compact 32-byte per-frame telemetry sample (2 samples per 64B cache line, 0 dead fields).
struct alignas(32) FrameProfileSample {
    std::array<u16, static_cast<usize>(ProfileScopeId::Count)> scopeUs{};
    u16 aliveEntities{0U};
    u16 submittedQuads{0U};
    u16 drawBatches{0U};
    u16 activeContacts{0U};
    u8 audioActiveVoices{0U};
    u8 audioVirtualVoices{0U};
    u8 worldScaleQ6{64U};
    u8 playerScaleQ6{64U};
};
static_assert(sizeof(FrameProfileSample) == 32);

// Compact 32-byte per-tick actor snapshot (2 records per 64B cache line).
struct alignas(32) ActorTickRecord {
    u32 tickIndex{0U};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 velX{0.0f};
    f32 velY{0.0f};
    u16 entityIndex{0U};
    i16 hp{0};
    i8 axisX_q7{0};
    i8 axisY_q7{0};
    u8 inputHeld{0U};
    u8 inputPressed{0U};
    u8 actionBufferMask{0U};
    u8 stateId{0U};
    u8 iframeTicks{0U};
    u8 contactFlags{0U};
};
static_assert(sizeof(ActorTickRecord) == 32);

struct alignas(8) SubsystemTelemetryDesc {
    const f32* channelScales{nullptr};
    u16 aliveEntities{0U};
    u16 submittedQuads{0U};
    u16 drawBatches{0U};
    u16 activeContacts{0U};
    u16 audioActiveVoices{0U};
    u16 audioVirtualVoices{0U};
    u16 reserved{0U};
};
static_assert(sizeof(SubsystemTelemetryDesc) == 24);

// Exact 256-byte (4 cache lines) sparkline plot track.
struct alignas(64) WatchPlotTrack {
    std::array<f32, kWatchPlotHistorySamples> samples{};
    std::array<char, 16> label{};
    f32 minVal{0.0f};
    f32 maxVal{0.0f};
    u32 nameHash{0U};
    u16 writeIdx{0U};
    u8 validCount{0U};
    u8 labelLen{0U};

    [[nodiscard]] constexpr std::string_view Label() const noexcept {
        return {label.data(), labelLen};
    }
};
static_assert(sizeof(WatchPlotTrack) == 256);

struct alignas(16) DebugFrameContext {
    const World* world{nullptr};
    const PhysicsSystem* physics{nullptr};
    std::span<const AudioEmitterView> audioEmitters{};
    ViewportExtent viewportSize{1280, 720};
    u32 currentTickIndex{0U};
    u8 worldViewSlot{0U};
};
static_assert(sizeof(DebugFrameContext) == 48);

struct alignas(8) DebugSystemConfig {
    u32 arenaBytes{131072U};
    OverlayFlags initialOverlays{OverlayFlags::ProfilerHud};
    u16 toggleConsoleScancode{53U};  // Grave / Tilde (`~`) key
    u16 togglePauseScancode{72U};    // Pause / F9 key
    u16 stepTickScancode{67U};       // F10 single-step key
    u16 toggleProfilerScancode{60U}; // F3 key
};
static_assert(sizeof(DebugSystemConfig) == 16);

} // namespace en

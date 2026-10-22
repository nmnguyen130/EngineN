#pragma once

#include "audio/AudioTypes.h"
#include "core/Math.h"
#include "core/Memory.h"
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
class GameplayPipeline;
class AudioSystem;
class InputPipeline;
struct TickInput;
struct FrameInput;
class ActionBuffer;

inline constexpr u8 kDebugWorldPassId = 14U;
inline constexpr u8 kDebugHudPassId = 15U;
inline constexpr u8 kDebugWorldViewSlot = 0U;
inline constexpr u8 kDebugHudViewSlot = RenderList::kHudViewSlot;
inline constexpr u16 kDebugFixedDepth = 65000U;
inline constexpr u32 kMaxDebugShapes = 2048U;
inline constexpr u32 kProfileHistoryFrames = 256U;
inline constexpr u32 kProfileBucketCount = 64U;
inline constexpr u32 kActorFlightHistoryTicks = 64U;
inline constexpr u32 kMaxWatchPlots = 4U;
inline constexpr u32 kWatchPlotHistorySamples = 64U;

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
    PhysicsJoints      = 1U << 9U,
    EntityInspector    = 1U << 10U,
    FlightScrubber     = 1U << 11U,
    ProfilerHud        = 1U << 12U,
    TimeChannelHud     = 1U << 13U,
    InputViewerHud     = 1U << 14U,
    WatchPlotHud       = 1U << 15U,
    ConsoleOverlay     = 1U << 16U,
    AllWorld           = 0x000003FFU,
    AllHud             = 0x0000FC00U,
};

consteval bool enable_bitmask_operators(OverlayFlags) noexcept { return true; }

[[nodiscard]] constexpr bool HasOverlay(OverlayFlags mask, OverlayFlags test) noexcept {
    return HasFlag(mask, test);
}

enum class ProfileScopeId : u8 {
    FrameTotal     = 0,
    PlatformPoll   = 1,
    InputSample    = 2,
    SimTickTotal   = 3,
    WorldTransform = 4,
    PhysicsStep    = 5,
    GameplayStep   = 6,
    AudioUpdate    = 7,
    WorldExtract   = 8,
    DebugOverlay   = 9,
    RenderCompile  = 10,
    RenderSubmit   = 11,
    Count          = 12,
};

enum class DebugShapeKind : u8 {
    Line       = 0,
    Box        = 1,
    SolidBox   = 2,
    Arrow      = 3,
    Circle     = 4,
    CapsuleY   = 5,
    TextWorld  = 6,
    TextScreen = 7,
};

enum class LogSeverity : u8 {
    Info    = 0,
    Warning = 1,
    Error   = 2,
    Command = 3,
};

// 8-byte deterministic loop control directives returned by DebugSystem::BeginFrame().
struct alignas(8) DebugLoopDirectives {
    static constexpr u8 kFlagTogglePause = 1U << 0U;
    static constexpr u8 kFlagForcePause  = 1U << 1U;
    static constexpr u8 kFlagResetState  = 1U << 2U;

    f32 channelTimeScale{-1.0f}; // >= 0.0f overrides targetTimeChannel scale
    u16 stepTicks{0U};           // > 0 advances N simulation ticks while paused
    u8 targetTimeChannel{0U};    // TimeChannel index [0..7]
    u8 controlFlags{0U};

    [[nodiscard]] constexpr bool TogglePause() const noexcept { return (controlFlags & kFlagTogglePause) != 0U; }
    [[nodiscard]] constexpr bool ForcePause() const noexcept { return (controlFlags & kFlagForcePause) != 0U; }
    [[nodiscard]] constexpr bool ResetRequested() const noexcept { return (controlFlags & kFlagResetState) != 0U; }
};
static_assert(sizeof(DebugLoopDirectives) == 8);

// 32-byte (2 per 64B cache line, zero padding) transient/persistent debug shape record.
struct alignas(32) DebugShapeCmd {
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
    DebugShapeKind kind{DebugShapeKind::Line};
    u8 auxByte{0U};
};
static_assert(sizeof(DebugShapeCmd) == 32);

// 64-byte (1 cache line exact, zero padding) per-frame profiler and cross-subsystem telemetry sample.
struct alignas(64) FrameProfileSample {
    u32 frameIndex{0U};
    u32 simTickIndex{0U};
    std::array<u16, static_cast<usize>(ProfileScopeId::Count)> scopeUs{};
    u16 aliveEntities{0U};
    u16 submittedQuads{0U};
    u16 culledQuads{0U};
    u16 drawBatches{0U};
    u16 activeContacts{0U};
    u16 activeBodies{0U};
    std::array<u8, kMaxTimeChannels> channelScaleQ6{64U, 64U, 64U, 64U, 64U, 64U, 64U, 64U};
    u16 audioActiveVoices{0U};
    u16 audioVirtualVoices{0U};
    u16 audioStolenVoices{0U};
    u16 catchupTicks{0U};
    u8 frozenChannelMask{0U};
    u8 radixPasses{0U};
    u8 audioMusicStems{0U};
    u8 frameSpikeFlag{0U};
};
static_assert(sizeof(FrameProfileSample) == 64);

// 64-byte (1 cache line exact, zero padding) per-tick flight recorder snapshot for the pinned entity.
struct alignas(64) ActorTickRecord {
    u32 tickIndex{0U};
    EntityId entityId{kNullEntity};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 velX{0.0f};
    f32 velY{0.0f};
    i32 hp{0};
    i16 axisX_q8{0};
    i16 axisY_q8{0};
    u16 inputHeld{0U};
    u16 inputPressed{0U};
    u16 inputReleased{0U};
    u16 actionBufferMask{0U};
    u16 stateTicks{0U};
    std::array<i16, 4> hurtboxQ8{};
    std::array<i16, 4> hitboxQ8{};
    u8 stateId{0U};
    u8 bufferedActionId{0xFFU};
    u8 contactFlags{0U};
    u8 iframeTicks{0U};
    u8 hitstopTicks{0U};
    u8 groundCoyoteTicks{0U};
};
static_assert(sizeof(ActorTickRecord) == 64);

// 32-byte (2 per 64B cache line, zero padding) cross-subsystem telemetry aggregation descriptor.
struct alignas(32) SubsystemTelemetryDesc {
    const f32* channelScales{nullptr};
    u16 aliveEntities{0U};
    u16 submittedQuads{0U};
    u16 culledQuads{0U};
    u16 drawBatches{0U};
    u16 activeContacts{0U};
    u16 activeBodies{0U};
    u16 catchupTicks{0U};
    u16 audioActiveVoices{0U};
    u16 audioVirtualVoices{0U};
    u16 audioStolenVoices{0U};
    u8 frozenChannelMask{0U};
    u8 radixPasses{0U};
    u8 audioMusicStems{0U};
    u8 spikeThresholdMs{18U};
};
static_assert(sizeof(SubsystemTelemetryDesc) == 32);

// 288-byte (9 x 32B blocks exact, zero padding) real-time variable watch sparkline ring buffer.
struct alignas(32) WatchPlotTrack {
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
static_assert(sizeof(WatchPlotTrack) == 288);

// 64-byte (1 cache line exact, zero padding) read-only frame context for DebugSystem::BeginFrame and EmitOverlays.
struct alignas(64) DebugFrameContext {
    const World* world{nullptr};                        // 8B
    const PhysicsSystem* physics{nullptr};              // 8B
    const GameplayPipeline* gameplay{nullptr};          // 8B
    const RenderTimeView* timeView{nullptr};            // 8B
    std::span<const AudioEmitterView> audioEmitters{};  // 16B
    ViewportExtent viewportSize{1280, 720};             // 8B
    u32 currentTickIndex{0U};                           // 4B
    u16 stepChunkTicks{1U};                             // 2B
    u8 worldViewSlot{0U};                               // 1B
    u8 isPaused{0U};                                    // 1B
};
static_assert(sizeof(DebugFrameContext) == 64);

// 16-byte configuration descriptor for DebugSystem::Init().
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

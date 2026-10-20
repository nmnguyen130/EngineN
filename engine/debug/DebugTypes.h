#pragma once

#include "audio/AudioTypes.h"
#include "world/WorldTypes.h"
#include "time/TimePipeline.h"
#include "input/InputTypes.h"
#include "renderer/RendererTypes.h"

namespace en {

class World;
class GameplayPipeline;

enum class OverlayFlags : u32 {
    None = 0,
    SpatialTransforms = 1U << 0,
    KinematicBounds = 1U << 1,
    VelocityVectors = 1U << 2,
    CombatHitboxes = 1U << 3,
    TileCollisionMap = 1U << 4,
    DynamicSpatialGrid = 1U << 5,
    SafeBreadcrumbs = 1U << 6,
    CameraRigOverlay = 1U << 7,
    AudioEmitters = 1U << 8,
    EntityInspector = 1U << 9,
    FlightScrubber = 1U << 10,
    ProfilerHud = 1U << 11,
    TimeChannelHud = 1U << 12,
    InputViewerHud = 1U << 13,
    AllWorld = 0x01FFU,
    AllHud = 0x3E00U,
};
consteval bool enable_bitmask_operators(OverlayFlags) { return true; }

struct alignas(8) DebugLoopDirectives {
    f32 channelTimeScale{-1.0f};
    u8 stepTicks{0};
    u8 targetTimeChannel{0};
    u8 controlFlags{0}; // bit 0: togglePause, bit 1: forcePause
    bool stepRequested{false};

    [[nodiscard]] constexpr bool TogglePause() const noexcept { return (controlFlags & 1U) != 0; }
    [[nodiscard]] constexpr bool ForcePause() const noexcept { return (controlFlags & 2U) != 0; }
};
static_assert(sizeof(DebugLoopDirectives) == 8);

enum class ProfileScopeId : u8 {
    FrameTotal = 0,
    PlatformPump = 1,
    SimTickTotal = 2,
    Kinematics = 3,
    Combat = 4,
    AudioSync = 5,
    RenderExtract = 6,
    RenderSubmit = 7,
    Count = 8,
};

struct alignas(64) FrameProfileSample {
    u32 frameIndex{0};
    u32 simTickIndex{0};
    std::array<u16, 12> scopeUs{};
    u16 aliveEntities{0};
    u16 submittedQuads{0};
    u16 culledQuads{0};
    u16 drawBatches{0};
    std::array<u16, 8> channelScaleQ8{};
    u16 audioCpuPermille{0};
    u8 catchupTicks{0};
    u8 activePhysVoices{0};
    u8 frozenChannelMask{0};
    u8 radixPassesExecuted{0};
};
static_assert(sizeof(FrameProfileSample) == 64);

struct alignas(8) ActorTickRecord {
    u32 tickIndex{0};
    EntityId entityId{kNullEntity};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 velX{0.0f};
    f32 velY{0.0f};
    u16 inputHeld{0};
    u16 inputPressed{0};
    u16 inputReleased{0};
    u16 actionBufferMask{0};
    i16 hp{0};
    u16 stateTicks{0};
    u8 stateId{0};
    u8 contactFlags{0};
    u8 iframeTicks{0};
    u8 hitboxActive{0};
};
static_assert(sizeof(ActorTickRecord) == 40);

enum class LogSeverity : u8 {
    Info = 0,
    Warning = 1,
    Error = 2,
};

struct DebugFrameContext {
    const World* world{nullptr};
    const GameplayPipeline* gameplay{nullptr};
    RenderTimeView timeView{};
    FrameInput frameInput{};
    ViewportExtent viewportSize{1280, 720};
    std::span<const AudioEmitterView> audioEmitters{};
    bool isPaused{false};
};

} // namespace en

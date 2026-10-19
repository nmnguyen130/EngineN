#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "core/time/TimeModulator.h"
#include "input/InputTypes.h"
#include "renderer/RendererTypes.h"
#include "world/WorldTypes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <string_view>

#ifndef EN_ENABLE_DEBUG_TOOLS
    #if defined(NDEBUG)
        #define EN_ENABLE_DEBUG_TOOLS 0
    #else
        #define EN_ENABLE_DEBUG_TOOLS 1
    #endif
#endif

namespace en::world { class World; }

namespace en::debug {

inline constexpr u8  kDebugWorldPassId = 14;
inline constexpr u8  kDebugHudPassId   = 15;
inline constexpr u8  kDebugHudViewSlot = 15;
inline constexpr u16 kDebugFixedDepth  = 32768;

inline constexpr u32 kDebugAtlasWidth  = 128;
inline constexpr u32 kDebugAtlasHeight = 64;

// The solid white 8x8 block occupies pixel region [120..127, 56..63] in the 128x64 debug atlas.
// Sampling its center at (124.0 / 128.0, 60.0 / 64.0) with zero UV span lets untextured
// geometry and monospace text share a single TextureHandle and bypass radix sorting in O(1).
inline constexpr renderer::FRect kSolidWhiteUv{
    124.0f / static_cast<f32>(kDebugAtlasWidth),
    60.0f / static_cast<f32>(kDebugAtlasHeight),
    0.0f,
    0.0f
};

enum class DebugErrc : u8 {
    InvalidConfig = 0,
    OutOfMemory,
    AtlasCreateFailed,
    SymbolTableFull,
    DuplicateSymbol,
    SymbolNotFound,
    InvalidArguments,
    ValueOutOfRange
};

enum class OverlayFlags : u32 {
    None               = 0,
    SpatialTransforms  = 1u << 0,
    KinematicBounds    = 1u << 1,
    VelocityVectors    = 1u << 2,
    CombatHitboxes     = 1u << 3,
    TileCollisionMap   = 1u << 4,
    DynamicSpatialGrid = 1u << 5,
    SafeBreadcrumbs    = 1u << 6,
    CameraRigOverlay   = 1u << 7,
    AudioEmitters      = 1u << 8,
    EntityInspector    = 1u << 9,
    FlightScrubber     = 1u << 10,
    ProfilerHud        = 1u << 11,
    TimeChannelHud     = 1u << 12,
    InputViewerHud     = 1u << 13,
    AllWorld           = (1u << 9) - 1u,
    AllHud             = ((1u << 14) - 1u) & ~AllWorld
};

[[nodiscard]] constexpr OverlayFlags operator|(OverlayFlags a, OverlayFlags b) noexcept {
    return static_cast<OverlayFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr OverlayFlags operator&(OverlayFlags a, OverlayFlags b) noexcept {
    return static_cast<OverlayFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
[[nodiscard]] constexpr OverlayFlags operator^(OverlayFlags a, OverlayFlags b) noexcept {
    return static_cast<OverlayFlags>(static_cast<u32>(a) ^ static_cast<u32>(b));
}
[[nodiscard]] constexpr OverlayFlags operator~(OverlayFlags a) noexcept {
    return static_cast<OverlayFlags>(~static_cast<u32>(a));
}
constexpr OverlayFlags& operator|=(OverlayFlags& a, OverlayFlags b) noexcept { return a = a | b; }
constexpr OverlayFlags& operator&=(OverlayFlags& a, OverlayFlags b) noexcept { return a = a & b; }
constexpr OverlayFlags& operator^=(OverlayFlags& a, OverlayFlags b) noexcept { return a = a ^ b; }
[[nodiscard]] constexpr bool HasOverlay(OverlayFlags mask, OverlayFlags bit) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(bit)) != 0;
}

// Single-register 8-byte command packet returned by DebugSystem::BeginFrame() so the outer loop
// mutates TimePipeline without DebugSystem holding subsystem pointers or including template headers.
struct alignas(8) DebugLoopDirectives {
    static constexpr u8 kFlagTogglePause = 1u << 0;
    static constexpr u8 kFlagForcePause  = 1u << 1;

    f32 channelTimeScale{1.0f};
    u16 stepTicks{0};
    u8  targetTimeChannel{0xFFu};
    u8  controlFlags{0};

    [[nodiscard]] constexpr bool TogglePause() const noexcept {
        return (controlFlags & kFlagTogglePause) != 0;
    }
    [[nodiscard]] constexpr bool ForcePause() const noexcept {
        return (controlFlags & kFlagForcePause) != 0;
    }
};
static_assert(sizeof(DebugLoopDirectives) == 8);

// 16-byte spatial audio emitter view matching audio::AudioEmitterView without audio/ header coupling.
struct alignas(16) DebugAudioEmitter {
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 maxDist{0.0f};
    f32 occlusionFactor{1.0f};
};
static_assert(sizeof(DebugAudioEmitter) == 16);

// Unified read-only per-frame context for DebugSystem::BeginFrame() and DebugSystem::EmitOverlays().
// Eliminates cross-subsystem references to AudioSystem, AssetBank, Platform, and GameplayPipeline.
struct DebugFrameContext {
    const world::World& world;
    time::RenderTimeView timeView{};
    const input::FrameInput& frameInput;
    renderer::ViewportExtent viewportSize{};
    std::span<const DebugAudioEmitter> audioEmitters{};
    bool isPaused{false};
};

using DebugBeginFrameContext  = DebugFrameContext;
using DebugEmitOverlayContext = DebugFrameContext;

// Self-contained POD telemetry descriptor passed once per frame to DebugProfiler::RecordSubsystemTelemetry().
struct SubsystemTelemetryDesc {
    std::span<const f32> channelScales{};
    f32 audioCpuLoadRatio{0.0f};
    f32 audioPeakL{0.0f};
    f32 audioPeakR{0.0f};
    u16 aliveEntities{0};
    u16 submittedQuads{0};
    u16 culledQuads{0};
    u16 drawBatches{0};
    u8  catchupTicks{0};
    u8  radixPasses{0};
    u8  frozenChannelMask{0};
    u8  activePhysVoices{0};
    u8  activeVirtVoices{0};
    u8  audioUnderruns{0};
};

enum class DebugShapeKind : u8 {
    Line = 0,
    BoxOutline,
    BoxFilled,
    CircleOutline,
    Arrow,
    TextWorld,
    TextScreen
};

// Self-contained 32-byte transient shape command (2 commands per 64B cache line).
// Inline text payload avoids external string buffer fragmentation when shapes expire out of order.
struct alignas(32) DebugShapeCmd {
    f32 x0{0.0f};
    f32 y0{0.0f};
    u32 rgba8{0xFFFFFFFFu};
    u32 expireTick{0};
    DebugShapeKind kind{DebugShapeKind::Line};
    u8  auxParam{0};
    u16 thicknessQ8{256};
    union {
        struct {
            f32 x1;
            f32 y1;
            f32 headSize;
        } geom{0.0f, 0.0f, 0.0f};
        std::array<char, 12> inlineText;
    };
};
static_assert(sizeof(DebugShapeCmd) == 32);

enum class ProfileScopeId : u8 {
    FrameTotal = 0,
    PlatformPump,
    AssetsPump,
    SimTickTotal,
    GameplaySpatial,
    GameplayKinematics,
    GameplayCombatFsm,
    WorldTransforms,
    AudioUpdate,
    WorldExtract,
    DebugOverlayEmit,
    RenderSubmit,
    Count
};
inline constexpr usize kProfileScopeCount = static_cast<usize>(ProfileScopeId::Count);

// Exact 64-byte (1 cache line, zero padding) per-frame telemetry snapshot.
struct alignas(64) FrameProfileSample {
    u32 frameIndex{0};
    u32 simTickIndex{0};
    std::array<u16, kProfileScopeCount> scopeUs{};
    u16 aliveEntities{0};
    u16 submittedQuads{0};
    u16 culledQuads{0};
    u16 drawBatches{0};
    std::array<u16, 8> channelScaleQ8{};
    u16 audioCpuPermille{0};
    u8  catchupTicks{0};
    u8  activePhysVoices{0};
    u8  activeVirtVoices{0};
    u8  frozenChannelMask{0};
    u8  audioUnderruns{0};
    u8  radixPassesExecuted{0};
};
static_assert(sizeof(FrameProfileSample) == 64);

// Exact 64-byte (1 cache line, zero padding) per-tick flight recorder snapshot for hitbox/input scrubbing.
struct alignas(64) ActorTickRecord {
    u32 tickIndex{0};
    world::EntityId entityId{world::kNullEntity};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    f32 velX{0.0f};
    f32 velY{0.0f};
    u16 inputHeld{0};
    u16 inputPressed{0};
    u16 inputReleased{0};
    u16 actionBufferMask{0};
    i16 axisX{0};
    i16 axisY{0};
    u16 cancelMask{0};
    u16 stateTicks{0};
    i16 hp{0};
    std::array<i16, 4> hurtboxQ8{};
    std::array<i16, 4> hitboxQ8{};
    u8  stateId{0};
    u8  animClipId{0};
    u8  animFrame{0};
    u8  contactFlags{0};
    u8  activeSliceIdx{0};
    u8  iframeTicks{0};
};
static_assert(sizeof(ActorTickRecord) == 64);

[[nodiscard]] constexpr u32 HashSymbolName(std::string_view str) noexcept {
    u32 hash = 2166136261u;
    for (const char c : str) {
        hash ^= static_cast<u32>(static_cast<u8>(c));
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
}

[[nodiscard]] constexpr u64 MixAuditHash(u64 seed, u64 val) noexcept {
    u64 z = seed ^ (val + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// 96 printable ASCII 8x8 1-bit glyphs (0x20 Space .. 0x7F Del), MSB = left pixel.
inline constexpr std::array<u64, 96> kEmbeddedFont8x8 = {
    0x0000000000000000ull, 0x183C3C1818001800ull, 0x6666240000000000ull, 0x6C6CFE6C6CFE6C00ull,
    0x183E603C067C1800ull, 0x00C6CC183066C600ull, 0x386C3876DCCE7B00ull, 0x1818300000000000ull,
    0x0C18303030180C00ull, 0x30180C0C0C183000ull, 0x00663CFF3C660000ull, 0x0018187E18180000ull,
    0x0000000000181830ull, 0x0000007E00000000ull, 0x0000000000181800ull, 0x060C183060C08000ull,
    0x7CC6CEDEF6E67C00ull, 0x1838181818187E00ull, 0x7CC6063C60C6FE00ull, 0x7CC6063C06C67C00ull,
    0x1C3C6CCCFE0C1E00ull, 0xFEC0FC0606C67C00ull, 0x3860C0FCC6C67C00ull, 0xFEC60C1830303000ull,
    0x7CC6C67CC6C67C00ull, 0x7CC6C67E060C7800ull, 0x0018180000181800ull, 0x0018180000181830ull,
    0x0C18306030180C00ull, 0x00007E00007E0000ull, 0x30180C060C183000ull, 0x7CC60C1818001800ull,
    0x7CC6DEDEDEC07800ull, 0x386CC6FEC6C6C600ull, 0xFC66667C6666FC00ull, 0x3C66C0C0C0663C00ull,
    0xF86C6666666CF800ull, 0xFE6268786862FE00ull, 0xFE6268786860F000ull, 0x3C66C0C0CE663E00ull,
    0xC6C6C6FEC6C6C600ull, 0x3C18181818183C00ull, 0x1E0C0C0CCCCC7800ull, 0xE6666C786C66E600ull,
    0xF06060606266FE00ull, 0xC6EEFEFED6C6C600ull, 0xC6E6F6DECEC6C600ull, 0x7CC6C6C6C6C67C00ull,
    0xFC66667C6060F000ull, 0x7CC6C6C6D6DE7C06ull, 0xFC66667C6C66E600ull, 0x7CC660380EC67C00ull,
    0x7E7E5A1818183C00ull, 0xC6C6C6C6C6C67C00ull, 0xC6C6C6C6C66C3800ull, 0xC6C6D6FEFE6C4400ull,
    0xC6C66C386CC6C600ull, 0x6666663C18183C00ull, 0xFECE983062C6FE00ull, 0x3C30303030303C00ull,
    0xC06030180C060200ull, 0x3C0C0C0C0C0C3C00ull, 0x10386CC600000000ull, 0x00000000000000FFull,
    0x30180C0000000000ull, 0x0000780C7CCCCC76ull, 0xE0607C666666DC00ull, 0x00007CC6C0C67C00ull,
    0x1C0C7CCC66667600ull, 0x00007CC6FEC07C00ull, 0x3C6660F86060F000ull, 0x000076CCCC7C0CF8ull,
    0xE0606C766666E600ull, 0x1800381818183C00ull, 0x060006060666663Cull, 0xE060666C786CE600ull,
    0x3818181818183C00ull, 0x0000ECFEFED6C600ull, 0x0000DC6666666600ull, 0x00007CC6C6C67C00ull,
    0x0000DC66667C60F0ull, 0x000076CCCC7C0C1Eull, 0x0000DC766060F000ull, 0x00007CC07C06FC00ull,
    0x3030FC3030361C00ull, 0x0000CCCCCCCC7600ull, 0x0000C6C6C66C3800ull, 0x0000C6D6FEFE6C00ull,
    0x0000C66C386CC600ull, 0x0000C6C6CE76067Cull, 0x0000FE9C3064FE00ull, 0x0E18187018180E00ull,
    0x1818180018181800ull, 0x7018180E18187000ull, 0x76DC000000000000ull, 0xFFAA55AA55AA55AAull
};

constexpr void UnpackDebugAtlasRGBA8(std::span<u8, kDebugAtlasWidth * kDebugAtlasHeight * 4> outPixels) noexcept {
    std::fill(outPixels.begin(), outPixels.end(), u8{0});

    auto setWhitePixel = [&](usize pixelIdx) noexcept {
        const usize byteIdx = pixelIdx * 4u;
        outPixels[byteIdx + 0] = 255;
        outPixels[byteIdx + 1] = 255;
        outPixels[byteIdx + 2] = 255;
        outPixels[byteIdx + 3] = 255;
    };

    for (u32 glyphIdx = 0; glyphIdx < 96; ++glyphIdx) {
        const u32 baseX = (glyphIdx & 15u) * 8u;
        const u32 baseY = (glyphIdx >> 4u) * 8u;
        const u64 bits  = kEmbeddedFont8x8[glyphIdx];

        for (u32 py = 0; py < 8; ++py) {
            u32 rowBits = static_cast<u32>((bits >> ((7u - py) * 8u)) & 0xFFu);
            while (rowBits != 0u) {
                const u32 bitPos = static_cast<u32>(std::countr_zero(rowBits));
                rowBits &= (rowBits - 1u);
                const u32 px = 7u - bitPos;
                setWhitePixel((baseY + py) * kDebugAtlasWidth + (baseX + px));
            }
        }
    }

    for (u32 py = 56; py < 64; ++py) {
        for (u32 px = 120; px < 128; ++px) {
            setWhitePixel(py * kDebugAtlasWidth + px);
        }
    }
}

} // namespace en::debug

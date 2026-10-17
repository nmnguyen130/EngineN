#pragma once

#include "core/Result.h"
#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "world/WorldTypes.h"

#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string_view>

#ifndef EN_ENABLE_DEBUG_TOOLS
    #if defined(NDEBUG)
        #define EN_ENABLE_DEBUG_TOOLS 0
    #else
        #define EN_ENABLE_DEBUG_TOOLS 1
    #endif
#endif

namespace en::debug {

inline constexpr u8  kDebugWorldPassId = 14;
inline constexpr u8  kDebugHudPassId   = 15;
inline constexpr u8  kDebugHudViewSlot = 15;
inline constexpr u16 kDebugFixedDepth  = 32768;

inline constexpr u32 kDebugAtlasWidth  = 128;
inline constexpr u32 kDebugAtlasHeight = 64;

// The solid white 8x8 block occupies pixel region [120..127, 56..63] in the 128x64 debug atlas.
// Sampling its center at (124.0 / 128.0, 60.0 / 64.0) with zero UV span enables untextured
// geometry and monospace text to share a single TextureHandle and skip radix sort passes in O(1).
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
    AllWorld           = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8),
    AllHud             = (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13)
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
[[nodiscard]] constexpr bool HasOverlay(OverlayFlags mask, OverlayFlags bit) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(bit)) != 0;
}

// Decoupled command packet returned by DebugSystem::BeginFrame() so the outer loop mutates TimePipeline
// without DebugSystem holding raw subsystem pointers or including heavy template headers.
struct alignas(8) DebugLoopDirectives {
    u16 stepTicks{0};
    f32 channelTimeScale{1.0f};
    u8  targetTimeChannel{0xFFu};
    bool togglePause{false};
    bool forcePause{false};
};
static_assert(sizeof(DebugLoopDirectives) == 12);

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

// Compact 128-byte (2 cache lines) per-frame telemetry snapshot.
struct alignas(64) FrameProfileSample {
    u64 frameIndex{0};
    u64 simTickIndex{0};
    std::array<u16, kProfileScopeCount> scopeUs{};
    u16 aliveEntities{0};
    u16 submittedQuads{0};
    u16 culledQuads{0};
    u16 drawBatches{0};
    std::array<u16, 8> channelScaleQ8{};
    u16 audioCpuPermille{0};
    u16 audioPeakL_Q10{0};
    u16 audioPeakR_Q10{0};
    u8  catchupTicks{0};
    u8  activePhysVoices{0};
    u8  activeVirtVoices{0};
    u8  frozenChannelMask{0};
    u8  audioUnderruns{0};
    u8  radixPassesExecuted{0};
    std::array<u8, 10> reserved{};
};
static_assert(sizeof(FrameProfileSample) == 128);

// 64-byte (1 cache line) per-tick flight recorder snapshot for frame-by-frame hitbox/input scrubbing.
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

// Computes 32-bit FNV-1a hash at compile time or runtime for ConsoleSymbol lookup.
[[nodiscard]] constexpr u32 HashSymbolName(std::string_view str) noexcept {
    u32 hash = 2166136261u;
    for (const char c : str) {
        hash ^= static_cast<u32>(static_cast<u8>(c));
        hash *= 16777619u;
    }
    return hash == 0 ? 1u : hash;
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

// Unpacks the 96 1-bit ASCII glyphs + bottom-right 8x8 solid white block into a 128x64 RGBA8 buffer.
constexpr void UnpackDebugAtlasRGBA8(std::span<u8, kDebugAtlasWidth * kDebugAtlasHeight * 4> outPixels) noexcept {
    for (u8& b : outPixels) {
        b = 0;
    }

    for (u32 glyphIdx = 0; glyphIdx < 96; ++glyphIdx) {
        const u32 col   = glyphIdx & 15u;
        const u32 row   = glyphIdx >> 4u;
        const u32 baseX = col * 8u;
        const u32 baseY = row * 8u;
        const u64 bits  = kEmbeddedFont8x8[glyphIdx];

        for (u32 py = 0; py < 8; ++py) {
            const u8 rowBits = static_cast<u8>((bits >> ((7u - py) * 8u)) & 0xFFu);
            for (u32 px = 0; px < 8; ++px) {
                if ((rowBits & (0x80u >> px)) != 0) {
                    const usize idx = ((baseY + py) * kDebugAtlasWidth + (baseX + px)) * 4u;
                    outPixels[idx + 0] = 255;
                    outPixels[idx + 1] = 255;
                    outPixels[idx + 2] = 255;
                    outPixels[idx + 3] = 255;
                }
            }
        }
    }

    for (u32 py = 56; py < 64; ++py) {
        for (u32 px = 120; px < 128; ++px) {
            const usize idx = (py * kDebugAtlasWidth + px) * 4u;
            outPixels[idx + 0] = 255;
            outPixels[idx + 1] = 255;
            outPixels[idx + 2] = 255;
            outPixels[idx + 3] = 255;
        }
    }
}

} // namespace en::debug

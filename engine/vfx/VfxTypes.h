#pragma once

#include "world/Components.h"

namespace en {

enum class VfxErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
};

// 32-byte 2D particle state stepped in parallel batches via JobSystem::ParallelFor and compacted via Swap-Back.
// Value type stored contiguously in VfxSystem's Arena; zero entity slot overhead.
// Invariant: sizeof(Particle2D) == 32.
struct Particle2D {
    Vec2  pos{0.0f, 0.0f};
    Vec2  vel{0.0f, 0.0f};
    Rgba8 color{255, 255, 255, 255};
    f32   size{0.12f};
    f32   gravity{-12.0f};
    u8    lifeTicks{0};
    u8    maxLifeTicks{20};
    u8    dampingQ6{8};
    u8    layer{12U};
};
static_assert(sizeof(Particle2D) == 32);

// 32-byte after-image ghost snapshot emitted during character dashes and compacted via Swap-Back.
// Value type stored contiguously in VfxSystem's Arena; fades out over remainingTicks and extracts to RenderList.
// Invariant: sizeof(DashGhost2D) == 32.
struct DashGhost2D {
    Vec2         pos{0.0f, 0.0f};
    Vec2         size{1.0f, 1.0f};
    Rgba8        tint{120, 220, 255, 180};
    SpriteHandle sprite{0U, 1U};
    u16          depth{32760U};
    u8           remainingTicks{0};
    u8           initialTicks{8};
    u8           layer{9U};
    u8           flags{0U}; // bit 0: flipX, bit 1: flipY
};
static_assert(sizeof(DashGhost2D) == 32);

struct ParticleBurstDesc {
    Vec2      origin{0.0f, 0.0f};
    Vec2      baseVel{0.0f, 2.0f};
    Vec2      velSpread{2.5f, 2.0f};
    Rgba8     color{255, 240, 200, 230};
    f32       size{0.10f};
    f32       gravity{-10.0f};
    u8        lifeTicks{16};
    u8        count{8};
    u8        layer{12U};
    BlendMode blend{BlendMode::Additive};
};
static_assert(sizeof(ParticleBurstDesc) == 40);

struct VfxConfig {
    u16 maxParticles{1024};
    u16 maxGhosts{128};
    u16 parallelBatchSize{64};
    u16 pad{0};
    u32 arenaBytes{64U * 1024U};
};
static_assert(sizeof(VfxConfig) == 12);

} // namespace en

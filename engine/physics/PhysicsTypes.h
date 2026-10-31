#pragma once

#include "world/Components.h"

namespace en {

inline constexpr u32 kTileMapMagic = 0x50414D54U;

enum class PhysicsErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    CorruptedBlob,
    PoolRegistrationFailed,
};

enum class BodyType : u8 {
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

enum class TileFlag : u8 {
    Empty         = 0,
    Solid         = 1U << 0,
    OneWay        = 1U << 1,
    Hazard        = 1U << 2,
    Climbable     = 1U << 3,
    Slope45Up     = 1U << 4,
    Slope45Down   = 1U << 5,
    SlopeLowHalf  = 1U << 6,
    SlopeHighHalf = 1U << 7,
    Slope22UpLo   = Slope45Up | SlopeLowHalf,
    Slope22UpHi   = Slope45Up | SlopeHighHalf,
    Slope22DownLo = Slope45Down | SlopeLowHalf,
    Slope22DownHi = Slope45Down | SlopeHighHalf,
};
consteval bool enable_bitmask_operators(TileFlag) noexcept { return true; }

[[nodiscard]] constexpr bool IsSlopeTile(TileFlag f) noexcept {
    return HasFlag(f, TileFlag::Slope45Up | TileFlag::Slope45Down);
}

[[nodiscard]] constexpr f32 SampleSlopeHeight(TileFlag f, f32 localU, f32 tileSize) noexcept {
    const f32 u    = std::clamp(localU, 0.0f, 1.0f);
    const bool up  = HasFlag(f, TileFlag::Slope45Up);
    const f32 ramp = up ? u : (1.0f - u);
    if (HasFlag(f, TileFlag::SlopeLowHalf))  return 0.5f * ramp * tileSize;
    if (HasFlag(f, TileFlag::SlopeHighHalf)) return (0.5f + 0.5f * ramp) * tileSize;
    return ramp * tileSize;
}

[[nodiscard]] constexpr Vec2 SlopeNormal(TileFlag f) noexcept {
    const f32 sign = HasFlag(f, TileFlag::Slope45Up) ? -1.0f : 1.0f;
    if (HasFlag(f, TileFlag::SlopeLowHalf | TileFlag::SlopeHighHalf)) {
        return {sign * 0.44721360f, 0.89442719f};
    }
    return {sign * 0.70710678f, 0.70710678f};
}

[[nodiscard]] constexpr Vec2 ProjectVelocityOnSlope(Vec2 velocity, Vec2 slopeNormal) noexcept {
    const Vec2 tangent{slopeNormal.y, -slopeNormal.x};
    return tangent * velocity.x;
}

enum class ContactFlags : u8 {
    None      = 0,
    Ground    = 1U << 0,
    Ceiling   = 1U << 1,
    WallLeft  = 1U << 2,
    WallRight = 1U << 3,
    OneWay    = 1U << 4,
    Slope     = 1U << 5,
    Crushed   = 1U << 6,
};
consteval bool enable_bitmask_operators(ContactFlags) noexcept { return true; }

enum class ContactEventKind : u8 {
    BeginContact = 0,
    EndContact   = 1,
    TriggerEnter = 2,
    TriggerExit  = 3,
};

// 32-byte 2D kinematic/dynamic body state paired with Collider2D for Metroidvania simulation.
// Value type; contacts and carrier are updated every PhysicsSystem::Step.
// Invariant: sizeof(PhysicsBody2D) == 32.
struct PhysicsBody2D {
    static constexpr u8 kTypeId = 9;

    Vec2         velocity{0.0f, 0.0f};
    f32          gravityScale{1.0f};
    f32          linearDamping{0.0f};
    f32          restitution{0.0f};
    f32          friction{0.0f};
    EntityId     carrier{kNullEntity};
    BodyType     type{BodyType::Dynamic};
    ContactFlags contacts{ContactFlags::None};
    u8           ignoreOneWay{0};
    u8           enabled{1};

    [[nodiscard]] constexpr bool IsStatic() const noexcept { return type == BodyType::Static; }
    [[nodiscard]] constexpr bool IsKinematic() const noexcept { return type == BodyType::Kinematic; }
    [[nodiscard]] constexpr bool IsDynamic() const noexcept { return type == BodyType::Dynamic; }
    [[nodiscard]] constexpr bool IsGrounded() const noexcept { return HasFlag(contacts, ContactFlags::Ground); }
    [[nodiscard]] constexpr bool OnSlope() const noexcept { return HasFlag(contacts, ContactFlags::Slope); }
    [[nodiscard]] constexpr bool OnCeiling() const noexcept { return HasFlag(contacts, ContactFlags::Ceiling); }
    [[nodiscard]] constexpr bool OnWallLeft() const noexcept { return HasFlag(contacts, ContactFlags::WallLeft); }
    [[nodiscard]] constexpr bool OnWallRight() const noexcept { return HasFlag(contacts, ContactFlags::WallRight); }
    [[nodiscard]] constexpr bool TouchingWall() const noexcept {
        return HasFlag(contacts, ContactFlags::WallLeft | ContactFlags::WallRight);
    }
    [[nodiscard]] constexpr bool OnOneWay() const noexcept { return HasFlag(contacts, ContactFlags::OneWay); }
    [[nodiscard]] constexpr bool IsCrushed() const noexcept { return HasFlag(contacts, ContactFlags::Crushed); }

    constexpr void ApplyImpulse(Vec2 impulse) noexcept {
        if (IsDynamic() && enabled != 0U) velocity += impulse;
    }
};
static_assert(sizeof(PhysicsBody2D) == 32);

// 32-byte 2D raycast result against tilemap geometry and entity colliders.
// Value type; fraction in [0, 1] and distance in meters when hit != 0.
struct RayHit2D {
    Vec2     point{};
    Vec2     normal{};
    f32      distance{0.0f};
    f32      fraction{1.0f};
    EntityId entity{kNullEntity};
    TileFlag tile{TileFlag::Empty};
    u8       hit{0};
    u8       startSolid{0};
    u8       pad{0};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hit != 0U; }
};
static_assert(sizeof(RayHit2D) == 32);

// 32-byte swept AABB collision result against tilemap geometry and entity colliders.
// Value type; time in [0, 1] indicates normalized impact fraction along delta.
struct SweepResult2D {
    Vec2     point{};
    Vec2     normal{};
    f32      time{1.0f};
    f32      penetration{0.0f};
    EntityId entity{kNullEntity};
    TileFlag tile{TileFlag::Empty};
    u8       hit{0};
    u8       startSolid{0};
    u8       oneWay{0};

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hit != 0U; }
};
static_assert(sizeof(SweepResult2D) == 32);

// 32-byte solid contact or trigger overlap transition event emitted during PhysicsSystem::Step.
// Value type; entityB is kNullEntity when entityA collides with static tilemap geometry.
struct ContactEvent2D {
    EntityId         entityA{kNullEntity};
    EntityId         entityB{kNullEntity};
    Vec2             point{};
    Vec2             normal{};
    f32              approachSpeed{0.0f};
    ContactEventKind kind{ContactEventKind::BeginContact};
    u8               isTrigger{0};
    u16              userTag{0};
};
static_assert(sizeof(ContactEvent2D) == 32);

// 24-byte binary header for serialized tilemap collision blobs.
struct TileMapHeader {
    u32  magic{kTileMapMagic};
    u16  version{1};
    u16  width{0};
    u16  height{0};
    u16  pad{0};
    f32  tileSize{1.0f};
    Vec2 origin{0.0f, 0.0f};
};
static_assert(sizeof(TileMapHeader) == 24);

// 40-byte data-driven configuration for PhysicsSystem initialization and solver tolerances.
// Value type; controls step-up, ledge-hop, ceiling corner correction, and downhill slope adhesion.
// Invariant: sizeof(PhysicsConfig) == 40.
struct PhysicsConfig {
    Vec2 gravity{0.0f, -32.0f};
    f32  cellSize{2.0f};
    f32  maxStepHeight{0.20f};
    f32  groundSnapDist{0.12f};
    f32  cornerCorrectionDist{0.18f};
    f32  slopeSnapMultiplier{1.25f};
    f32  ledgeHopMaxVy{8.0f};
    u16  maxProxies{4096};
    u16  maxContactPairs{1024};
    u32  arenaBytes{1024U * 1024U};
};
static_assert(sizeof(PhysicsConfig) == 40);

[[nodiscard]] constexpr RayHit2D RaycastBox(
    Vec2 origin, Vec2 dir, f32 maxDist, const Rect& box) noexcept {
    if (box.IsEmpty() || maxDist < 0.0f) return {};
    if (box.Contains(origin)) {
        return {
            .point      = origin,
            .normal     = -dir,
            .distance   = 0.0f,
            .fraction   = 0.0f,
            .hit        = 1U,
            .startSolid = 1U,
        };
    }

    f32 tMin  = 0.0f;
    f32 tMax  = maxDist;
    Vec2 norm = {};

    if (std::abs(dir.x) <= 1e-8f) {
        if (origin.x < box.x || origin.x > box.Right()) return {};
    } else {
        const f32 invX = 1.0f / dir.x;
        f32 t1 = (box.x - origin.x) * invX;
        f32 t2 = (box.Right() - origin.x) * invX;
        f32 sign = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0f;
        }
        if (t1 > tMin) {
            tMin = t1;
            norm = {sign, 0.0f};
        }
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return {};
    }

    if (std::abs(dir.y) <= 1e-8f) {
        if (origin.y < box.y || origin.y > box.Bottom()) return {};
    } else {
        const f32 invY = 1.0f / dir.y;
        f32 t1 = (box.y - origin.y) * invY;
        f32 t2 = (box.Bottom() - origin.y) * invY;
        f32 sign = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0f;
        }
        if (t1 > tMin) {
            tMin = t1;
            norm = {0.0f, sign};
        }
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return {};
    }

    if (tMin < 0.0f || tMin > maxDist) return {};
    return {
        .point    = origin + dir * tMin,
        .normal   = norm,
        .distance = tMin,
        .fraction = (maxDist > 1e-8f) ? (tMin / maxDist) : 0.0f,
        .hit      = 1U,
    };
}

[[nodiscard]] constexpr SweepResult2D SweepBoxVsBox(
    const Rect& moving, Vec2 delta, const Rect& target) noexcept {
    if (moving.IsEmpty() || target.IsEmpty()) return {};

    constexpr f32 kSkin = 2e-4f;
    const Vec2 center   = moving.Center();
    const Vec2 half     = moving.HalfExtents();
    const f32  eMinX    = target.x - half.x;
    const f32  eMinY    = target.y - half.y;
    const f32  eMaxX    = target.Right() + half.x;
    const f32  eMaxY    = target.Bottom() + half.y;

    if (center.x > eMinX + kSkin && center.x < eMaxX - kSkin &&
        center.y > eMinY + kSkin && center.y < eMaxY - kSkin) {
        const f32 dl = center.x - eMinX;
        const f32 dr = eMaxX - center.x;
        const f32 dd = center.y - eMinY;
        const f32 du = eMaxY - center.y;
        const f32 px = std::min(dl, dr);
        const f32 py = std::min(dd, du);
        if (px < py) {
            return {
                .point       = center,
                .normal      = {(dl < dr) ? -1.0f : 1.0f, 0.0f},
                .time        = 0.0f,
                .penetration = px,
                .hit         = 1U,
                .startSolid  = 1U,
            };
        }
        return {
            .point       = center,
            .normal      = {0.0f, (dd < du) ? -1.0f : 1.0f},
            .time        = 0.0f,
            .penetration = py,
            .hit         = 1U,
            .startSolid  = 1U,
        };
    }

    f32 tNearX = -1e30f, tFarX = 1e30f;
    if (std::abs(delta.x) <= 1e-8f) {
        if (center.x <= eMinX || center.x >= eMaxX) return {};
    } else {
        const f32 invX = 1.0f / delta.x;
        tNearX = (eMinX - center.x) * invX;
        tFarX  = (eMaxX - center.x) * invX;
        if (tNearX > tFarX) std::swap(tNearX, tFarX);
    }

    f32 tNearY = -1e30f, tFarY = 1e30f;
    if (std::abs(delta.y) <= 1e-8f) {
        if (center.y <= eMinY || center.y >= eMaxY) return {};
    } else {
        const f32 invY = 1.0f / delta.y;
        tNearY = (eMinY - center.y) * invY;
        tFarY  = (eMaxY - center.y) * invY;
        if (tNearY > tFarY) std::swap(tNearY, tFarY);
    }

    const f32 tEnter = std::max(tNearX, tNearY);
    const f32 tExit  = std::min(tFarX, tFarY);
    const f32 move   = std::abs(delta.x) + std::abs(delta.y);
    const f32 minT   = (move > 1e-6f) ? (-2.0f * kSkin / move) : -1e-3f;
    if (tEnter > tExit || tEnter < minT || tEnter >= 1.0f || tExit <= 0.0f) return {};

    Vec2 n{};
    if (tNearX > tNearY) {
        n.x = (delta.x < 0.0f) ? 1.0f : -1.0f;
    } else {
        n.y = (delta.y < 0.0f) ? 1.0f : -1.0f;
    }
    const f32 clampedT = std::clamp(tEnter, 0.0f, 1.0f);
    return {
        .point  = center + delta * clampedT,
        .normal = n,
        .time   = clampedT,
        .hit    = 1U,
    };
}

} // namespace en

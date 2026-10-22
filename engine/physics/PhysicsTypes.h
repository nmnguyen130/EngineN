#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "time/TimeTypes.h"
#include "world/WorldTypes.h"

#include <type_traits>

namespace en {

template <BitmaskEnum E>
[[nodiscard]] constexpr bool HasPhysicsFlag(E flags, E test) noexcept {
    return HasFlag(flags, test);
}

enum class PhysicsErrc : u8 {
    None = 0,
    InvalidConfig,
    OutOfMemory,
    CorruptedCollisionBlob,
    ArenaExhausted,
    PoolRegistrationFailed,
    ManifoldCapacityReached,
};

enum class BodyType2D : u8 {
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
};

enum class ShapeKind2D : u8 {
    Aabb = 0,
    Circle = 1,
    CapsuleY = 2,
    Obb = 3,
    Segment = 4,
};

enum class ColliderFlags2D : u8 {
    None     = 0,
    IsSensor = 1U << 0U,
    IsOneWay = 1U << 1U,
};
consteval bool enable_bitmask_operators(ColliderFlags2D) noexcept { return true; }

enum class ContactFlags : u8 {
    None      = 0,
    Ground    = 1U << 0U,
    Ceiling   = 1U << 1U,
    WallLeft  = 1U << 2U,
    WallRight = 1U << 3U,
    OnSlope   = 1U << 4U,
    OnOneWay  = 1U << 5U,
    Submerged = 1U << 6U,
    Crushed   = 1U << 7U,
};
consteval bool enable_bitmask_operators(ContactFlags) noexcept { return true; }

enum class BodyFlags2D : u8 {
    None          = 0,
    BulletCCD     = 1U << 0U,
    FixedRotation = 1U << 1U,
    Sleeping      = 1U << 2U,
    CanSleep      = 1U << 3U,
    IgnoreOneWay  = 1U << 4U,
    UseGravity    = 1U << 5U,
    Enabled       = 1U << 6U,
};
consteval bool enable_bitmask_operators(BodyFlags2D) noexcept { return true; }

enum class TileMaterialFlags : u8 {
    Empty  = 0,
    Solid  = 1U << 0U,
    OneWay = 1U << 1U,
};
consteval bool enable_bitmask_operators(TileMaterialFlags) noexcept { return true; }

enum class ContactEventKind : u8 {
    BeginTouch  = 0,
    EndTouch    = 1,
    HitImpact   = 2,
    SensorEnter = 3,
    SensorExit  = 4,
};

// 8-byte quantized Q8.8 local geometry descriptor in meters (1/256 m resolution, +/-127.99 m range).
// For Aabb/Circle/CapsuleY/Obb: (offsetX, offsetY, halfW, halfH). For Segment: endpoints (x0, y0) -> (x1, y1).
struct alignas(4) CompactAabb2D {
    i16 offsetX_q8{0};
    i16 offsetY_q8{0};
    u16 halfW_q8{128};
    u16 halfH_q8{128};

    [[nodiscard]] static constexpr i16 QuantizeS16(f32 meters) noexcept {
        const f32 scaled = meters * 256.0f;
        const i32 rounded = static_cast<i32>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
        return static_cast<i16>(std::clamp(rounded, -32768, 32767));
    }

    [[nodiscard]] static constexpr u16 QuantizeU16(f32 meters) noexcept {
        const f32 scaled = std::max(0.0f, meters) * 256.0f;
        const i32 rounded = static_cast<i32>(scaled + 0.5f);
        return static_cast<u16>(std::clamp(rounded, 0, 65535));
    }

    [[nodiscard]] static constexpr CompactAabb2D FromMeters(
        f32 offsetX,
        f32 offsetY,
        f32 halfW,
        f32 halfH) noexcept {
        return {QuantizeS16(offsetX), QuantizeS16(offsetY), QuantizeU16(halfW), QuantizeU16(halfH)};
    }

    [[nodiscard]] static constexpr CompactAabb2D FromSegment(
        f32 x0,
        f32 y0,
        f32 x1,
        f32 y1) noexcept {
        return {
            QuantizeS16(x0),
            QuantizeS16(y0),
            static_cast<u16>(QuantizeS16(x1)),
            static_cast<u16>(QuantizeS16(y1))};
    }

    [[nodiscard]] constexpr f32 OffsetX() const noexcept { return static_cast<f32>(offsetX_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 OffsetY() const noexcept { return static_cast<f32>(offsetY_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfW() const noexcept { return static_cast<f32>(halfW_q8) * (1.0f / 256.0f); }
    [[nodiscard]] constexpr f32 HalfH() const noexcept { return static_cast<f32>(halfH_q8) * (1.0f / 256.0f); }

    [[nodiscard]] constexpr Vec2 SegmentP0() const noexcept { return {OffsetX(), OffsetY()}; }
    [[nodiscard]] constexpr Vec2 SegmentP1() const noexcept {
        return {
            static_cast<f32>(static_cast<i16>(halfW_q8)) * (1.0f / 256.0f),
            static_cast<f32>(static_cast<i16>(halfH_q8)) * (1.0f / 256.0f)};
    }
};
static_assert(sizeof(CompactAabb2D) == 8);

// 8-byte collision category, mask, and Box2D-style signed group index filter.
struct alignas(4) CollisionFilter2D {
    u16 categoryBits{0x0001U};
    u16 maskBits{0xFFFFU};
    i16 groupIndex{0};
    u16 materialTag{0};

    [[nodiscard]] constexpr bool ShouldCollide(const CollisionFilter2D& other) const noexcept {
        if (groupIndex != 0 && groupIndex == other.groupIndex) {
            return groupIndex > 0;
        }
        return ((categoryBits & other.maskBits) != 0U) &&
               ((other.categoryBits & maskBits) != 0U);
    }
};
static_assert(sizeof(CollisionFilter2D) == 8);

// 24-byte (zero padding) geometric collider component attached to entities in World.
struct alignas(8) Collider2D {
    static constexpr u8 kTypeId = 8;

    CompactAabb2D localBounds{};
    CollisionFilter2D filter{};
    u16 friction_q16{19661U};    // ~0.30f in Q0.16
    u16 restitution_q16{0U};     // 0.0f in Q0.16
    u16 density_q8{256U};        // 1.0f in Q8.8
    ShapeKind2D shape{ShapeKind2D::Aabb};
    ColliderFlags2D flags{ColliderFlags2D::None};

    [[nodiscard]] static constexpr u16 QuantizeUnit(f32 v) noexcept {
        const f32 c = std::clamp(v, 0.0f, 1.0f);
        return static_cast<u16>(c * 65535.0f + 0.5f);
    }

    [[nodiscard]] constexpr f32 Friction() const noexcept {
        return static_cast<f32>(friction_q16) * (1.0f / 65535.0f);
    }
    [[nodiscard]] constexpr f32 Restitution() const noexcept {
        return static_cast<f32>(restitution_q16) * (1.0f / 65535.0f);
    }
    [[nodiscard]] constexpr f32 Density() const noexcept {
        return static_cast<f32>(density_q8) * (1.0f / 256.0f);
    }
    [[nodiscard]] constexpr bool IsSensor() const noexcept {
        return HasPhysicsFlag(flags, ColliderFlags2D::IsSensor);
    }
    [[nodiscard]] constexpr bool IsOneWay() const noexcept {
        return HasPhysicsFlag(flags, ColliderFlags2D::IsOneWay);
    }

    constexpr void SetFriction(f32 f) noexcept { friction_q16 = QuantizeUnit(f); }
    constexpr void SetRestitution(f32 r) noexcept { restitution_q16 = QuantizeUnit(r); }
    constexpr void SetSensor(bool sensor) noexcept {
        if (sensor) flags |= ColliderFlags2D::IsSensor;
        else        flags &= ~ColliderFlags2D::IsSensor;
    }
    constexpr void SetOneWay(bool oneWay) noexcept {
        if (oneWay) flags |= ColliderFlags2D::IsOneWay;
        else        flags &= ~ColliderFlags2D::IsOneWay;
    }
};
static_assert(sizeof(Collider2D) == 24);

// 32-byte (2 per 64B cache line, zero padding) rigid-body state component for Soft-Step TGS rotational & linear dynamics.
struct alignas(32) RigidBody2D {
    static constexpr u8 kTypeId = 9;

    f32 vx{0.0f};
    f32 vy{0.0f};
    f32 angularVel{0.0f};
    f32 invMass{1.0f};
    f32 invInertia{0.0f};
    f32 gravityScale{1.0f};
    u8 linearDamping_q4{0};     // [0..255] -> [0.0..15.9375] s^-1
    u8 angularDamping_q4{0};
    u8 sleepTimerTicks{0};
    BodyType2D bodyType{BodyType2D::Dynamic};
    BodyFlags2D flags{BodyFlags2D::UseGravity | BodyFlags2D::CanSleep | BodyFlags2D::Enabled};
    ContactFlags contacts{ContactFlags::None};
    u8 islandId{0};
    u8 userTag{0};

    [[nodiscard]] constexpr bool IsStatic() const noexcept { return bodyType == BodyType2D::Static || (bodyType == BodyType2D::Dynamic && invMass <= 0.0f); }
    [[nodiscard]] constexpr bool IsKinematic() const noexcept { return bodyType == BodyType2D::Kinematic; }
    [[nodiscard]] constexpr bool IsDynamic() const noexcept { return bodyType == BodyType2D::Dynamic && invMass > 0.0f; }
    [[nodiscard]] constexpr bool IsSleeping() const noexcept { return HasPhysicsFlag(flags, BodyFlags2D::Sleeping); }
    [[nodiscard]] constexpr bool IsBullet() const noexcept { return HasPhysicsFlag(flags, BodyFlags2D::BulletCCD); }

    [[nodiscard]] constexpr f32 LinearDamping() const noexcept {
        return static_cast<f32>(linearDamping_q4) * (1.0f / 16.0f);
    }
    constexpr void SetLinearDamping(f32 d) noexcept {
        linearDamping_q4 = static_cast<u8>(std::clamp(d, 0.0f, 15.9375f) * 16.0f + 0.5f);
    }

    [[nodiscard]] constexpr f32 AngularDamping() const noexcept {
        return static_cast<f32>(angularDamping_q4) * (1.0f / 16.0f);
    }
    constexpr void SetAngularDamping(f32 d) noexcept {
        angularDamping_q4 = static_cast<u8>(std::clamp(d, 0.0f, 15.9375f) * 16.0f + 0.5f);
    }

    constexpr void WakeUp() noexcept {
        flags &= ~BodyFlags2D::Sleeping;
        sleepTimerTicks = 0U;
    }

    constexpr void ApplyLinearImpulse(f32 ix, f32 iy) noexcept {
        if (!IsDynamic()) return;
        vx += ix * invMass;
        vy += iy * invMass;
        WakeUp();
    }

    constexpr void ApplyAngularImpulse(f32 angularImpulse) noexcept {
        if (!IsDynamic() || invInertia <= 0.0f) return;
        angularVel += angularImpulse * invInertia;
        WakeUp();
    }

    constexpr void ApplyImpulseAtOffset(f32 ix, f32 iy, f32 rx, f32 ry) noexcept {
        if (!IsDynamic()) return;
        vx += ix * invMass;
        vy += iy * invMass;
        angularVel += (rx * iy - ry * ix) * invInertia;
        WakeUp();
    }

    constexpr void ApplyForce(f32 fx, f32 fy, f32 dt) noexcept {
        ApplyLinearImpulse(fx * dt, fy * dt);
    }

    constexpr void SetBoxMass(f32 mass, f32 halfW, f32 halfH, bool fixedRotation = true) noexcept {
        if (mass <= 0.0f || bodyType != BodyType2D::Dynamic) {
            invMass = 0.0f;
            invInertia = 0.0f;
            return;
        }
        invMass = 1.0f / mass;
        if (fixedRotation) {
            flags |= BodyFlags2D::FixedRotation;
            invInertia = 0.0f;
        } else {
            flags &= ~BodyFlags2D::FixedRotation;
            const f32 inertia = (mass * (halfW * halfW + halfH * halfH)) * (1.0f / 3.0f);
            invInertia = (inertia > 1e-6f) ? (1.0f / inertia) : 0.0f;
        }
    }

    constexpr void SetCircleMass(f32 mass, f32 radius, bool fixedRotation = false) noexcept {
        if (mass <= 0.0f || bodyType != BodyType2D::Dynamic) {
            invMass = 0.0f;
            invInertia = 0.0f;
            return;
        }
        invMass = 1.0f / mass;
        if (fixedRotation) {
            flags |= BodyFlags2D::FixedRotation;
            invInertia = 0.0f;
        } else {
            flags &= ~BodyFlags2D::FixedRotation;
            const f32 inertia = 0.5f * mass * radius * radius;
            invInertia = (inertia > 1e-6f) ? (1.0f / inertia) : 0.0f;
        }
    }

    constexpr void ComputeMassFromCollider(const Collider2D& col, bool fixedRotation = false) noexcept {
        if (bodyType != BodyType2D::Dynamic || col.IsSensor()) {
            invMass = 0.0f;
            invInertia = 0.0f;
            return;
        }
        const f32 rho = std::max(0.01f, col.Density());
        const f32 hw = std::max(0.01f, col.localBounds.HalfW());
        const f32 hh = std::max(0.01f, col.localBounds.HalfH());
        if (col.shape == ShapeKind2D::Circle) {
            SetCircleMass(kPi * hw * hw * rho, hw, fixedRotation);
        } else {
            SetBoxMass(4.0f * hw * hh * rho, hw, hh, fixedRotation);
        }
    }
};
static_assert(sizeof(RigidBody2D) == 32);

// 32-byte (2 per 64B cache line, zero padding) kinematic character controller component for MoveAndSlide.
// Pure core physics state with zero game-specific pushbox groups or input countdown timers.
struct alignas(32) CharacterMotor2D {
    static constexpr u8 kTypeId = 10;

    f32 vx{0.0f};
    f32 vy{0.0f};
    f32 groundNormalX{0.0f};
    f32 groundNormalY{1.0f};
    CompactAabb2D bounds{};
    EntityId carrierId{kNullEntity};
    u16 solidMask{0x0001U};
    ContactFlags contacts{ContactFlags::None};
    BodyFlags2D flags{BodyFlags2D::UseGravity | BodyFlags2D::Enabled};

    [[nodiscard]] constexpr bool IsGrounded() const noexcept {
        return HasPhysicsFlag(contacts, ContactFlags::Ground);
    }
    [[nodiscard]] constexpr Vec2 GroundNormal() const noexcept {
        return {groundNormalX, groundNormalY};
    }
};
static_assert(sizeof(CharacterMotor2D) == 32);

// 32-byte (2 per 64B cache line, zero padding) Box2D v3 Soft-Step distance/spring/rope joint constraint component.
struct alignas(32) DistanceJoint2D {
    static constexpr u8 kTypeId = 11;

    EntityId entityA{kNullEntity};
    EntityId entityB{kNullEntity};
    f32 minLength{0.0f};
    f32 maxLength{1.0f};
    f32 impulse{0.0f};
    i16 localAnchorAX_q8{0};
    i16 localAnchorAY_q8{0};
    i16 localAnchorBX_q8{0};
    i16 localAnchorBY_q8{0};
    u16 hertz_q6{0U};           // 0 = rigid rod/rope, > 0 = harmonic spring frequency in Q10.6 Hz
    u8 dampingRatio_q6{64U};    // 64 == 1.0f critical damping
    u8 enabled{1U};

    [[nodiscard]] constexpr Vec2 LocalAnchorA() const noexcept {
        return {
            static_cast<f32>(localAnchorAX_q8) * (1.0f / 256.0f),
            static_cast<f32>(localAnchorAY_q8) * (1.0f / 256.0f)};
    }
    [[nodiscard]] constexpr Vec2 LocalAnchorB() const noexcept {
        return {
            static_cast<f32>(localAnchorBX_q8) * (1.0f / 256.0f),
            static_cast<f32>(localAnchorBY_q8) * (1.0f / 256.0f)};
    }
    constexpr void SetAnchors(Vec2 anchorA, Vec2 anchorB) noexcept {
        localAnchorAX_q8 = CompactAabb2D::QuantizeS16(anchorA.x);
        localAnchorAY_q8 = CompactAabb2D::QuantizeS16(anchorA.y);
        localAnchorBX_q8 = CompactAabb2D::QuantizeS16(anchorB.x);
        localAnchorBY_q8 = CompactAabb2D::QuantizeS16(anchorB.y);
    }
    constexpr void SetSoftSpring(f32 restLen, f32 hertz, f32 dampingRatio = 1.0f) noexcept {
        minLength = restLen;
        maxLength = restLen;
        hertz_q6 = static_cast<u16>(std::clamp(hertz, 0.0f, 1000.0f) * 64.0f + 0.5f);
        dampingRatio_q6 = static_cast<u8>(std::clamp(dampingRatio, 0.0f, 3.9f) * 64.0f + 0.5f);
    }
    [[nodiscard]] constexpr f32 Hertz() const noexcept { return static_cast<f32>(hertz_q6) * (1.0f / 64.0f); }
    [[nodiscard]] constexpr f32 DampingRatio() const noexcept { return static_cast<f32>(dampingRatio_q6) * (1.0f / 64.0f); }
};
static_assert(sizeof(DistanceJoint2D) == 32);

// 32-byte (2 per 64B cache line, zero padding) raycast intersection result.
struct alignas(32) RayHit2D {
    f32 fraction{1.0f};
    f32 distance{0.0f};
    f32 pointX{0.0f};
    f32 pointY{0.0f};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    EntityId hitEntity{kNullEntity};
    TileMaterialFlags tileFlags{TileMaterialFlags::Empty};
    ShapeKind2D shapeKind{ShapeKind2D::Aabb};
    u8 hitValid{0U};
    u8 startSolid{0U};

    [[nodiscard]] constexpr bool Hit() const noexcept { return hitValid != 0U; }
};
static_assert(sizeof(RayHit2D) == 32);

// 32-byte (2 per 64B cache line, zero padding) swept-shape time-of-impact result.
struct alignas(32) SweepHit2D {
    f32 toi{1.0f};
    f32 pointX{0.0f};
    f32 pointY{0.0f};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    f32 penetration{0.0f};
    EntityId hitEntity{kNullEntity};
    TileMaterialFlags tileFlags{TileMaterialFlags::Empty};
    u8 startPenetrating{0U};
    u8 hitValid{0U};
    u8 isOneWay{0U};

    [[nodiscard]] constexpr bool Hit() const noexcept { return hitValid != 0U; }
};
static_assert(sizeof(SweepHit2D) == 32);

// 64-byte (1 cache line exact, zero padding) 2-point contact manifold with warm-started impulses and speculative support.
struct alignas(64) ContactManifold2D {
    EntityId entityA{kNullEntity};
    EntityId entityB{kNullEntity};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    f32 pointX[2]{0.0f, 0.0f};
    f32 pointY[2]{0.0f, 0.0f};
    // Negative separation indicates penetration depth; positive separation indicates a speculative contact gap.
    f32 separation[2]{0.0f, 0.0f};
    f32 normalImpulse[2]{0.0f, 0.0f};
    f32 tangentImpulse[2]{0.0f, 0.0f};
    u16 friction_q16{0U};
    u16 restitution_q16{0U};
    u8 pointCount{0U};
    u8 isSensor{0U};
    u8 persistedTicks{0U};
    u8 activeThisTick{1U};

    [[nodiscard]] constexpr u64 PairKey() const noexcept {
        return (static_cast<u64>(entityA.Packed()) << 32U) | static_cast<u64>(entityB.Packed());
    }
};
static_assert(sizeof(ContactManifold2D) == 64);

// 32-byte (2 per 64B cache line, zero padding) contact/sensor transition event record.
struct alignas(32) ContactEvent2D {
    EntityId entityA{kNullEntity};
    EntityId entityB{kNullEntity};
    f32 pointX{0.0f};
    f32 pointY{0.0f};
    f32 normalX{0.0f};
    f32 normalY{0.0f};
    f32 approachSpeed{0.0f};
    ContactEventKind kind{ContactEventKind::BeginTouch};
    u8 pointCount{1U};
    u16 materialTag{0U};
};
static_assert(sizeof(ContactEvent2D) == 32);

// 48-byte (zero padding) simulation and Soft-Step solver tuning parameters.
struct alignas(16) PhysicsConfig {
    f32 gravityX{0.0f};
    f32 gravityY{-24.0f};
    f32 contactHertz{30.0f};        // Box2D v3 Soft-Step constraint frequency (Hz)
    f32 contactDampingRatio{1.0f};  // Critical damping ratio (zeta = 1.0)
    f32 warmStartFactor{0.85f};     // Impulse retention factor across ticks
    f32 minGroundNormalY{0.65f};    // cos(~49 deg) walkable surface normal threshold
    f32 groundSnapDist{0.15f};      // Downward ground/slope snap distance (m)
    f32 maxStepHeight{0.18f};       // Maximum stair/ledge step-up height (m)
    u16 maxManifolds{1024U};
    u16 maxContactEvents{512U};
    u16 maxSpatialProxies{1024U};
    u8 velocityIterations{6U};
    u8 sleepTicksThreshold{60U};
    u32 arenaBytes{2U * 1024U * 1024U};
};
static_assert(sizeof(PhysicsConfig) == 48);

// 32-byte (2 per 64B cache line, zero padding) cooked static collision map header ("ECOL" v1).
struct alignas(16) CookedCollisionMapHeader {
    static constexpr u32 kMagic = 0x4C4F4345U; // "ECOL"
    static constexpr u16 kVersion = 1U;

    u32 magic{kMagic};
    u16 version{kVersion};
    u16 widthTiles{0U};
    u16 heightTiles{0U};
    u16 chunkCols{0U};
    u16 chunkRows{0U};
    u16 flags{0U};
    f32 tileSizeMeters{1.0f};
    f32 originX{0.0f};
    f32 originY{0.0f};
    u32 payloadChecksum{0U};
};
static_assert(sizeof(CookedCollisionMapHeader) == 32);

} // namespace en

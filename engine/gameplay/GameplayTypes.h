#pragma once

#include "input/ActionBuffer.h"
#include "physics/PhysicsTypes.h"

namespace en {

class World;
class AssetBank;
class GameplayPipeline;

enum class SimCueKind : u8 {
    AudioOneShot = 0,
    SpawnPrefabEffect = 1,
    CameraTraumaPulse = 2,
    GamepadRumblePulse = 3,
    CustomGameEvent = 4,
};

struct alignas(32) SimCue {
    AssetId assetId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    EntityId emitter{kNullEntity};
    u16 magnitude_q16{32768};
    u16 durationTicks{10};
    SimCueKind kind{SimCueKind::AudioOneShot};
    TimeChannel channel{TimeChannel::World};
    u16 userTag{0};
    u32 customPayload{0};
};
static_assert(sizeof(SimCue) == 32);

enum class CancelCategory : u8 {
    None = 0,
    Dash = 1U << 0,
    Jump = 1U << 1,
    LightAttack = 1U << 2,
    HeavyAttack = 1U << 3,
    Parry = 1U << 4,
    Special = 1U << 5,
    Any = 0x3FU,
};
consteval bool enable_bitmask_operators(CancelCategory) { return true; }

enum class HitboxRigFlags : u8 {
    None = 0,
    HurtboxActive = 1U << 0,
    HitboxActive = 1U << 1,
    Invulnerable = 1U << 2,
    ParryWindow = 1U << 3,
    SuperArmor = 1U << 4,
    ReflectProjectiles = 1U << 5,
};
consteval bool enable_bitmask_operators(HitboxRigFlags) { return true; }

struct alignas(16) StateMachine2D {
    static constexpr u8 kTypeId = 19;
    BlobHandle rigBlob{};
    u16 stateTicks{0};
    u16 totalStateTicks{0};
    CancelCategory cancelMask{CancelCategory::Any};
    CancelCategory onHitCancelMask{CancelCategory::Any};
    u8 lastAnimFrame{0xFFU};
    u8 stateId{0};
    u8 prevStateId{0};
    u8 animClipId{0};
    u8 hurtboxProfileId{0};
    u8 fsmFlags{0};

    constexpr void TransitionTo(u8 nextState, u8 clipId = 0) noexcept {
        prevStateId = stateId;
        stateId = nextState;
        animClipId = clipId;
        stateTicks = 0;
        lastAnimFrame = 0xFFU;
    }
};
static_assert(sizeof(StateMachine2D) == 16);

struct alignas(32) HitboxRig2D {
    static constexpr u8 kTypeId = 20;
    u64 hitBloomMask{0};
    CompactAabb2D hurtbox{CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 0.8f)};
    CompactAabb2D primaryHitbox{CompactAabb2D::FromMeters(0.75f, 0.0f, 0.65f, 0.5f)};
    u16 hitLayerMask{2};
    u16 hurtLayerMask{1};
    u16 swingId{0};
    u8 activeDamage{10};
    HitboxRigFlags rigFlags{HitboxRigFlags::HurtboxActive};

    constexpr void BeginNewSwing(u8 damage, CompactAabb2D box) noexcept {
        ++swingId;
        hitBloomMask = 0;
        activeDamage = damage;
        primaryHitbox = box;
        rigFlags |= HitboxRigFlags::HitboxActive;
    }

    constexpr void EndSwing() noexcept {
        rigFlags &= ~HitboxRigFlags::HitboxActive;
    }

    [[nodiscard]] static constexpr u64 ComputePairBits(u16 swing, EntityId target) noexcept {
        const u64 h = Mix64((static_cast<u64>(swing) << 32U) | target.Packed());
        return (1ULL << (h & 63U)) | (1ULL << ((h >> 8U) & 63U));
    }

    [[nodiscard]] constexpr bool HasAlreadyHit(EntityId target) const noexcept {
        const u64 bits = ComputePairBits(swingId, target);
        return (hitBloomMask & bits) == bits;
    }

    constexpr void MarkHit(EntityId target) noexcept {
        hitBloomMask |= ComputePairBits(swingId, target);
    }
};
static_assert(sizeof(HitboxRig2D) == 32);

struct alignas(16) DamageReceiver2D {
    static constexpr u8 kTypeId = 21;
    i16 hp{100};
    i16 maxHp{100};
    u16 iframeTicks{0};
    u16 stunTicks{0};
    i16 poise{30};
    i16 maxPoise{30};
    u8 teamId{0};
    u8 statusMask{0};
    u16 statusTicks{0};

    [[nodiscard]] constexpr bool IsDead() const noexcept { return hp <= 0; }
};
static_assert(sizeof(DamageReceiver2D) == 16);

struct alignas(16) SafeGroundBreadcrumb2D {
    static constexpr u8 kTypeId = 22;
    f32 safeX{0.0f};
    f32 safeY{0.0f};
    f32 prevSafeX{0.0f};
    f32 prevSafeY{0.0f};
};
static_assert(sizeof(SafeGroundBreadcrumb2D) == 16);

struct alignas(16) CameraController2D {
    static constexpr u8 kTypeId = 23;
    Rect activeRoomBounds{-1000.0f, -1000.0f, 2000.0f, 2000.0f};
    EntityId primaryTarget{kNullEntity};
    EntityId secondaryTarget{kNullEntity};
    f32 velX{0.0f};
    f32 velY{0.0f};
    f32 kickOffsetX{0.0f};
    f32 kickOffsetY{0.0f};
    f32 kickVelX{0.0f};
    f32 kickVelY{0.0f};
    f32 lookaheadDist{1.5f};
    f32 deadzoneHalfW{0.6f};
    f32 deadzoneHalfH{0.5f};
    f32 springFreqX{10.0f};
    f32 springFreqY{8.0f};
    f32 trauma{0.0f};
    f32 secondaryWeight{0.35f};
    f32 verticalOffset{0.8f};

    constexpr void AddTrauma(f32 amount) noexcept {
        trauma = std::min(1.0f, trauma + amount);
    }
    constexpr void ApplyKick(Vec2 impulse) noexcept {
        kickVelX += impulse.x;
        kickVelY += impulse.y;
    }
};
static_assert(sizeof(CameraController2D) == 80);

struct alignas(32) ResolvedHitEvent {
    EntityId attacker{kNullEntity};
    EntityId defender{kNullEntity};
    Vec2 contactPoint{};
    Vec2 knockbackDir{1.0f, 0.0f};
    i16 damage{10};
    u16 hitstopTicks{4};
    bool parried{false};
    bool reflected{false};
    u16 hitFlags{0};
};
static_assert(sizeof(ResolvedHitEvent) == 32);

inline constexpr u32 kCookedRigMagic = 0x47495245U; // 'ERIG'

struct alignas(16) CookedRigHeader {
    u32 magic{kCookedRigMagic};
    u16 version{1};
    u16 directiveCount{0};
    u32 clipCount{1};
    u32 schemaCrc32{0};
};
static_assert(sizeof(CookedRigHeader) == 16);

struct alignas(16) CookedRigFrameDirective {
    CompactAabb2D hitbox{};
    i16 rootVelX_q4{0};
    i16 rootVelY_q4{0};
    u8 stateId{0};
    u8 triggerTick{0};
    u8 enableHitbox{0};
    u8 damage{10};
};
static_assert(sizeof(CookedRigFrameDirective) == 16);

struct GameplayPolicyTable {
    void (*onStepStateMachines)(
        World& world,
        const AssetBank& assets,
        const SimStepView& step,
        const TickInput& tickInput,
        ActionBuffer& actionBuffer,
        GameplayPipeline& pipeline) noexcept {nullptr};

    void (*onResolveCombatHits)(
        World& world,
        const SimStepView& step,
        std::span<const ResolvedHitEvent> hits,
        GameplayPipeline& pipeline) noexcept {nullptr};
};

} // namespace en

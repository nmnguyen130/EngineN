#include "gameplay/GameplayPipeline.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>

namespace en::gameplay {

namespace {

constexpr void StepAnalyticCriticallyDampedSpring(
    f32& inoutPos, f32& inoutVel,
    f32 targetPos, f32 naturalFreqHz, f32 dt) noexcept {
    const f32 omega   = naturalFreqHz * 6.283185307f;
    const f32 x       = omega * dt;
    const f32 expTerm = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    const f32 diff = inoutPos - targetPos;
    const f32 temp = (inoutVel + omega * diff) * dt;
    inoutPos = targetPos + (diff + temp) * expTerm;
    inoutVel = (inoutVel - temp * omega) * expTerm;
}

[[nodiscard]] constexpr f32 SoftCushionClamp(f32 val, f32 minB, f32 maxB, f32 cushionWidth) noexcept {
    if (minB >= maxB) return 0.5f * (minB + maxB);
    const f32 m = std::min(cushionWidth, 0.25f * (maxB - minB));
    if (m <= 1e-4f) return std::clamp(val, minB, maxB);
    if (val < minB + m) {
        const f32 t = std::min(1.0f, ((minB + m) - val) / (2.0f * m));
        return (minB + m) - m * (t * (2.0f - t));
    }
    if (val > maxB - m) {
        const f32 t = std::min(1.0f, (val - (maxB - m)) / (2.0f * m));
        return (maxB - m) + m * (t * (2.0f - t));
    }
    return val;
}

[[nodiscard]] constexpr f32 HashSignedNoise(u64 tickIndex, u32 seedChannel) noexcept {
    u64 h = (tickIndex * 0x9E3779B97F4A7C15ULL) ^ (static_cast<u64>(seedChannel) * 0xBF58476D1CE4E5B9ULL);
    h = (h ^ (h >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27u)) * 0x94D049BB133111EBULL;
    h = h ^ (h >> 31u);
    return static_cast<f32>(h & 0xFFFFFFULL) * (2.0f / 16777215.0f) - 1.0f;
}

[[nodiscard]] constexpr u32 CanonicalFloatBits(f32 v) noexcept {
    return (v == 0.0f) ? 0u : std::bit_cast<u32>(v);
}

} // namespace

GameplayPipeline::~GameplayPipeline() noexcept {
    Shutdown();
}

GameplayPipeline::GameplayPipeline(GameplayPipeline&& other) noexcept
    : m_policies(other.m_policies),
      m_kinematicCfg(other.m_kinematicCfg),
      m_invPixelsPerMeter(other.m_invPixelsPerMeter),
      m_hitScratch(other.m_hitScratch),
      m_cueBuffer(other.m_cueBuffer),
      m_spatial(other.m_spatial),
      m_arena(other.m_arena),
      m_arenaBytes(other.m_arenaBytes),
      m_activeFlagWordsMask(other.m_activeFlagWordsMask),
      m_cueCount(other.m_cueCount),
      m_worldFlags(other.m_worldFlags) {
    other.m_arena               = nullptr;
    other.m_arenaBytes          = 0;
    other.m_hitScratch          = nullptr;
    other.m_cueBuffer           = nullptr;
    other.m_cueCount            = 0;
    other.m_activeFlagWordsMask = 0;
}

GameplayPipeline& GameplayPipeline::operator=(GameplayPipeline&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_policies            = other.m_policies;
        m_kinematicCfg        = other.m_kinematicCfg;
        m_invPixelsPerMeter   = other.m_invPixelsPerMeter;
        m_hitScratch          = other.m_hitScratch;
        m_cueBuffer           = other.m_cueBuffer;
        m_spatial             = other.m_spatial;
        m_arena               = other.m_arena;
        m_arenaBytes          = other.m_arenaBytes;
        m_activeFlagWordsMask = other.m_activeFlagWordsMask;
        m_cueCount            = other.m_cueCount;
        m_worldFlags          = other.m_worldFlags;

        other.m_arena               = nullptr;
        other.m_arenaBytes          = 0;
        other.m_hitScratch          = nullptr;
        other.m_cueBuffer           = nullptr;
        other.m_cueCount            = 0;
        other.m_activeFlagWordsMask = 0;
    }
    return *this;
}

Result<void, GameplayErrc> GameplayPipeline::Init(
    world::World& world,
    const GameplayPolicyTable& policies,
    const GameplayPipelineConfig& cfg) noexcept {
    Shutdown();

    const usize spatialBytes = (SpatialIndex2D::RequiredArenaBytes() + 63u) & ~usize{63u};
    const usize hitsBytes    = (sizeof(ResolvedHitEvent) * kMaxHitsPerTick + 63u) & ~usize{63u};
    const usize cuesBytes    = (sizeof(SimCue) * kMaxCuesPerFrame + 63u) & ~usize{63u};
    const usize minBytes     = spatialBytes + hitsBytes + cuesBytes;

    if (cfg.arenaBytes < minBytes) [[unlikely]] {
        return Err(GameplayErrc::InvalidConfig);
    }

    const usize alignedSize = (cfg.arenaBytes + 63u) & ~usize{63u};
    void* rawMem = ::operator new(alignedSize, std::align_val_t{64}, std::nothrow);
    if (rawMem == nullptr) [[unlikely]] {
        return Err(GameplayErrc::OutOfMemory);
    }

    m_arena               = static_cast<u8*>(rawMem);
    m_arenaBytes          = alignedSize;
    m_policies            = policies;
    m_kinematicCfg        = cfg.kinematicConfig;
    m_invPixelsPerMeter   = 1.0f / std::max(1.0f, cfg.pixelsPerMeter);
    m_activeFlagWordsMask = 0ULL;
    m_worldFlags.fill(0ULL);
    m_cueCount            = 0;

    m_spatial.BindArena(std::span<u8>{m_arena, spatialBytes});

    u8* cursor   = m_arena + spatialBytes;
    m_hitScratch = reinterpret_cast<ResolvedHitEvent*>(cursor);
    cursor += hitsBytes;
    m_cueBuffer  = reinterpret_cast<SimCue*>(cursor);

    if (!world.RegisterComponentPool<KinematicMotor2D>(cfg.maxMotors) ||
        !world.RegisterComponentPool<KinematicCarrier2D>(cfg.maxCarriers) ||
        !world.RegisterComponentPool<MotionStack2D>(cfg.maxMotors) ||
        !world.RegisterComponentPool<StateMachine2D>(cfg.maxStateMachines) ||
        !world.RegisterComponentPool<HitboxRig2D>(cfg.maxHitboxRigs) ||
        !world.RegisterComponentPool<DamageReceiver2D>(cfg.maxDamageReceivers) ||
        !world.RegisterComponentPool<SafeGroundBreadcrumb2D>(cfg.maxBreadcrumbs) ||
        !world.RegisterComponentPool<CameraController2D>(cfg.maxCameras)) [[unlikely]] {
        Shutdown();
        return Err(GameplayErrc::WorldRegistrationFailed);
    }

    return {};
}

void GameplayPipeline::Shutdown() noexcept {
    m_spatial.ClearCollisionMap();
    if (m_arena != nullptr) {
        ::operator delete(m_arena, std::align_val_t{64}, std::nothrow);
        m_arena = nullptr;
    }
    m_arenaBytes          = 0;
    m_hitScratch          = nullptr;
    m_cueBuffer           = nullptr;
    m_cueCount            = 0;
    m_activeFlagWordsMask = 0;
}

Result<void, GameplayErrc> GameplayPipeline::SetActiveRoomCollision(
    const assets::AssetBank& bank,
    assets::BlobHandle collisionMapBlob) noexcept {
    return m_spatial.BindCollisionMapBlob(bank.GetBlob(collisionMapBlob));
}

void GameplayPipeline::Tick(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const input::TickInput& tickInput,
    input::ActionBuffer& actionBuffer) noexcept {
    // Stage 1: Advance moving carriers and transfer displacements onto grounded riders.
    KinematicSolver2D::StepCarriersAndRiders(world, m_spatial, step, m_kinematicCfg);

    // Stage 2: Single-pass animation & frame-edge directives (0-tick hitbox latency).
    StateAndCombatSolver::DispatchAnimationFrameDirectives(world, assetBank, step, *this);

    // Stage 3: Advance forgiveness/iframe timers and evaluate game policy callbacks against cancelMask.
    StateAndCombatSolver::StepStateMachinesAndPolicies(
        world, assetBank, step, tickInput, actionBuffer, m_policies, *this);

    // Stage 4: Integrate ambient non-colliding Velocity2D, rebuild kinematic-only grid, solve swept motors, propagate transforms.
    world.StepAmbientVelocities(step);
    m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::KinematicsOnly);
    KinematicSolver2D::StepMotorsAndMotionStacks(world, m_spatial, step, m_kinematicCfg);
    world.PropagateTransforms();

    // Stage 5: Conditionally rebuild combat grid only if at least one hitbox slice is active, then arbitrate hits.
    m_spatial.RebuildDynamicGrid(world, SpatialRebuildMode::CombatOnlyIfActive);
    const u32 hitCount = m_spatial.GatherCombatOverlaps(
        world, assetBank, std::span<ResolvedHitEvent>{m_hitScratch, kMaxHitsPerTick});
    if (hitCount > 0) {
        StateAndCombatSolver::ResolveCombatHits(
            world, step, std::span<const ResolvedHitEvent>{m_hitScratch, hitCount}, m_policies, *this);
    }

    // Stage 6: Update Camera Director on TimeChannel::Camera (unfrozen during combat hitstop).
    StepCameraControllers(world, step);
}

void GameplayPipeline::StepCameraControllers(
    world::World& world,
    time::SimStepView step) noexcept {
    if (step.IsFrozen(time::TimeChannel::Camera)) return;
    const f32 dt = step.Dt(time::TimeChannel::Camera);
    if (dt <= 0.0f) return;

    auto& camPool      = world.GetPool<CameraController2D>();
    const u16 camCount = camPool.Size();
    if (camCount == 0) return;

    const auto camIds = camPool.Entities();
    auto cams         = camPool.Components();
    const auto* __restrict worldTf = world.WorldTransforms().data();

    constexpr world::ComponentMask kMotorBit  = world::MakeComponentMask<KinematicMotor2D>();
    constexpr world::ComponentMask kMotionBit = world::MakeComponentMask<MotionStack2D>();
    constexpr world::ComponentMask kRigCamBit = world::MakeComponentMask<world::CameraRig2D>();

    const auto& motorPool  = world.GetPool<KinematicMotor2D>();
    const auto& motionPool = world.GetPool<MotionStack2D>();
    const auto& camRigPool = world.GetPool<world::CameraRig2D>();

    bool anyCameraMoved = false;

    for (u16 i = 0; i < camCount; ++i) {
        CameraController2D& cam = cams[i];
        if (cam.primaryTarget == world::kNullEntity || !world.IsAlive(cam.primaryTarget)) continue;

        const world::EntityId camId       = camIds[i];
        world::LocalTransform2D& camLocal = world.ModifyLocalTransformByIndex(camId.index);
        anyCameraMoved = true;

        f32 baseCamX = camLocal.x - cam.kickOffsetX;
        f32 baseCamY = camLocal.y - cam.kickOffsetY;

        const u16 priIdx                   = cam.primaryTarget.index;
        const world::ComponentMask priMask = world.GetComponentMaskByIndex(priIdx);
        const auto& priWt                  = worldTf[priIdx];
        const auto flags                   = static_cast<CameraCtrlFlags>(cam.cameraFlags);

        // 1. Branchless Horizontal Focus Box
        const f32 deadzoneW = static_cast<f32>(cam.deadzoneHalfW_q4) * (1.0f / 16.0f);
        f32 targetX         = std::clamp(baseCamX, priWt.x - deadzoneW, priWt.x + deadzoneW);
        f32 targetY         = priWt.y;

        // 2. Ground-Latching, Airborne Vertical Deadzone & Fast-Fall Look-Down
        bool isGrounded = false;
        f32 actorVx = 0.0f, actorVy = 0.0f;
        if ((priMask & kMotorBit) != 0) {
            const KinematicMotor2D& motor = motorPool.DirectAtEntityIndex(priIdx);
            isGrounded = HasContact(motor.contacts, ContactFlags::Ground);
            actorVx    = motor.vx;
            actorVy    = motor.vy;
            if (isGrounded) cam.latchedGroundY = priWt.y;
        }

        if (HasFlag(flags, CameraCtrlFlags::LockVerticalAirborne)) {
            const f32 deadzoneH = static_cast<f32>(cam.deadzoneHalfH_q4) * (1.0f / 16.0f);
            cam.latchedGroundY  = std::clamp(cam.latchedGroundY, priWt.y - deadzoneH, priWt.y + deadzoneH);
            targetY             = cam.latchedGroundY;
            if (!isGrounded && actorVy < -14.0f) {
                targetY -= std::min(1.0f, (-actorVy - 14.0f) * (1.0f / 12.0f)) * 2.2f;
            }
        }

        // 3. Turn-Confirmation Filtered Lookahead
        if ((priMask & kMotionBit) != 0) {
            const i8 facing = motionPool.DirectAtEntityIndex(priIdx).FacingSign();
            if ((actorVx * static_cast<f32>(facing)) > 0.6f) {
                cam.sustainedMoveTicks += (cam.sustainedMoveTicks < 255u);
            } else if (std::abs(actorVx) < 0.2f) {
                cam.sustainedMoveTicks -= (cam.sustainedMoveTicks > 0u);
            } else {
                cam.sustainedMoveTicks = 0u;
            }

            const f32 curLookX = static_cast<f32>(cam.lookaheadX_q8) * (1.0f / 256.0f);
            f32 desiredLookX   = curLookX;

            if (cam.sustainedMoveTicks >= 10u) {
                const f32 velScale = HasFlag(flags, CameraCtrlFlags::VelocityLookahead)
                    ? std::clamp(std::abs(actorVx) * (1.0f / 6.5f), 0.35f, 1.0f)
                    : 1.0f;
                desiredLookX = static_cast<f32>(facing) * 1.65f * velScale;
            }

            const f32 nextLookX = curLookX + (desiredLookX - curLookX) * std::min(1.0f, dt * 4.5f);
            cam.lookaheadX_q8   = static_cast<i16>(std::clamp(nextLookX * 256.0f, -32767.0f, 32767.0f));
        }

        targetX += static_cast<f32>(cam.lookaheadX_q8) * (1.0f / 256.0f);
        targetY += static_cast<f32>(cam.peekOffsetY_q8) * (1.0f / 256.0f);

        // 4. Secondary Boss / Point-of-Interest weighted framing
        if (cam.secondaryTarget != world::kNullEntity &&
            cam.secondaryWeight_q8 > 0 &&
            world.IsAlive(cam.secondaryTarget)) {
            const auto& secWt = worldTf[cam.secondaryTarget.index];
            const f32 w = static_cast<f32>(cam.secondaryWeight_q8) * (1.0f / 255.0f);
            targetX += (secWt.x - targetX) * w;
            targetY += (secWt.y - targetY) * w;
        }

        // 5. Viewport-Aware Room Boundary Clamping & Soft Cushioning
        bool hasValidRoomBounds = false;
        f32 minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;

        if (HasFlag(flags, CameraCtrlFlags::ClampToRoom) &&
            cam.activeRoomBounds.halfW_q8 > 0 && cam.activeRoomBounds.halfH_q8 > 0) {
            hasValidRoomBounds = true;

            f32 halfVpW = 0.0f, halfVpH = 0.0f;
            if ((world.GetComponentMaskByIndex(camId.index) & kRigCamBit) != 0) {
                const world::CameraRig2D& rig = camRigPool.DirectAtEntityIndex(camId.index);
                const f32 scale = 0.5f * m_invPixelsPerMeter / std::max(0.01f, rig.zoom);
                halfVpW = rig.viewport.w * scale;
                halfVpH = rig.viewport.h * scale;
            }

            minX = (cam.activeRoomBounds.OffsetX() - cam.activeRoomBounds.HalfW()) + halfVpW;
            maxX = (cam.activeRoomBounds.OffsetX() + cam.activeRoomBounds.HalfW()) - halfVpW;
            minY = (cam.activeRoomBounds.OffsetY() - cam.activeRoomBounds.HalfH()) + halfVpH;
            maxY = (cam.activeRoomBounds.OffsetY() + cam.activeRoomBounds.HalfH()) - halfVpH;

            if (cam.roomBlendTicks > 0) {
                --cam.roomBlendTicks;
                const f32 rawT = 1.0f - (static_cast<f32>(cam.roomBlendTicks) /
                                         static_cast<f32>(std::max<u8>(1u, cam.roomBlendDuration)));
                const f32 s = rawT * rawT * (3.0f - 2.0f * rawT);
                const f32 prevMinX = (cam.prevRoomBounds.OffsetX() - cam.prevRoomBounds.HalfW()) + halfVpW;
                const f32 prevMaxX = (cam.prevRoomBounds.OffsetX() + cam.prevRoomBounds.HalfW()) - halfVpW;
                const f32 prevMinY = (cam.prevRoomBounds.OffsetY() - cam.prevRoomBounds.HalfH()) + halfVpH;
                const f32 prevMaxY = (cam.prevRoomBounds.OffsetY() + cam.prevRoomBounds.HalfH()) - halfVpH;
                minX = prevMinX + (minX - prevMinX) * s;
                maxX = prevMaxX + (maxX - prevMaxX) * s;
                minY = prevMinY + (minY - prevMinY) * s;
                maxY = prevMaxY + (maxY - prevMaxY) * s;
            }

            const bool soft = HasFlag(flags, CameraCtrlFlags::SoftBoundaryCushion);
            targetX = SoftCushionClamp(targetX, minX, maxX, soft ? 2.0f : 0.0f);
            targetY = SoftCushionClamp(targetY, minY, maxY, soft ? 1.5f : 0.0f);
        }

        // 6. Step Analytic Critically-Damped Springs on Base Camera Coordinates
        const f32 freqX = std::max(0.5f, static_cast<f32>(cam.springFreqX_q4) * (1.0f / 16.0f));
        const f32 freqY = std::max(0.5f, static_cast<f32>(cam.springFreqY_q4) * (1.0f / 16.0f));

        if (!HasFlag(flags, CameraCtrlFlags::LockAxisX)) StepAnalyticCriticallyDampedSpring(baseCamX, cam.velX, targetX, freqX, dt);
        if (!HasFlag(flags, CameraCtrlFlags::LockAxisY)) StepAnalyticCriticallyDampedSpring(baseCamY, cam.velY, targetY, freqY, dt);

        if (hasValidRoomBounds) {
            if (minX > maxX) minX = maxX = 0.5f * (minX + maxX);
            if (minY > maxY) minY = maxY = 0.5f * (minY + maxY);
            if ((baseCamX <= minX && cam.velX < 0.0f) || (baseCamX >= maxX && cam.velX > 0.0f)) cam.velX = 0.0f;
            if ((baseCamY <= minY && cam.velY < 0.0f) || (baseCamY >= maxY && cam.velY > 0.0f)) cam.velY = 0.0f;
            baseCamX = std::clamp(baseCamX, minX, maxX);
            baseCamY = std::clamp(baseCamY, minY, maxY);
        }

        // 7. Decay trauma & step unified 2nd-order kick + trauma displacement spring
        f32 shakeTargetX = 0.0f, shakeTargetY = 0.0f;
        if (cam.trauma_q16 > 0) {
            const u32 decay = static_cast<u32>(dt * 1.8f * 65535.0f);
            cam.trauma_q16  = (cam.trauma_q16 > decay) ? static_cast<u16>(cam.trauma_q16 - decay) : u16{0};

            if (cam.trauma_q16 > 0) {
                const f32 trauma   = static_cast<f32>(cam.trauma_q16) * (1.0f / 65535.0f);
                const f32 shakeMag = trauma * trauma * 0.35f;
                shakeTargetX = shakeMag * HashSignedNoise(step.TickIndex(), 1u);
                shakeTargetY = shakeMag * HashSignedNoise(step.TickIndex(), 2u);
            }
        }

        f32 kickVx = static_cast<f32>(cam.kickVelX_q8) * (1.0f / 256.0f);
        f32 kickVy = static_cast<f32>(cam.kickVelY_q8) * (1.0f / 256.0f);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetX, kickVx, shakeTargetX, 14.0f, dt);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetY, kickVy, shakeTargetY, 14.0f, dt);
        cam.kickVelX_q8 = static_cast<i16>(std::clamp(kickVx * 256.0f, -32767.0f, 32767.0f));
        cam.kickVelY_q8 = static_cast<i16>(std::clamp(kickVy * 256.0f, -32767.0f, 32767.0f));

        camLocal.x = baseCamX + cam.kickOffsetX;
        camLocal.y = baseCamY + cam.kickOffsetY;
    }

    if (anyCameraMoved) {
        world.PropagateTransforms();
    }
}

u64 GameplayPipeline::ComputeAuditHash(const world::World& world) const noexcept {
    u64 hash = world.ComputeAuditHash() ^ 0xCBF29CE484222325ULL;
    const auto mixU64 = [&hash](u64 v) noexcept {
        hash ^= v;
        hash *= 0x100000001B3ULL;
    };

    mixU64(m_activeFlagWordsMask);
    for (u64 m = m_activeFlagWordsMask; m != 0ULL; m &= (m - 1ULL)) {
        const u32 w = static_cast<u32>(std::countr_zero(m));
        mixU64((static_cast<u64>(w) << 32u) ^ m_worldFlags[w]);
    }
    mixU64(m_cueCount);
    for (u32 i = 0; i < m_cueCount; ++i) {
        const SimCue& c = m_cueBuffer[i];
        mixU64(c.assetId.value);
        mixU64((static_cast<u64>(CanonicalFloatBits(c.worldX)) << 32u) |
               static_cast<u64>(CanonicalFloatBits(c.worldY)));
        mixU64((static_cast<u64>(c.emitter.generation) << 48u) |
               (static_cast<u64>(c.emitter.index) << 32u) |
               (static_cast<u64>(c.magnitude_q16) << 16u) |
               static_cast<u64>(c.durationTicks));
        mixU64((static_cast<u64>(c.userTag) << 16u) |
               (static_cast<u64>(c.channel) << 8u) |
               static_cast<u64>(c.kind));
    }
    return hash;
}

} // namespace en::gameplay

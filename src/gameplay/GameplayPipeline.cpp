#include "gameplay/GameplayPipeline.h"

#include "assets/AssetBank.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace en::gameplay {

namespace {

// Exact closed-form critically damped 2nd-order harmonic oscillator via Halley-Pade rational approximation.
// Guarantees unconditional stability across arbitrary dt and slow-motion ramps with zero libm::expf calls.
constexpr void StepAnalyticCriticallyDampedSpring(
    f32& inoutPos, f32& inoutVel,
    f32 targetPos, f32 naturalFreqHz, f32 dt) noexcept {
    const f32 omega = naturalFreqHz * 6.283185307f;
    const f32 x = omega * dt;
    const f32 expTerm = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    const f32 diff = inoutPos - targetPos;
    const f32 temp = (inoutVel + omega * diff) * dt;
    inoutPos = targetPos + (diff + temp) * expTerm;
    inoutVel = (inoutVel - temp * omega) * expTerm;
}

[[nodiscard]] constexpr f32 HashSignedNoise(u64 tickIndex, u32 seedChannel) noexcept {
    u64 h = (tickIndex * 0x9E3779B97F4A7C15ULL) ^ (static_cast<u64>(seedChannel) * 0xBF58476D1CE4E5B9ULL);
    h = (h ^ (h >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27u)) * 0x94D049BB133111EBULL;
    h = h ^ (h >> 31u);
    const f32 norm01 = static_cast<f32>(h & 0xFFFFFFULL) * (1.0f / 16777215.0f);
    return norm01 * 2.0f - 1.0f;
}

} // namespace

GameplayPipeline::~GameplayPipeline() noexcept {
    Shutdown();
}

GameplayPipeline::GameplayPipeline(GameplayPipeline&& other) noexcept
    : m_arena(other.m_arena),
      m_arenaBytes(other.m_arenaBytes),
      m_policies(other.m_policies),
      m_kinematicCfg(other.m_kinematicCfg),
      m_spatial(other.m_spatial),
      m_hitScratch(other.m_hitScratch),
      m_cueBuffer(other.m_cueBuffer),
      m_cueCount(other.m_cueCount),
      m_worldFlags(other.m_worldFlags) {
    other.m_arena = nullptr;
    other.m_arenaBytes = 0;
    other.m_hitScratch = nullptr;
    other.m_cueBuffer = nullptr;
    other.m_cueCount = 0;
}

GameplayPipeline& GameplayPipeline::operator=(GameplayPipeline&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arena = other.m_arena;
        m_arenaBytes = other.m_arenaBytes;
        m_policies = other.m_policies;
        m_kinematicCfg = other.m_kinematicCfg;
        m_spatial = other.m_spatial;
        m_hitScratch = other.m_hitScratch;
        m_cueBuffer = other.m_cueBuffer;
        m_cueCount = other.m_cueCount;
        m_worldFlags = other.m_worldFlags;

        other.m_arena = nullptr;
        other.m_arenaBytes = 0;
        other.m_hitScratch = nullptr;
        other.m_cueBuffer = nullptr;
        other.m_cueCount = 0;
    }
    return *this;
}

Result<void, GameplayErrc> GameplayPipeline::Init(
    world::World& world,
    const GameplayPolicyTable& policies,
    const GameplayPipelineConfig& cfg) noexcept {
    Shutdown();

    const usize minBytes = SpatialIndex2D::RequiredArenaBytes() +
                           sizeof(ResolvedHitEvent) * kMaxHitsPerTick +
                           sizeof(SimCue) * kMaxCuesPerFrame + 512u;
    if (cfg.arenaBytes < minBytes) {
        return Err(GameplayErrc::InvalidConfig);
    }

    const usize alignedSize = (cfg.arenaBytes + 63u) & ~usize{63u};
    void* rawMem = ::operator new(alignedSize, std::align_val_t{64}, std::nothrow);
    if (rawMem == nullptr) {
        return Err(GameplayErrc::OutOfMemory);
    }

    m_arena = static_cast<u8*>(rawMem);
    m_arenaBytes = alignedSize;
    std::memset(m_arena, 0, m_arenaBytes);

    m_policies = policies;
    m_kinematicCfg = cfg.kinematicConfig;
    m_worldFlags.fill(0ULL);
    m_cueCount = 0;

    const usize spatialBytes = (SpatialIndex2D::RequiredArenaBytes() + 63u) & ~usize{63u};
    m_spatial.BindArena(std::span<u8>{m_arena, spatialBytes});

    u8* cursor = m_arena + spatialBytes;
    m_hitScratch = reinterpret_cast<ResolvedHitEvent*>(cursor);
    cursor += (sizeof(ResolvedHitEvent) * kMaxHitsPerTick + 63u) & ~usize{63u};

    m_cueBuffer = reinterpret_cast<SimCue*>(cursor);

    if (!world.RegisterComponentPool<KinematicMotor2D>(cfg.maxMotors) ||
        !world.RegisterComponentPool<KinematicCarrier2D>(cfg.maxCarriers) ||
        !world.RegisterComponentPool<MotionStack2D>(cfg.maxMotors) ||
        !world.RegisterComponentPool<StateMachine2D>(cfg.maxStateMachines) ||
        !world.RegisterComponentPool<HitboxRig2D>(cfg.maxHitboxRigs) ||
        !world.RegisterComponentPool<DamageReceiver2D>(cfg.maxDamageReceivers) ||
        !world.RegisterComponentPool<SafeGroundBreadcrumb2D>(cfg.maxBreadcrumbs) ||
        !world.RegisterComponentPool<CameraController2D>(cfg.maxCameras)) {
        Shutdown();
        return Err(GameplayErrc::WorldRegistrationFailed);
    }

    return Ok;
}

void GameplayPipeline::Shutdown() noexcept {
    m_spatial.ClearCollisionMap();
    if (m_arena != nullptr) {
        ::operator delete(m_arena, std::align_val_t{64}, std::nothrow);
        m_arena = nullptr;
    }
    m_arenaBytes = 0;
    m_hitScratch = nullptr;
    m_cueBuffer = nullptr;
    m_cueCount = 0;
}

Result<void, GameplayErrc> GameplayPipeline::SetActiveRoomCollision(
    const assets::AssetBank& bank,
    assets::BlobHandle collisionMapBlob) noexcept {
    const assets::BlobRecord& blob = bank.GetBlob(collisionMapBlob);
    return m_spatial.BindCollisionMapBlob(blob);
}

void GameplayPipeline::Tick(
    world::World& world,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const platform::input::TickInput& tickInput,
    platform::input::ActionBuffer& actionBuffer) noexcept {
    // Stage 1: Advance moving carriers and transfer displacements onto grounded riders.
    KinematicSolver2D::StepCarriersAndRiders(world, m_spatial, step);

    // Stage 2: Channel-gated animation step & frame-edge directive dispatch (0-tick hitbox latency).
    StateAndCombatSolver::GateFrozenAnimators(world, step, false);
    world.StepKinematicsAndAnimations(step, assetBank);
    StateAndCombatSolver::GateFrozenAnimators(world, step, true);
    StateAndCombatSolver::DispatchAnimationFrameDirectives(world, assetBank, step, *this);

    // Stage 3: Advance forgiveness/iframe timers and evaluate game policy callbacks against cancelMask.
    StateAndCombatSolver::StepStateMachinesAndPolicies(
        world, assetBank, step, tickInput, actionBuffer, m_policies, *this);

    // Stage 4: Rebuild spatial grid for soft pushboxes/carriers, solve swept kinematics, propagate transforms.
    m_spatial.RebuildDynamicGrid(world);
    KinematicSolver2D::StepMotorsAndMotionStacks(world, m_spatial, step, m_kinematicCfg);
    world.PropagateTransforms();

    // Stage 5: Rebuild dynamic combat grid at updated world transforms, gather multi-socket overlaps, arbitrate hits.
    m_spatial.RebuildDynamicGrid(world);
    const u32 hitCount = m_spatial.GatherCombatOverlaps(
        world, assetBank, std::span<ResolvedHitEvent>{m_hitScratch, kMaxHitsPerTick});
    if (hitCount > 0) {
        StateAndCombatSolver::ResolveCombatHits(
            world, step, std::span<const ResolvedHitEvent>{m_hitScratch, hitCount}, m_policies, *this);
    }

    // Stage 6: Update Ultimate Camera Director on TimeChannel::Camera (unfrozen during combat hitstop).
    StepCameraControllers(world, step);
}

void GameplayPipeline::StepCameraControllers(
    world::World& world,
    time::SimStepView step) noexcept {
    if (step.IsFrozen(time::TimeChannel::Camera)) {
        return;
    }
    const f32 dt = step.Dt(time::TimeChannel::Camera);
    if (dt <= 0.0f) {
        return;
    }

    for (auto [camId, cam] : world.View<CameraController2D>()) {
        if (cam.primaryTarget == world::kNullEntity || !world.IsAlive(cam.primaryTarget)) {
            continue;
        }

        const auto& priWt = world.GetWorldTransform(cam.primaryTarget);
        f32 targetX = priWt.x;
        f32 targetY = priWt.y;

        if (const auto* motor = world.TryGet<KinematicMotor2D>(cam.primaryTarget)) {
            if (HasContact(motor->contacts, ContactFlags::Ground)) {
                cam.latchedGroundY = priWt.y;
            }
        }

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockVerticalAirborne)) != 0) {
            const f32 deadzoneH = static_cast<f32>(cam.deadzoneHalfH_q4) * (1.0f / 16.0f);
            if (std::abs(priWt.y - cam.latchedGroundY) <= deadzoneH) {
                targetY = cam.latchedGroundY;
            } else if (priWt.y < cam.latchedGroundY - deadzoneH) {
                cam.latchedGroundY = priWt.y + deadzoneH;
                targetY = cam.latchedGroundY;
            } else {
                cam.latchedGroundY = priWt.y - deadzoneH;
                targetY = cam.latchedGroundY;
            }
        }

        if (const auto* motion = world.TryGet<MotionStack2D>(cam.primaryTarget)) {
            const f32 desiredLookX = static_cast<f32>(motion->FacingSign()) * 1.6f;
            const f32 curLookX = static_cast<f32>(cam.lookaheadX_q8) * (1.0f / 256.0f);
            const f32 nextLookX = curLookX + (desiredLookX - curLookX) * std::min(1.0f, dt * 6.0f);
            cam.lookaheadX_q8 = static_cast<i16>(nextLookX * 256.0f);
        }

        targetX += static_cast<f32>(cam.lookaheadX_q8) * (1.0f / 256.0f);
        targetY += static_cast<f32>(cam.peekOffsetY_q8) * (1.0f / 256.0f);

        if (cam.secondaryTarget != world::kNullEntity &&
            cam.secondaryWeight_q8 > 0 &&
            world.IsAlive(cam.secondaryTarget)) {
            const auto& secWt = world.GetWorldTransform(cam.secondaryTarget);
            const f32 w = static_cast<f32>(cam.secondaryWeight_q8) * (1.0f / 255.0f);
            targetX = targetX + (secWt.x - targetX) * w;
            targetY = targetY + (secWt.y - targetY) * w;
        }

        world::LocalTransform2D camLocal = world.GetLocalTransform(camId);
        const f32 freqX = std::max(0.5f, static_cast<f32>(cam.springFreqX_q4) * (1.0f / 16.0f));
        const f32 freqY = std::max(0.5f, static_cast<f32>(cam.springFreqY_q4) * (1.0f / 16.0f));

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockAxisX)) == 0) {
            StepAnalyticCriticallyDampedSpring(camLocal.x, cam.velX, targetX, freqX, dt);
        }
        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockAxisY)) == 0) {
            StepAnalyticCriticallyDampedSpring(camLocal.y, cam.velY, targetY, freqY, dt);
        }

        // Underdamped directional hit-kick spring recovery.
        f32 kickVx = static_cast<f32>(cam.kickVelX_q8) * (1.0f / 256.0f);
        f32 kickVy = static_cast<f32>(cam.kickVelY_q8) * (1.0f / 256.0f);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetX, kickVx, 0.0f, 12.0f, dt);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetY, kickVy, 0.0f, 12.0f, dt);
        cam.kickVelX_q8 = static_cast<i16>(std::clamp(kickVx * 256.0f, -32767.0f, 32767.0f));
        cam.kickVelY_q8 = static_cast<i16>(std::clamp(kickVy * 256.0f, -32767.0f, 32767.0f));

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::ClampToRoom)) != 0 &&
            cam.activeRoomBounds.halfW_q8 > 0 && cam.activeRoomBounds.halfH_q8 > 0) {
            f32 minX = cam.activeRoomBounds.OffsetX() - cam.activeRoomBounds.HalfW();
            f32 maxX = cam.activeRoomBounds.OffsetX() + cam.activeRoomBounds.HalfW();
            f32 minY = cam.activeRoomBounds.OffsetY() - cam.activeRoomBounds.HalfH();
            f32 maxY = cam.activeRoomBounds.OffsetY() + cam.activeRoomBounds.HalfH();

            if (cam.roomBlendTicks > 0) {
                --cam.roomBlendTicks;
                const f32 rawT = 1.0f - (static_cast<f32>(cam.roomBlendTicks) /
                                         static_cast<f32>( std::max<u16>(1, cam.roomBlendDuration) ));
                const f32 s = rawT * rawT * (3.0f - 2.0f * rawT);
                const f32 prevMinX = cam.prevRoomBounds.OffsetX() - cam.prevRoomBounds.HalfW();
                const f32 prevMaxX = cam.prevRoomBounds.OffsetX() + cam.prevRoomBounds.HalfW();
                const f32 prevMinY = cam.prevRoomBounds.OffsetY() - cam.prevRoomBounds.HalfH();
                const f32 prevMaxY = cam.prevRoomBounds.OffsetY() + cam.prevRoomBounds.HalfH();
                minX = prevMinX + (minX - prevMinX) * s;
                maxX = prevMaxX + (maxX - prevMaxX) * s;
                minY = prevMinY + (minY - prevMinY) * s;
                maxY = prevMaxY + (maxY - prevMaxY) * s;
            }

            camLocal.x = std::clamp(camLocal.x, minX, maxX);
            camLocal.y = std::clamp(camLocal.y, minY, maxY);
        }

        f32 shakeX = 0.0f;
        f32 shakeY = 0.0f;
        f32 shakeRot = 0.0f;
        if (cam.trauma_q16 > 0) {
            const f32 trauma = static_cast<f32>(cam.trauma_q16) * (1.0f / 65535.0f);
            const f32 shakeMag = trauma * trauma;
            shakeX   = shakeMag * 0.35f * HashSignedNoise(step.TickIndex(), 1u);
            shakeY   = shakeMag * 0.35f * HashSignedNoise(step.TickIndex(), 2u);
            shakeRot = shakeMag * 0.025f * HashSignedNoise(step.TickIndex(), 3u);

            const u32 decay = static_cast<u32>(dt * 1.8f * 65535.0f);
            cam.trauma_q16 = (cam.trauma_q16 > decay)
                ? static_cast<u16>(cam.trauma_q16 - decay)
                : u16{0};
        }

        camLocal.x += cam.kickOffsetX + shakeX;
        camLocal.y += cam.kickOffsetY + shakeY;
        camLocal.rotationRad = shakeRot;
        world.SetLocalTransform(camId, camLocal);
    }
}

u64 GameplayPipeline::ComputeAuditHash(const world::World& world) const noexcept {
    u64 hash = world.ComputeAuditHash() ^ 0xCBF29CE484222325ULL;
    const auto mixU64 = [&hash](u64 v) noexcept {
        hash ^= v;
        hash *= 0x100000001B3ULL;
    };

    for (u64 word : m_worldFlags) {
        mixU64(word);
    }
    mixU64(m_cueCount);
    for (u32 i = 0; i < m_cueCount; ++i) {
        mixU64(m_cueBuffer[i].assetId.value);
        mixU64(static_cast<u64>(m_cueBuffer[i].kind));
    }
    return hash;
}

} // namespace en::gameplay

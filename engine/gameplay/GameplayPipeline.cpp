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
    if (minB >= maxB) {
        return 0.5f * (minB + maxB);
    }
    const f32 span = maxB - minB;
    const f32 m    = std::min(cushionWidth, 0.25f * span);
    if (m <= 1e-4f) {
        return std::clamp(val, minB, maxB);
    }

    if (val < minB + m) {
        const f32 d = (minB + m) - val;
        const f32 t = std::min(1.0f, d / (2.0f * m));
        const f32 s = t * (2.0f - t);
        return (minB + m) - m * s;
    }
    if (val > maxB - m) {
        const f32 d = val - (maxB - m);
        const f32 t = std::min(1.0f, d / (2.0f * m));
        const f32 s = t * (2.0f - t);
        return (maxB - m) + m * s;
    }
    return val;
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
      m_invPixelsPerMeter(other.m_invPixelsPerMeter),
      m_spatial(other.m_spatial),
      m_hitScratch(other.m_hitScratch),
      m_cueBuffer(other.m_cueBuffer),
      m_cueCount(other.m_cueCount),
      m_worldFlags(other.m_worldFlags) {
    other.m_arena      = nullptr;
    other.m_arenaBytes = 0;
    other.m_hitScratch = nullptr;
    other.m_cueBuffer  = nullptr;
    other.m_cueCount   = 0;
}

GameplayPipeline& GameplayPipeline::operator=(GameplayPipeline&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arena             = other.m_arena;
        m_arenaBytes        = other.m_arenaBytes;
        m_policies          = other.m_policies;
        m_kinematicCfg      = other.m_kinematicCfg;
        m_invPixelsPerMeter = other.m_invPixelsPerMeter;
        m_spatial           = other.m_spatial;
        m_hitScratch        = other.m_hitScratch;
        m_cueBuffer         = other.m_cueBuffer;
        m_cueCount          = other.m_cueCount;
        m_worldFlags        = other.m_worldFlags;

        other.m_arena      = nullptr;
        other.m_arenaBytes = 0;
        other.m_hitScratch = nullptr;
        other.m_cueBuffer  = nullptr;
        other.m_cueCount   = 0;
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
    if (cfg.arenaBytes < minBytes) [[unlikely]] {
        return Err(GameplayErrc::InvalidConfig);
    }

    const usize alignedSize = (cfg.arenaBytes + 63u) & ~usize{63u};
    void* rawMem = ::operator new(alignedSize, std::align_val_t{64}, std::nothrow);
    if (rawMem == nullptr) [[unlikely]] {
        return Err(GameplayErrc::OutOfMemory);
    }

    m_arena      = static_cast<u8*>(rawMem);
    m_arenaBytes = alignedSize;
    std::memset(m_arena, 0, m_arenaBytes);

    m_policies          = policies;
    m_kinematicCfg      = cfg.kinematicConfig;
    m_invPixelsPerMeter = 1.0f / std::max(1.0f, cfg.pixelsPerMeter);
    m_worldFlags.fill(0ULL);
    m_cueCount = 0;

    const usize spatialBytes = (SpatialIndex2D::RequiredArenaBytes() + 63u) & ~usize{63u};
    m_spatial.BindArena(std::span<u8>{m_arena, spatialBytes});

    u8* cursor   = m_arena + spatialBytes;
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
    m_arenaBytes = 0;
    m_hitScratch = nullptr;
    m_cueBuffer  = nullptr;
    m_cueCount   = 0;
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
    const input::TickInput& tickInput,
    input::ActionBuffer& actionBuffer) noexcept {
    // Stage 1: Advance moving carriers and transfer displacements onto grounded riders.
    KinematicSolver2D::StepCarriersAndRiders(world, m_spatial, step);

    // Stage 2: Dedicated AnimationSubsystem & frame-edge directives (0-tick hitbox latency, native channel gating).
    AnimationSubsystem::StepAnimationsAndDirectives(world, assetBank, step, *this);

    // Stage 3: Advance forgiveness/iframe timers and evaluate game policy callbacks against cancelMask.
    StateAndCombatSolver::StepStateMachinesAndPolicies(
        world, assetBank, step, tickInput, actionBuffer, m_policies, *this);

    // Stage 4: Integrate ambient non-colliding Velocity2D, rebuild dynamic grid, solve swept motors, propagate transforms.
    world.StepAmbientVelocities(step);
    m_spatial.RebuildDynamicGrid(world);
    KinematicSolver2D::StepMotorsAndMotionStacks(world, m_spatial, step, m_kinematicCfg);
    world.PropagateTransforms();

    // Stage 5: Rebuild dynamic combat grid at updated world transforms, gather overlaps, arbitrate hits.
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

    const auto* __restrict worldTf = world.WorldTransforms().data();

    for (auto [camId, cam] : world.View<CameraController2D>()) {
        if (cam.primaryTarget == world::kNullEntity || !world.IsAlive(cam.primaryTarget)) {
            continue;
        }

        world::LocalTransform2D& camLocal = world.ModifyLocalTransformByIndex(camId.index);

        // Strip BOTH previous tick's kickOffset AND previous tick's trauma shake so noise never accumulates into baseCamX/Y
        f32 prevShakeX = 0.0f;
        f32 prevShakeY = 0.0f;
        if (cam.trauma_q16 > 0) {
            const f32 prevTrauma   = static_cast<f32>(cam.trauma_q16) * (1.0f / 65535.0f);
            const f32 prevShakeMag = prevTrauma * prevTrauma;
            const u64 prevTick     = step.TickIndex() > 0 ? (step.TickIndex() - 1ULL) : 0ULL;
            prevShakeX = prevShakeMag * 0.35f * HashSignedNoise(prevTick, 1u);
            prevShakeY = prevShakeMag * 0.35f * HashSignedNoise(prevTick, 2u);
        }

        f32 baseCamX = camLocal.x - cam.kickOffsetX - prevShakeX;
        f32 baseCamY = camLocal.y - cam.kickOffsetY - prevShakeY;

        const auto& priWt = worldTf[cam.primaryTarget.index];
        f32 targetX = priWt.x;
        f32 targetY = priWt.y;

        // 1. Horizontal Focus Box (Soft Deadzone + Hard Panic Window)
        const f32 deadzoneW = static_cast<f32>(cam.deadzoneHalfW_q4) * (1.0f / 16.0f);
        if (deadzoneW > 0.0f) {
            if (std::abs(priWt.x - baseCamX) <= deadzoneW) {
                targetX = baseCamX;
            } else if (priWt.x > baseCamX + deadzoneW) {
                targetX = priWt.x - deadzoneW;
            } else {
                targetX = priWt.x + deadzoneW;
            }
        }

        // 2. Ground-Latching, Airborne Vertical Deadzone & Fast-Fall Look-Down
        bool isGrounded = false;
        f32 actorVx = 0.0f;
        f32 actorVy = 0.0f;
        if (const auto* motor = world.TryGet<KinematicMotor2D>(cam.primaryTarget)) {
            isGrounded = HasContact(motor->contacts, ContactFlags::Ground);
            actorVx    = motor->vx;
            actorVy    = motor->vy;
            if (isGrounded) {
                cam.latchedGroundY = priWt.y;
            }
        }

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockVerticalAirborne)) != 0) {
            const f32 deadzoneH = static_cast<f32>(cam.deadzoneHalfH_q4) * (1.0f / 16.0f);
            if (isGrounded) {
                targetY = cam.latchedGroundY;
            } else if (std::abs(priWt.y - cam.latchedGroundY) <= deadzoneH) {
                targetY = cam.latchedGroundY;
            } else if (priWt.y < cam.latchedGroundY - deadzoneH) {
                cam.latchedGroundY = priWt.y + deadzoneH;
                targetY = cam.latchedGroundY;
            } else {
                cam.latchedGroundY = priWt.y - deadzoneH;
                targetY = cam.latchedGroundY;
            }

            if (!isGrounded && actorVy < -14.0f) {
                const f32 fallFactor = std::min(1.0f, (-actorVy - 14.0f) * (1.0f / 12.0f));
                targetY -= fallFactor * 2.2f;
            }
        }

        // 3. Turn-Confirmation Filtered Lookahead
        if (const auto* motion = world.TryGet<MotionStack2D>(cam.primaryTarget)) {
            const i8 facing = motion->FacingSign();
            const bool movingForward = (actorVx * static_cast<f32>(facing)) > 0.6f;

            if (movingForward) {
                if (cam.sustainedMoveTicks < 255u) {
                    ++cam.sustainedMoveTicks;
                }
            } else if (std::abs(actorVx) < 0.2f) {
                if (cam.sustainedMoveTicks > 0u) {
                    --cam.sustainedMoveTicks;
                }
            } else {
                cam.sustainedMoveTicks = 0u;
            }

            const f32 curLookX = static_cast<f32>(cam.lookaheadX_q8) * (1.0f / 256.0f);
            f32 desiredLookX   = curLookX;

            if (cam.sustainedMoveTicks >= 10u) {
                const f32 velScale = ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::VelocityLookahead)) != 0)
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

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::ClampToRoom)) != 0 &&
            cam.activeRoomBounds.halfW_q8 > 0 && cam.activeRoomBounds.halfH_q8 > 0) {
            hasValidRoomBounds = true;

            // Compute half-viewport extent in world meters from attached CameraRig2D
            f32 halfVpW = 0.0f;
            f32 halfVpH = 0.0f;
            if (const auto* rig = world.TryGet<world::CameraRig2D>(camId)) {
                const f32 invZoom = 1.0f / std::max(0.01f, rig->zoom);
                halfVpW = 0.5f * rig->viewport.w * m_invPixelsPerMeter * invZoom;
                halfVpH = 0.5f * rig->viewport.h * m_invPixelsPerMeter * invZoom;
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

            // If room is narrower/shorter than viewport, lock camera center to room midpoint
            if (minX > maxX) {
                const f32 midX = 0.5f * (minX + maxX);
                minX = maxX = midX;
            }
            if (minY > maxY) {
                const f32 midY = 0.5f * (minY + maxY);
                minY = maxY = midY;
            }

            if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::SoftBoundaryCushion)) != 0) {
                targetX = SoftCushionClamp(targetX, minX, maxX, 2.0f);
                targetY = SoftCushionClamp(targetY, minY, maxY, 1.5f);
            } else {
                targetX = std::clamp(targetX, minX, maxX);
                targetY = std::clamp(targetY, minY, maxY);
            }
        }

        // 6. Step Analytic Critically-Damped Springs on Base Camera Coordinates
        const f32 freqX = std::max(0.5f, static_cast<f32>(cam.springFreqX_q4) * (1.0f / 16.0f));
        const f32 freqY = std::max(0.5f, static_cast<f32>(cam.springFreqY_q4) * (1.0f / 16.0f));

        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockAxisX)) == 0) {
            StepAnalyticCriticallyDampedSpring(baseCamX, cam.velX, targetX, freqX, dt);
        }
        if ((cam.cameraFlags & static_cast<u8>(CameraCtrlFlags::LockAxisY)) == 0) {
            StepAnalyticCriticallyDampedSpring(baseCamY, cam.velY, targetY, freqY, dt);
        }

        if (hasValidRoomBounds) {
            if (baseCamX <= minX && cam.velX < 0.0f) { baseCamX = minX; cam.velX = 0.0f; }
            if (baseCamX >= maxX && cam.velX > 0.0f) { baseCamX = maxX; cam.velX = 0.0f; }
            if (baseCamY <= minY && cam.velY < 0.0f) { baseCamY = minY; cam.velY = 0.0f; }
            if (baseCamY >= maxY && cam.velY > 0.0f) { baseCamY = maxY; cam.velY = 0.0f; }
        }

        // 7. Directional combat hit-kick spring recovery
        f32 kickVx = static_cast<f32>(cam.kickVelX_q8) * (1.0f / 256.0f);
        f32 kickVy = static_cast<f32>(cam.kickVelY_q8) * (1.0f / 256.0f);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetX, kickVx, 0.0f, 12.0f, dt);
        StepAnalyticCriticallyDampedSpring(cam.kickOffsetY, kickVy, 0.0f, 12.0f, dt);
        cam.kickVelX_q8 = static_cast<i16>(std::clamp(kickVx * 256.0f, -32767.0f, 32767.0f));
        cam.kickVelY_q8 = static_cast<i16>(std::clamp(kickVy * 256.0f, -32767.0f, 32767.0f));

        // 8. Decay trauma & compute current tick's deterministic hash-noise shake
        f32 shakeX   = 0.0f;
        f32 shakeY   = 0.0f;
        f32 shakeRot = 0.0f;
        if (cam.trauma_q16 > 0) {
            const u32 decay = static_cast<u32>(dt * 1.8f * 65535.0f);
            cam.trauma_q16 = (cam.trauma_q16 > decay)
                ? static_cast<u16>(cam.trauma_q16 - decay)
                : u16{0};

            if (cam.trauma_q16 > 0) {
                const f32 trauma   = static_cast<f32>(cam.trauma_q16) * (1.0f / 65535.0f);
                const f32 shakeMag = trauma * trauma;
                shakeX   = shakeMag * 0.35f * HashSignedNoise(step.TickIndex(), 1u);
                shakeY   = shakeMag * 0.35f * HashSignedNoise(step.TickIndex(), 2u);
                shakeRot = shakeMag * 0.025f * HashSignedNoise(step.TickIndex(), 3u);
            }
        }

        camLocal.x           = baseCamX + cam.kickOffsetX + shakeX;
        camLocal.y           = baseCamY + cam.kickOffsetY + shakeY;
        camLocal.rotationRad = shakeRot;
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

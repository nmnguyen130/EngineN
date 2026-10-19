#include "debug/DebugInspector.h"

#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "world/World.h"

#include <cmath>

namespace en::debug {

namespace {

inline constexpr f32 kInvQ8      = 1.0f / 256.0f;
inline constexpr u64 kVelBit     = 1ULL << world::Velocity2D::kTypeId;
inline constexpr u64 kMotorBit   = 1ULL << gameplay::KinematicMotor2D::kTypeId;
inline constexpr u64 kCarrierBit = 1ULL << gameplay::KinematicCarrier2D::kTypeId;
inline constexpr u64 kFsmBit     = 1ULL << gameplay::StateMachine2D::kTypeId;
inline constexpr u64 kRigBit     = 1ULL << gameplay::HitboxRig2D::kTypeId;
inline constexpr u64 kBcBit      = 1ULL << gameplay::SafeGroundBreadcrumb2D::kTypeId;
inline constexpr u64 kCamCtrlBit = 1ULL << gameplay::CameraController2D::kTypeId;

template <typename T>
[[nodiscard]] constexpr const T* TryGetDirect(const world::World& world, u16 idx, u64 mask) noexcept {
    return ((mask & (1ULL << T::kTypeId)) != 0ULL)
        ? &world.GetPool<T>().DirectAtEntityIndex(idx)
        : nullptr;
}

[[nodiscard]] constexpr renderer::FRect UnpackBoxQ8(f32 wx, f32 wy, i16 ox, i16 oy, i16 hw, i16 hh) noexcept {
    const f32 cx = wx + static_cast<f32>(ox) * kInvQ8;
    const f32 cy = wy + static_cast<f32>(oy) * kInvQ8;
    const f32 w  = static_cast<f32>(hw) * (kInvQ8 * 2.0f);
    const f32 h  = static_cast<f32>(hh) * (kInvQ8 * 2.0f);
    return {cx - w * 0.5f, cy - h * 0.5f, w, h};
}

[[nodiscard]] constexpr renderer::FRect UnpackBoxQ8(f32 wx, f32 wy, const gameplay::CompactAabb2D& b) noexcept {
    return UnpackBoxQ8(wx, wy, b.offsetX_q8, b.offsetY_q8, b.halfW_q8, b.halfH_q8);
}

[[nodiscard]] constexpr renderer::FRect UnpackBoxQ8(f32 wx, f32 wy, const std::array<i16, 4>& q8) noexcept {
    return UnpackBoxQ8(wx, wy, q8[0], q8[1], q8[2], q8[3]);
}

struct ActiveCameraView {
    f32 centerX{0.0f};
    f32 centerY{0.0f};
    f32 zoom{1.0f};
    renderer::FRect viewport{};
    renderer::FRect worldBounds{};
};

[[nodiscard]] ActiveCameraView ResolveActiveCamera(const world::World& world,
                                                   renderer::ViewportExtent viewportSize) noexcept {
    ActiveCameraView out{};
    out.viewport = {0.0f, 0.0f, static_cast<f32>(viewportSize.w), static_cast<f32>(viewportSize.h)};
    out.zoom     = 1.0f;

    for (const auto [id, rig] : world.View<world::CameraRig2D>()) {
        const u16 idx = id.index;
        if (rig.active && !rig.screenSpace && rig.viewSlot == 0) {
            const world::WorldTransform2D& wt = world.GetWorldByIndex(idx);
            out.centerX = wt.x;
            out.centerY = wt.y;
            out.zoom    = rig.zoom > 0.01f ? rig.zoom : 1.0f;
            if (rig.viewport.w > 0.0f && rig.viewport.h > 0.0f) {
                out.viewport = rig.viewport;
            }
            break;
        }
    }

    const f32 invZoom = 1.0f / out.zoom;
    const f32 halfW   = (out.viewport.w * 0.5f) * invZoom;
    const f32 halfH   = (out.viewport.h * 0.5f) * invZoom;
    out.worldBounds   = {out.centerX - halfW, out.centerY - halfH, halfW * 2.0f, halfH * 2.0f};
    return out;
}

} // namespace

void DebugInspector::BindArena(std::span<ActorTickRecord, kHistoryTicks> tickRing) noexcept {
    m_tickRing = tickRing;
    Clear();
}

void DebugInspector::Clear() noexcept {
    m_pinnedEntity  = world::kNullEntity;
    m_writeCursor   = 0;
    m_scrubOffset   = 0;
    m_recordedCount = 0;
}

void DebugInspector::RecordActorTick(const world::World& world,
                                     u64 tickIndex,
                                     const input::TickInput& tickInput,
                                     const input::ActionBuffer& actionBuffer) noexcept {
    if (m_tickRing.empty()) {
        return;
    }

    world::EntityId target = m_pinnedEntity;
    if (!world.IsAlive(target)) {
        target = world::kNullEntity;
        for (const auto [id, _] : world.View<gameplay::KinematicMotor2D>()) {
            if (target.index == 0) {
                target = id;
            }
            if ((world.GetComponentMaskByIndex(id.index) & kFsmBit) != 0ULL) {
                target = id;
                break;
            }
        }
    }

    ActorTickRecord rec{};
    rec.tickIndex        = static_cast<u32>(tickIndex);
    rec.entityId         = target;
    rec.inputHeld        = tickInput.held;
    rec.inputPressed     = tickInput.pressed;
    rec.inputReleased    = tickInput.released;
    rec.actionBufferMask = actionBuffer.PendingPressMask();
    rec.axisX            = tickInput.axis[0];
    rec.axisY            = tickInput.axis[1];

    if (world.IsAlive(target)) {
        const u16 idx  = target.index;
        const u64 mask = world.GetComponentMaskByIndex(idx);
        const world::WorldTransform2D& wt = world.GetWorldByIndex(idx);
        rec.worldX = wt.x;
        rec.worldY = wt.y;

        if (const auto* motor = TryGetDirect<gameplay::KinematicMotor2D>(world, idx, mask)) {
            rec.velX         = motor->vx;
            rec.velY         = motor->vy;
            rec.contactFlags = static_cast<u8>(motor->contacts);
        } else if (const auto* vel = TryGetDirect<world::Velocity2D>(world, idx, mask)) {
            rec.velX = vel->vx;
            rec.velY = vel->vy;
        }

        if (const auto* fsm = TryGetDirect<gameplay::StateMachine2D>(world, idx, mask)) {
            rec.cancelMask = fsm->cancelMask;
            rec.stateTicks = fsm->stateTicks;
            rec.stateId    = fsm->stateId;
            rec.animClipId = fsm->animClipId;
            rec.animFrame  = fsm->lastAnimFrame;
        }

        if (const auto* rig = TryGetDirect<gameplay::HitboxRig2D>(world, idx, mask)) {
            rec.hurtboxQ8      = {rig->hurtbox.offsetX_q8, rig->hurtbox.offsetY_q8,
                                  static_cast<i16>(rig->hurtbox.halfW_q8), static_cast<i16>(rig->hurtbox.halfH_q8)};
            rec.hitboxQ8       = {rig->primaryHitbox.offsetX_q8, rig->primaryHitbox.offsetY_q8,
                                  static_cast<i16>(rig->primaryHitbox.halfW_q8), static_cast<i16>(rig->primaryHitbox.halfH_q8)};
            rec.activeSliceIdx = rig->activeSliceIndex;
        }

        if (const auto* dmg = TryGetDirect<gameplay::DamageReceiver2D>(world, idx, mask)) {
            rec.hp          = dmg->hp;
            rec.iframeTicks = dmg->iframeTicks;
        }
    }

    m_tickRing[m_writeCursor] = rec;
    m_writeCursor = static_cast<u8>((m_writeCursor + 1u) & (kHistoryTicks - 1u));
    if (m_recordedCount < kHistoryTicks) {
        ++m_recordedCount;
    }
}

world::EntityId DebugInspector::PickEntityAtScreenPos(const world::World& world,
                                                      input::Vec2 mousePos,
                                                      renderer::ViewportExtent viewportSize) noexcept {
    const ActiveCameraView cam = ResolveActiveCamera(world, viewportSize);
    const f32 relX   = mousePos.x - (cam.viewport.x + cam.viewport.w * 0.5f);
    const f32 relY   = mousePos.y - (cam.viewport.y + cam.viewport.h * 0.5f);
    const f32 worldX = cam.centerX + relX / cam.zoom;
    const f32 worldY = cam.centerY + relY / cam.zoom;

    world::EntityId bestEntity = world::kNullEntity;
    f32 bestArea = 1e30f;

    for (const auto [id, _] : world.View<world::LocalTransform2D>()) {
        const u16 idx  = id.index;
        const u64 mask = world.GetComponentMaskByIndex(idx);
        const world::WorldTransform2D& wt = world.GetWorldByIndex(idx);
        renderer::FRect bounds{wt.x - 8.0f, wt.y - 8.0f, 16.0f, 16.0f};

        if (const auto* rig = TryGetDirect<gameplay::HitboxRig2D>(world, idx, mask);
            rig != nullptr && rig->hurtbox.halfW_q8 > 0) {
            bounds = UnpackBoxQ8(wt.x, wt.y, rig->hurtbox);
        } else if (const auto* motor = TryGetDirect<gameplay::KinematicMotor2D>(world, idx, mask);
                   motor != nullptr && motor->bounds.halfW_q8 > 0) {
            bounds = UnpackBoxQ8(wt.x, wt.y, motor->bounds);
        }

        if (worldX >= bounds.x && worldX <= bounds.x + bounds.w &&
            worldY >= bounds.y && worldY <= bounds.y + bounds.h) {
            const f32 area = bounds.w * bounds.h;
            if (area < bestArea) {
                bestArea   = area;
                bestEntity = id;
            }
        }
    }

    m_pinnedEntity = bestEntity;
    return bestEntity;
}

void DebugInspector::EmitWorldAndInspectorOverlays(OverlayFlags flags,
                                                   const DebugEmitOverlayContext& ctx,
                                                   DebugBatcher& worldBatcher,
                                                   DebugBatcher& hudBatcher) const noexcept {
    if (flags == OverlayFlags::None) {
        return;
    }

    const ActiveCameraView cam = ResolveActiveCamera(ctx.world, ctx.viewportSize);
    const f32 cullMinX = cam.worldBounds.x - 64.0f;
    const f32 cullMinY = cam.worldBounds.y - 64.0f;
    const f32 cullMaxX = cam.worldBounds.x + cam.worldBounds.w + 64.0f;
    const f32 cullMaxY = cam.worldBounds.y + cam.worldBounds.h + 64.0f;

    if (HasOverlay(flags, OverlayFlags::DynamicSpatialGrid)) {
        constexpr f32 kCellSize = 64.0f;
        const f32 startX = std::floor(cam.worldBounds.x / kCellSize) * kCellSize;
        const f32 startY = std::floor(cam.worldBounds.y / kCellSize) * kCellSize;
        const f32 endX   = cam.worldBounds.x + cam.worldBounds.w;
        const f32 endY   = cam.worldBounds.y + cam.worldBounds.h;

        for (f32 gx = startX; gx <= endX; gx += kCellSize) {
            worldBatcher.PushLine(gx, cam.worldBounds.y, gx, endY, 1.0f,
                                  renderer::Color::FromRGBA8(0x6644AA38u));
        }
        for (f32 gy = startY; gy <= endY; gy += kCellSize) {
            worldBatcher.PushLine(cam.worldBounds.x, gy, endX, gy, 1.0f,
                                  renderer::Color::FromRGBA8(0x6644AA38u));
        }
    }

    if (HasOverlay(flags, OverlayFlags::AudioEmitters)) {
        for (const DebugAudioEmitter& em : ctx.audioEmitters) {
            const renderer::Color col = (em.occlusionFactor < 0.95f)
                ? renderer::Color::FromRGBA8(0xFF8833CCu)
                : renderer::Color::FromRGBA8(0x33EEFFCCu);
            if (em.maxDist > 0.0f) {
                worldBatcher.PushCircleOutline(em.worldX, em.worldY, em.maxDist, 24, 1.0f, col);
            }
            worldBatcher.PushBoxOutline({em.worldX - 2.5f, em.worldY - 2.5f, 5.0f, 5.0f}, 1.0f, col);
        }
    }

    const bool drawTransforms  = HasOverlay(flags, OverlayFlags::SpatialTransforms);
    const bool drawKinematics  = HasOverlay(flags, OverlayFlags::KinematicBounds);
    const bool drawVelocities  = HasOverlay(flags, OverlayFlags::VelocityVectors);
    const bool drawCombat      = HasOverlay(flags, OverlayFlags::CombatHitboxes);
    const bool drawBreadcrumbs = HasOverlay(flags, OverlayFlags::SafeBreadcrumbs);
    const bool drawCameraRigs  = HasOverlay(flags, OverlayFlags::CameraRigOverlay);

    u64 activeCompMask = drawTransforms ? ~0ULL : 0ULL;
    if (drawKinematics)  activeCompMask |= (kMotorBit | kCarrierBit);
    if (drawVelocities)  activeCompMask |= (kMotorBit | kVelBit);
    if (drawCombat)      activeCompMask |= kRigBit;
    if (drawBreadcrumbs) activeCompMask |= kBcBit;
    if (drawCameraRigs)  activeCompMask |= kCamCtrlBit;

    if (activeCompMask != 0ULL || HasOverlay(flags, OverlayFlags::EntityInspector)) {
        for (const auto [id, _] : ctx.world.View<world::LocalTransform2D>()) {
            const u16 idx       = id.index;
            const u64 compMask  = ctx.world.GetComponentMaskByIndex(idx);
            const bool isPinned = (id == m_pinnedEntity);
            if (((compMask & activeCompMask) == 0ULL) && !isPinned) {
                continue;
            }

            const world::WorldTransform2D& wt = ctx.world.GetWorldByIndex(idx);
            const f32 wx = wt.x;
            const f32 wy = wt.y;

            const bool inFrustum = (wx >= cullMinX && wx <= cullMaxX && wy >= cullMinY && wy <= cullMaxY);
            if (!inFrustum && !drawCameraRigs && !isPinned) {
                continue;
            }

            if (drawTransforms && inFrustum) {
                worldBatcher.PushLine(wx, wy, wx + wt.cosRot * 8.0f, wy + wt.sinRot * 8.0f,
                                      1.0f, renderer::Color::FromRGBA8(0xFF4444FFu));
                worldBatcher.PushLine(wx, wy, wx - wt.sinRot * 8.0f, wy + wt.cosRot * 8.0f,
                                      1.0f, renderer::Color::FromRGBA8(0x44FF44FFu));
            }

            if (inFrustum && (drawKinematics || drawVelocities)) {
                if (const auto* motor = TryGetDirect<gameplay::KinematicMotor2D>(ctx.world, idx, compMask)) {
                    if (drawKinematics && motor->bounds.halfW_q8 > 0) {
                        const renderer::Color col = (static_cast<u8>(motor->contacts) & 1u) != 0
                            ? renderer::Color::FromRGBA8(0x44FF88FFu)
                            : renderer::Color::FromRGBA8(0x44CCFFFFu);
                        worldBatcher.PushBoxOutline(UnpackBoxQ8(wx, wy, motor->bounds), 1.0f, col);
                    }
                    if (drawVelocities && (std::abs(motor->vx) > 0.5f || std::abs(motor->vy) > 0.5f)) {
                        worldBatcher.PushArrow(wx, wy, wx + motor->vx * 0.1f, wy + motor->vy * 0.1f,
                                               4.0f, 1.0f, renderer::Color::FromRGBA8(0xFFFF44FFu));
                    }
                } else if (drawVelocities) {
                    if (const auto* vel = TryGetDirect<world::Velocity2D>(ctx.world, idx, compMask)) {
                        if (std::abs(vel->vx) > 0.5f || std::abs(vel->vy) > 0.5f) {
                            worldBatcher.PushArrow(wx, wy, wx + vel->vx * 0.1f, wy + vel->vy * 0.1f,
                                                   4.0f, 1.0f, renderer::Color::FromRGBA8(0xFFCC44FFu));
                        }
                    }
                }

                if (drawKinematics) {
                    if (const auto* carrier = TryGetDirect<gameplay::KinematicCarrier2D>(ctx.world, idx, compMask)) {
                        worldBatcher.PushBoxOutline(UnpackBoxQ8(wx, wy, carrier->bounds), 1.5f,
                                                    renderer::Color::FromRGBA8(0xFF9922FFu));
                    }
                }
            }

            if (drawCombat && inFrustum) {
                if (const auto* rig = TryGetDirect<gameplay::HitboxRig2D>(ctx.world, idx, compMask)) {
                    const auto* dmg      = TryGetDirect<gameplay::DamageReceiver2D>(ctx.world, idx, compMask);
                    const bool hasIframe = (dmg != nullptr && dmg->iframeTicks > 0);
                    if (rig->hurtbox.halfW_q8 > 0) {
                        const renderer::Color col = hasIframe
                            ? renderer::Color::FromRGBA8(0xFFFFFFFFu)
                            : renderer::Color::FromRGBA8(0x22FF66CCu);
                        worldBatcher.PushBoxOutline(UnpackBoxQ8(wx, wy, rig->hurtbox), 1.0f, col);
                    }
                    if (rig->activeSliceIndex != 0xFFu && rig->primaryHitbox.halfW_q8 > 0) {
                        const renderer::FRect atk = UnpackBoxQ8(wx, wy, rig->primaryHitbox);
                        worldBatcher.PushSolidRect(atk, renderer::Color::FromRGBA8(0xFF224444u));
                        worldBatcher.PushBoxOutline(atk, 1.5f, renderer::Color::FromRGBA8(0xFF2244FFu));
                    }
                }
            }

            if (drawBreadcrumbs && inFrustum) {
                if (const auto* bc = TryGetDirect<gameplay::SafeGroundBreadcrumb2D>(ctx.world, idx, compMask)) {
                    worldBatcher.PushLine(wx, wy, bc->safeX, bc->safeY, 1.0f,
                                          renderer::Color::FromRGBA8(0x44FFAA88u));
                    worldBatcher.PushBoxOutline({bc->safeX - 3.0f, bc->safeY - 3.0f, 6.0f, 6.0f},
                                                1.0f, renderer::Color::FromRGBA8(0x00FF88FFu));
                }
            }

            if (drawCameraRigs) {
                if (const auto* ctrl = TryGetDirect<gameplay::CameraController2D>(ctx.world, idx, compMask)) {
                    const f32 dzW = static_cast<f32>(ctrl->deadzoneHalfW_q4) * (1.0f / 16.0f);
                    const f32 dzH = static_cast<f32>(ctrl->deadzoneHalfH_q4) * (1.0f / 16.0f);
                    worldBatcher.PushBoxOutline({wx - dzW, wy - dzH, dzW * 2.0f, dzH * 2.0f},
                                                1.0f, renderer::Color::FromRGBA8(0xFF66FFAAu));
                }
            }

            if (isPinned) {
                worldBatcher.PushBoxOutline({wx - 12.0f, wy - 12.0f, 24.0f, 24.0f},
                                            1.5f, renderer::Color::FromRGBA8(0xFFFF00FFu));
            }
        }
    }

    if (HasOverlay(flags, OverlayFlags::EntityInspector)) {
        EmitEntityInspectorCard(ctx.world, ctx.viewportSize, hudBatcher);
    }
    if (HasOverlay(flags, OverlayFlags::FlightScrubber)) {
        EmitFlightScrubberOverlay(ctx.viewportSize, worldBatcher, hudBatcher);
    }
    if (HasOverlay(flags, OverlayFlags::InputViewerHud)) {
        EmitInputViewerOverlay(ctx.frameInput, ctx.viewportSize, hudBatcher);
    }
}

void DebugInspector::EmitEntityInspectorCard(const world::World& world,
                                             renderer::ViewportExtent viewportSize,
                                             DebugBatcher& hudBatcher) const noexcept {
    const f32 cardW = 248.0f;
    const f32 cardH = 128.0f;
    const f32 cardX = static_cast<f32>(viewportSize.w) - cardW - 12.0f;
    const f32 cardY = 12.0f;

    hudBatcher.PushSolidRect({cardX, cardY, cardW, cardH}, renderer::Color::FromRGBA8(0x10141EE0u));
    hudBatcher.PushBoxOutline({cardX, cardY, cardW, cardH}, 1.0f, renderer::Color::FromRGBA8(0x4488CCFFu));

    const f32 tx = cardX + 8.0f;
    f32 ty       = cardY + 8.0f;

    if (!world.IsAlive(m_pinnedEntity)) {
        hudBatcher.PushText(tx, ty, "INSPECTOR: <Click Entity>", renderer::Color::FromRGBA8(0xAACCFFFFu));
        return;
    }

    const u16 idx  = m_pinnedEntity.index;
    const u64 mask = world.GetComponentMaskByIndex(idx);

    DebugTextLine line{};
    line.Str("ENTITY #").I64(idx).Str(" (Gen ").I64(m_pinnedEntity.generation).Char(')');
    hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xFFFF66FFu));
    ty += 12.0f;

    const world::WorldTransform2D& wt = world.GetWorldByIndex(idx);
    line.Clear();
    line.Str("Pos: (").F32(wt.x).Str(", ").F32(wt.y).Char(')');
    hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xE0E8F0FFu));
    ty += 11.0f;

    if (const auto* motor = TryGetDirect<gameplay::KinematicMotor2D>(world, idx, mask)) {
        line.Clear();
        line.Str("Vel: (").F32(motor->vx).Str(", ").F32(motor->vy).Str(") C:0x")
            .U64Hex(static_cast<u8>(motor->contacts), 2);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0x88EEFFFFu));
        ty += 11.0f;
    }

    if (const auto* fsm = TryGetDirect<gameplay::StateMachine2D>(world, idx, mask)) {
        line.Clear();
        line.Str("FSM: St=").I64(fsm->stateId)
            .Str(" Clip=").I64(fsm->animClipId)
            .Str(" Frm=").I64(fsm->lastAnimFrame)
            .Str(" T=").I64(fsm->stateTicks);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0x88FFAAFFu));
        ty += 11.0f;

        line.Clear();
        line.Str("CancelMask: 0x").U64Hex(fsm->cancelMask, 4);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xCCCCCCFFu));
        ty += 11.0f;
    }

    if (const auto* dmg = TryGetDirect<gameplay::DamageReceiver2D>(world, idx, mask)) {
        line.Clear();
        line.Str("HP: ").I64(dmg->hp).Char('/').I64(dmg->maxHp)
            .Str("  IFrames: ").I64(dmg->iframeTicks);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xFF99AAFFu));
    }
}

void DebugInspector::EmitFlightScrubberOverlay(renderer::ViewportExtent viewportSize,
                                               DebugBatcher& worldBatcher,
                                               DebugBatcher& hudBatcher) const noexcept {
    if (m_recordedCount == 0 || m_tickRing.empty()) {
        return;
    }

    const u8 off  = std::min<u8>(m_scrubOffset, static_cast<u8>(m_recordedCount - 1));
    const u8 slot = static_cast<u8>((m_writeCursor + kHistoryTicks - 1u - off) & (kHistoryTicks - 1u));
    const ActorTickRecord& rec = m_tickRing[slot];

    if (rec.entityId.index != 0) {
        if (rec.hurtboxQ8[2] > 0) {
            worldBatcher.PushBoxOutline(UnpackBoxQ8(rec.worldX, rec.worldY, rec.hurtboxQ8),
                                        1.5f, renderer::Color::FromRGBA8(0x00FFFFCCu));
        }
        if (rec.activeSliceIdx != 0xFFu && rec.hitboxQ8[2] > 0) {
            worldBatcher.PushBoxOutline(UnpackBoxQ8(rec.worldX, rec.worldY, rec.hitboxQ8),
                                        1.5f, renderer::Color::FromRGBA8(0xFF00FFCCu));
        }
    }

    const f32 barW = 360.0f;
    const f32 barH = 44.0f;
    const f32 barX = (static_cast<f32>(viewportSize.w) - barW) * 0.5f;
    const f32 barY = static_cast<f32>(viewportSize.h) - barH - 12.0f;

    hudBatcher.PushSolidRect({barX, barY, barW, barH}, renderer::Color::FromRGBA8(0x10141EE0u));
    hudBatcher.PushBoxOutline({barX, barY, barW, barH}, 1.0f, renderer::Color::FromRGBA8(0x66AAFFFFu));

    DebugTextLine line{};
    line.Str("SCRUB [-").I64(off).Str("t] Tick:").I64(rec.tickIndex)
        .Str(" St:").I64(rec.stateId)
        .Str(" Frm:").I64(rec.animFrame)
        .Str(" In:0x").U64Hex(rec.inputHeld, 4);
    hudBatcher.PushText(barX + 8.0f, barY + 6.0f, line.View(), renderer::Color::FromRGBA8(0xFFFFFFFFu));

    const f32 cellW = (barW - 16.0f) / static_cast<f32>(kHistoryTicks);
    for (u8 i = 0; i < m_recordedCount; ++i) {
        const u8 histSlot = static_cast<u8>((m_writeCursor + kHistoryTicks - 1u - i) & (kHistoryTicks - 1u));
        const ActorTickRecord& item = m_tickRing[histSlot];
        const f32 cx = barX + barW - 8.0f - static_cast<f32>(i + 1u) * cellW;
        const renderer::Color col = (i == off)
            ? renderer::Color::FromRGBA8(0xFFFF00FFu)
            : (item.activeSliceIdx != 0xFFu
                ? renderer::Color::FromRGBA8(0xFF4466FFu)
                : renderer::Color::FromRGBA8(0x336699FFu));
        hudBatcher.PushSolidRect({cx, barY + 22.0f, cellW - 1.0f, 14.0f}, col);
    }
}

void DebugInspector::EmitInputViewerOverlay(const input::FrameInput& frameInput,
                                            renderer::ViewportExtent viewportSize,
                                            DebugBatcher& hudBatcher) const noexcept {
    const f32 x = 12.0f;
    const f32 y = static_cast<f32>(viewportSize.h) - 52.0f;
    const f32 w = 224.0f;
    const f32 h = 40.0f;

    hudBatcher.PushSolidRect({x, y, w, h}, renderer::Color::FromRGBA8(0x10141EE0u));
    hudBatcher.PushBoxOutline({x, y, w, h}, 1.0f, renderer::Color::FromRGBA8(0x557799FFu));

    DebugTextLine line{};
    line.Str("INPUT Held:0x").U64Hex(frameInput.held, 4)
        .Str(" Edge:0x").U64Hex(frameInput.pressed, 4);
    hudBatcher.PushText(x + 6.0f, y + 5.0f, line.View(), renderer::Color::FromRGBA8(0xDDE8FFFFu));

    for (u32 bit = 0; bit < 16; ++bit) {
        const bool isHeld = (frameInput.held & (1u << bit)) != 0;
        const bool isEdge = (frameInput.pressed & (1u << bit)) != 0;
        const renderer::Color col = isEdge
            ? renderer::Color::FromRGBA8(0xFFFF44FFu)
            : (isHeld ? renderer::Color::FromRGBA8(0x44FF88FFu) : renderer::Color::FromRGBA8(0x283040FFu));
        hudBatcher.PushSolidRect({x + 6.0f + static_cast<f32>(bit) * 13.0f, y + 20.0f, 10.0f, 12.0f}, col);
    }
}

u64 DebugInspector::ComputeAuditHash() const noexcept {
    u64 h = MixAuditHash(0xA7C0159EC701ull, m_recordedCount);
    h = MixAuditHash(h, (static_cast<u64>(m_pinnedEntity.generation) << 16) | m_pinnedEntity.index);
    for (u8 i = 0; i < m_recordedCount; ++i) {
        const u8 slot = static_cast<u8>((m_writeCursor + kHistoryTicks - 1u - i) & (kHistoryTicks - 1u));
        const ActorTickRecord& r = m_tickRing[slot];
        h = MixAuditHash(h, (static_cast<u64>(r.tickIndex) << 32) |
                            (static_cast<u64>(r.stateId) << 16) | r.inputHeld);
    }
    return h;
}

} // namespace en::debug

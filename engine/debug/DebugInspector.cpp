#include "debug/DebugInspector.h"

#include "gameplay/GameplayTypes.h"
#include "input/ActionBuffer.h"
#include "renderer/RenderList.h"
#include "world/World.h"

#include <cmath>

namespace en::debug {

namespace {

inline constexpr f32 kInvQ8 = 1.0f / 256.0f;

[[nodiscard]] constexpr renderer::FRect UnpackWorldBox(f32 wx, f32 wy, const gameplay::CompactAabb2D& box) noexcept {
    const f32 cx = wx + static_cast<f32>(box.offsetX_q8) * kInvQ8;
    const f32 cy = wy + static_cast<f32>(box.offsetY_q8) * kInvQ8;
    const f32 hw = static_cast<f32>(box.halfW_q8) * kInvQ8;
    const f32 hh = static_cast<f32>(box.halfH_q8) * kInvQ8;
    return {cx - hw, cy - hh, hw * 2.0f, hh * 2.0f};
}

[[nodiscard]] constexpr renderer::FRect UnpackQ8ArrayBox(f32 wx, f32 wy, const std::array<i16, 4>& q8) noexcept {
    const f32 cx = wx + static_cast<f32>(q8[0]) * kInvQ8;
    const f32 cy = wy + static_cast<f32>(q8[1]) * kInvQ8;
    const f32 hw = static_cast<f32>(q8[2]) * kInvQ8;
    const f32 hh = static_cast<f32>(q8[3]) * kInvQ8;
    return {cx - hw, cy - hh, hw * 2.0f, hh * 2.0f};
}

struct ActiveCameraView {
    f32 centerX{0.0f};
    f32 centerY{0.0f};
    f32 zoom{1.0f};
    renderer::FRect viewport{};
    renderer::FRect worldBounds{};
};

[[nodiscard]] ActiveCameraView ResolveActiveCamera(const world::World& world,
                                                   platform::Extent2i viewportSize) noexcept {
    ActiveCameraView out{};
    out.viewport = {0.0f, 0.0f, static_cast<f32>(viewportSize.w), static_cast<f32>(viewportSize.h)};
    out.zoom     = 1.0f;

    for (const world::EntityId id : world.View<world::CameraRig2D>()) {
        const auto* rig = world.TryGet<world::CameraRig2D>(id);
        if (rig != nullptr && rig->active && !rig->screenSpace && rig->viewSlot == 0) {
            const world::WorldTransform2D& wt = world.GetWorldTransform(id);
            out.centerX = wt.x;
            out.centerY = wt.y;
            out.zoom    = rig->zoom > 0.01f ? rig->zoom : 1.0f;
            if (rig->viewport.w > 0.0f && rig->viewport.h > 0.0f) {
                out.viewport = rig->viewport;
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
    for (ActorTickRecord& rec : m_tickRing) {
        rec = ActorTickRecord{};
    }
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
        for (const world::EntityId id : world.View<gameplay::StateMachine2D, gameplay::KinematicMotor2D>()) {
            target = id;
            break;
        }
        if (target.index == 0) {
            for (const world::EntityId id : world.View<gameplay::KinematicMotor2D>()) {
                target = id;
                break;
            }
        }
    }

    ActorTickRecord rec{};
    rec.tickIndex     = static_cast<u32>(tickIndex);
    rec.entityId      = target;
    rec.inputHeld     = tickInput.held;
    rec.inputPressed  = tickInput.pressed;
    rec.inputReleased = tickInput.released;
    rec.axisX         = tickInput.axis[0];
    rec.axisY         = tickInput.axis[1];

    u16 bufferedMask = 0;
    for (u8 a = 0; a < input::kMaxActions; ++a) {
        if (actionBuffer.Peek(a, 12)) {
            bufferedMask |= static_cast<u16>(1u << a);
        }
    }
    rec.actionBufferMask = bufferedMask;

    if (world.IsAlive(target)) {
        const world::WorldTransform2D& wt = world.GetWorldTransform(target);
        rec.worldX = wt.x;
        rec.worldY = wt.y;

        if (const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(target)) {
            rec.velX         = motor->vx;
            rec.velY         = motor->vy;
            rec.contactFlags = static_cast<u8>(motor->contacts);
        } else if (const auto* vel = world.TryGet<world::Velocity2D>(target)) {
            rec.velX = vel->vx;
            rec.velY = vel->vy;
        }

        if (const auto* fsm = world.TryGet<gameplay::StateMachine2D>(target)) {
            rec.cancelMask = fsm->cancelMask;
            rec.stateTicks = fsm->stateTicks;
            rec.stateId    = fsm->stateId;
            rec.animClipId = fsm->animClipId;
            rec.animFrame  = fsm->lastAnimFrame;
        }

        if (const auto* rig = world.TryGet<gameplay::HitboxRig2D>(target)) {
            rec.hurtboxQ8 = {rig->hurtbox.offsetX_q8, rig->hurtbox.offsetY_q8,
                             rig->hurtbox.halfW_q8, rig->hurtbox.halfH_q8};
            rec.hitboxQ8  = {rig->primaryHitbox.offsetX_q8, rig->primaryHitbox.offsetY_q8,
                             rig->primaryHitbox.halfW_q8, rig->primaryHitbox.halfH_q8};
            rec.activeSliceIdx = rig->activeSliceIndex;
        }

        if (const auto* dmg = world.TryGet<gameplay::DamageReceiver2D>(target)) {
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
                                                      time::RenderTimeView,
                                                      input::Vec2 mousePos,
                                                      platform::Extent2i viewportSize) noexcept {
    const ActiveCameraView cam = ResolveActiveCamera(world, viewportSize);
    const f32 relX   = mousePos.x - (cam.viewport.x + cam.viewport.w * 0.5f);
    const f32 relY   = mousePos.y - (cam.viewport.y + cam.viewport.h * 0.5f);
    const f32 worldX = cam.centerX + relX / cam.zoom;
    const f32 worldY = cam.centerY + relY / cam.zoom;

    world::EntityId bestEntity = world::kNullEntity;
    f32 bestArea = 1e30f;

    for (const world::EntityId id : world.View<world::LocalTransform2D>()) {
        const world::WorldTransform2D& wt = world.GetWorldTransform(id);
        renderer::FRect bounds{wt.x - 8.0f, wt.y - 8.0f, 16.0f, 16.0f};

        if (const auto* rig = world.TryGet<gameplay::HitboxRig2D>(id); rig && rig->hurtbox.halfW_q8 > 0) {
            bounds = UnpackWorldBox(wt.x, wt.y, rig->hurtbox);
        } else if (const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(id); motor && motor->bounds.halfW_q8 > 0) {
            bounds = UnpackWorldBox(wt.x, wt.y, motor->bounds);
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

    // Single fused pass across alive spatial entities for all world-space overlays.
    if (HasOverlay(flags, OverlayFlags::AllWorld | OverlayFlags::EntityInspector)) {
        const bool drawTransforms  = HasOverlay(flags, OverlayFlags::SpatialTransforms);
        const bool drawKinematics  = HasOverlay(flags, OverlayFlags::KinematicBounds);
        const bool drawVelocities  = HasOverlay(flags, OverlayFlags::VelocityVectors);
        const bool drawCombat      = HasOverlay(flags, OverlayFlags::CombatHitboxes);
        const bool drawBreadcrumbs = HasOverlay(flags, OverlayFlags::SafeBreadcrumbs);
        const bool drawCameraRigs  = HasOverlay(flags, OverlayFlags::CameraRigOverlay);

        for (const world::EntityId id : ctx.world.View<world::LocalTransform2D>()) {
            const world::WorldTransform2D& wt = ctx.world.GetWorldTransform(id);
            const f32 wx = wt.x;
            const f32 wy = wt.y;

            const bool inFrustum = (wx >= cullMinX && wx <= cullMaxX && wy >= cullMinY && wy <= cullMaxY);
            if (!inFrustum && !drawCameraRigs && id.index != m_pinnedEntity.index) {
                continue;
            }

            if (drawTransforms && inFrustum) {
                worldBatcher.PushLine(wx, wy, wx + wt.cosRot * 8.0f, wy + wt.sinRot * 8.0f,
                                      1.0f, renderer::Color::FromRGBA8(0xFF4444FFu));
                worldBatcher.PushLine(wx, wy, wx - wt.sinRot * 8.0f, wy + wt.cosRot * 8.0f,
                                      1.0f, renderer::Color::FromRGBA8(0x44FF44FFu));
            }

            if (drawKinematics || drawVelocities) {
                if (const auto* motor = ctx.world.TryGet<gameplay::KinematicMotor2D>(id)) {
                    if (drawKinematics && inFrustum && motor->bounds.halfW_q8 > 0) {
                        const renderer::FRect box = UnpackWorldBox(wx, wy, motor->bounds);
                        const u8 contacts         = static_cast<u8>(motor->contacts);
                        const renderer::Color col = (contacts & 1u) != 0
                            ? renderer::Color::FromRGBA8(0x44FF88FFu)
                            : renderer::Color::FromRGBA8(0x44CCFFFFu);
                        worldBatcher.PushBoxOutline(box, 1.0f, col);
                    }
                    if (drawVelocities && inFrustum && (std::abs(motor->vx) > 0.5f || std::abs(motor->vy) > 0.5f)) {
                        worldBatcher.PushArrow(wx, wy, wx + motor->vx * 0.1f, wy + motor->vy * 0.1f,
                                               4.0f, 1.0f, renderer::Color::FromRGBA8(0xFFFF44FFu));
                    }
                } else if (drawVelocities && inFrustum) {
                    if (const auto* vel = ctx.world.TryGet<world::Velocity2D>(id)) {
                        if (std::abs(vel->vx) > 0.5f || std::abs(vel->vy) > 0.5f) {
                            worldBatcher.PushArrow(wx, wy, wx + vel->vx * 0.1f, wy + vel->vy * 0.1f,
                                                   4.0f, 1.0f, renderer::Color::FromRGBA8(0xFFCC44FFu));
                        }
                    }
                }

                if (drawKinematics && inFrustum) {
                    if (const auto* carrier = ctx.world.TryGet<gameplay::KinematicCarrier2D>(id)) {
                        const renderer::FRect box = UnpackWorldBox(wx, wy, carrier->bounds);
                        worldBatcher.PushBoxOutline(box, 1.5f, renderer::Color::FromRGBA8(0xFF9922FFu));
                    }
                }
            }

            if (drawCombat && inFrustum) {
                if (const auto* rig = ctx.world.TryGet<gameplay::HitboxRig2D>(id)) {
                    const auto* dmg      = ctx.world.TryGet<gameplay::DamageReceiver2D>(id);
                    const bool hasIframe = (dmg != nullptr && dmg->iframeTicks > 0);
                    if (rig->hurtbox.halfW_q8 > 0) {
                        const renderer::FRect hb  = UnpackWorldBox(wx, wy, rig->hurtbox);
                        const renderer::Color col = hasIframe
                            ? renderer::Color::FromRGBA8(0xFFFFFFFFu)
                            : renderer::Color::FromRGBA8(0x22FF66CCu);
                        worldBatcher.PushBoxOutline(hb, 1.0f, col);
                    }
                    if (rig->activeSliceIndex != 0xFFu && rig->primaryHitbox.halfW_q8 > 0) {
                        const renderer::FRect atk = UnpackWorldBox(wx, wy, rig->primaryHitbox);
                        worldBatcher.PushSolidRect(atk, renderer::Color::FromRGBA8(0xFF224444u));
                        worldBatcher.PushBoxOutline(atk, 1.5f, renderer::Color::FromRGBA8(0xFF2244FFu));
                    }
                }
            }

            if (drawBreadcrumbs && inFrustum) {
                if (const auto* bc = ctx.world.TryGet<gameplay::SafeGroundBreadcrumb2D>(id)) {
                    worldBatcher.PushLine(wx, wy, bc->safeX, bc->safeY, 1.0f,
                                          renderer::Color::FromRGBA8(0x44FFAA88u));
                    worldBatcher.PushBoxOutline({bc->safeX - 3.0f, bc->safeY - 3.0f, 6.0f, 6.0f},
                                                1.0f, renderer::Color::FromRGBA8(0x00FF88FFu));
                }
            }

            if (drawCameraRigs) {
                if (const auto* ctrl = ctx.world.TryGet<gameplay::CameraController2D>(id)) {
                    const f32 dzW = static_cast<f32>(ctrl->deadzoneHalfW_q4) * (1.0f / 16.0f);
                    const f32 dzH = static_cast<f32>(ctrl->deadzoneHalfH_q4) * (1.0f / 16.0f);
                    worldBatcher.PushBoxOutline({wx - dzW, wy - dzH, dzW * 2.0f, dzH * 2.0f},
                                                1.0f, renderer::Color::FromRGBA8(0xFF66FFAAu));
                }
            }

            if (id.index == m_pinnedEntity.index && id.generation == m_pinnedEntity.generation) {
                worldBatcher.PushBoxOutline({wx - 12.0f, wy - 12.0f, 24.0f, 24.0f},
                                            1.5f, renderer::Color::FromRGBA8(0xFFFF00FFu));
            }
        }
    }

    if (HasOverlay(flags, OverlayFlags::EntityInspector)) {
        EmitEntityInspectorCard(ctx.world, ctx.viewportSize, hudBatcher);
    }
    if (HasOverlay(flags, OverlayFlags::FlightScrubber)) {
        EmitFlightScrubberOverlay(ctx.timeView.tickIndex, ctx.viewportSize, worldBatcher, hudBatcher);
    }
    if (HasOverlay(flags, OverlayFlags::InputViewerHud)) {
        EmitInputViewerOverlay(ctx.frameInput, ctx.viewportSize, hudBatcher);
    }
}

void DebugInspector::EmitWorldAndInspectorOverlays(OverlayFlags flags,
                                                   const DebugEmitOverlayContext& ctx,
                                                   renderer::TextureHandle debugAtlas,
                                                   renderer::RenderList& outList) const noexcept {
    DebugBatcher worldBatcher(outList, debugAtlas, kDebugWorldPassId, 0);
    DebugBatcher hudBatcher(outList, debugAtlas, kDebugHudPassId, kDebugHudViewSlot);
    EmitWorldAndInspectorOverlays(flags, ctx, worldBatcher, hudBatcher);
}

void DebugInspector::EmitEntityInspectorCard(const world::World& world,
                                             platform::Extent2i viewportSize,
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

    DebugTextLine line{};
    line.Str("ENTITY #").I64(m_pinnedEntity.index).Str(" (Gen ").I64(m_pinnedEntity.generation).Char(')');
    hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xFFFF66FFu));
    ty += 12.0f;

    const world::WorldTransform2D& wt = world.GetWorldTransform(m_pinnedEntity);
    line.Clear();
    line.Str("Pos: (").F32(wt.x).Str(", ").F32(wt.y).Char(')');
    hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xE0E8F0FFu));
    ty += 11.0f;

    if (const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(m_pinnedEntity)) {
        line.Clear();
        line.Str("Vel: (").F32(motor->vx).Str(", ").F32(motor->vy).Str(") C:0x")
            .U64Hex(static_cast<u8>(motor->contacts), 2);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0x88EEFFFFu));
        ty += 11.0f;
    }

    if (const auto* fsm = world.TryGet<gameplay::StateMachine2D>(m_pinnedEntity)) {
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

    if (const auto* dmg = world.TryGet<gameplay::DamageReceiver2D>(m_pinnedEntity)) {
        line.Clear();
        line.Str("HP: ").I64(dmg->hp).Char('/').I64(dmg->maxHp)
            .Str("  IFrames: ").I64(dmg->iframeTicks);
        hudBatcher.PushText(tx, ty, line.View(), renderer::Color::FromRGBA8(0xFF99AAFFu));
    }
}

void DebugInspector::EmitFlightScrubberOverlay(u64,
                                               platform::Extent2i viewportSize,
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
            const renderer::FRect hb = UnpackQ8ArrayBox(rec.worldX, rec.worldY, rec.hurtboxQ8);
            worldBatcher.PushBoxOutline(hb, 1.5f, renderer::Color::FromRGBA8(0x00FFFFCCu));
        }
        if (rec.activeSliceIdx != 0xFFu && rec.hitboxQ8[2] > 0) {
            const renderer::FRect atk = UnpackQ8ArrayBox(rec.worldX, rec.worldY, rec.hitboxQ8);
            worldBatcher.PushBoxOutline(atk, 1.5f, renderer::Color::FromRGBA8(0xFF00FFCCu));
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
                                            platform::Extent2i viewportSize,
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
        const ActorTickRecord& r = m_tickRing[i];
        h = MixAuditHash(h, (static_cast<u64>(r.tickIndex) << 32) |
                            (static_cast<u64>(r.stateId) << 16) | r.inputHeld);
    }
    return h;
}

} // namespace en::debug

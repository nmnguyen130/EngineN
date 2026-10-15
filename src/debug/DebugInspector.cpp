#include "debug/DebugInspector.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "gameplay/GameplayTypes.h"
#include "platform/input/ActionBuffer.h"
#include "renderer/RendererList.h"
#include "world/World.h"

#include <algorithm>
#include <cmath>

namespace en::debug {

namespace {

constexpr f32 UnpackQ8(i16 q8) noexcept {
    return static_cast<f32>(q8) * (1.0f / 256.0f);
}

constexpr f32 UnpackQ14(i16 q14) noexcept {
    return static_cast<f32>(q14) * (1.0f / 16384.0f);
}

struct ActiveCameraView {
    f32 centerX{0.0f};
    f32 centerY{0.0f};
    f32 zoom{1.0f};
    f32 minX{-640.0f};
    f32 minY{-360.0f};
    f32 maxX{640.0f};
    f32 maxY{360.0f};
};

// Queries the active CameraRig2D in viewSlot 0 directly from World to compute frustum bounds.
[[nodiscard]] ActiveCameraView ResolveActiveCamera(const world::World& world,
                                                   platform::Extent2i viewportSize) noexcept {
    ActiveCameraView out{};
    const f32 halfW = static_cast<f32>(viewportSize.width) * 0.5f;
    const f32 halfH = static_cast<f32>(viewportSize.height) * 0.5f;

    for (const world::EntityId id : world.View<world::CameraRig2D>()) {
        const auto* rig = world.TryGet<world::CameraRig2D>(id);
        if (rig != nullptr && rig->active && rig->viewSlot == 0 && !rig->screenSpace) {
            const auto& wt = world.GetWorldTransform(id);
            out.centerX = wt.x;
            out.centerY = wt.y;
            out.zoom    = std::max(rig->zoom, 0.05f);
            break;
        }
    }

    const f32 invZoom = 1.0f / out.zoom;
    out.minX = out.centerX - halfW * invZoom;
    out.maxX = out.centerX + halfW * invZoom;
    out.minY = out.centerY - halfH * invZoom;
    out.maxY = out.centerY + halfH * invZoom;
    return out;
}

} // namespace

void DebugInspector::BindArena(std::span<ActorTickRecord, kHistoryTicks> tickRing) noexcept {
    m_tickRing = tickRing;
    Clear();
}

void DebugInspector::Clear() noexcept {
    for (auto& rec : m_tickRing) {
        rec = ActorTickRecord{};
    }
    m_pinnedEntity     = world::kNullEntity;
    m_lastRecordedTick = 0;
    m_scrubOffset      = 0;
    m_recordedCount    = 0;
}

void DebugInspector::RecordActorTick(const world::World& world,
                                     u64 tickIndex,
                                     const platform::input::TickInput& tickInput,
                                     const platform::input::ActionBuffer& actionBuffer) noexcept {
    if (m_tickRing.empty()) {
        return;
    }

    world::EntityId targetId = m_pinnedEntity;
    if (!world.IsAlive(targetId)) {
        for (const world::EntityId id : world.View<gameplay::KinematicMotor2D, gameplay::StateMachine2D>()) {
            targetId = id;
            break;
        }
    }

    ActorTickRecord& rec = m_tickRing[tickIndex & (kHistoryTicks - 1u)];
    rec = ActorTickRecord{};
    rec.tickIndex     = tickIndex;
    rec.entityId      = targetId;
    rec.inputHeld     = tickInput.held;
    rec.inputPressed  = tickInput.pressed;
    rec.inputReleased = tickInput.released;
    rec.axisX         = tickInput.axis[0];
    rec.axisY         = tickInput.axis[1];

    u16 bufMask = 0;
    for (u8 a = 0; a < 16; ++a) {
        if (actionBuffer.Peek(a, 12)) {
            bufMask |= static_cast<u16>(1u << a);
        }
    }
    rec.actionBufferMask = bufMask;

    if (world.IsAlive(targetId)) {
        const auto& wt = world.GetWorldTransform(targetId);
        rec.worldX = wt.x;
        rec.worldY = wt.y;

        if (const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(targetId)) {
            rec.velX         = motor->vx;
            rec.velY         = motor->vy;
            rec.contactFlags = static_cast<u8>(motor->contacts);
        }
        if (const auto* fsm = world.TryGet<gameplay::StateMachine2D>(targetId)) {
            rec.stateId    = fsm->stateId;
            rec.animClipId = fsm->animClipId;
            rec.animFrame  = static_cast<u8>(fsm->lastAnimFrame);
            rec.stateTicks = fsm->stateTicks;
            rec.cancelMask = static_cast<u16>(fsm->cancelMask);
        }
        if (const auto* rig = world.TryGet<gameplay::HitboxRig2D>(targetId)) {
            rec.activeSliceIdx = rig->activeSliceIndex;
            rec.hurtboxQ8      = {rig->hurtbox.offsetX_q8, rig->hurtbox.offsetY_q8,
                                  rig->hurtbox.halfW_q8,   rig->hurtbox.halfH_q8};
            rec.hitboxQ8       = {rig->primaryHitbox.offsetX_q8, rig->primaryHitbox.offsetY_q8,
                                  rig->primaryHitbox.halfW_q8,   rig->primaryHitbox.halfH_q8};
        }
        if (const auto* dmg = world.TryGet<gameplay::DamageReceiver2D>(targetId)) {
            rec.hp          = static_cast<i16>(dmg->hp);
            rec.iframeTicks = static_cast<u8>(std::min<u16>(255u, dmg->iframeTicks));
        }
    }

    m_lastRecordedTick = tickIndex;
    if (m_recordedCount < kHistoryTicks) {
        ++m_recordedCount;
    }
}

world::EntityId DebugInspector::PickEntityAtScreenPos(const world::World& world,
                                                      time::RenderTimeView /*timeView*/,
                                                      platform::input::Vec2 mousePos,
                                                      platform::Extent2i viewportSize) noexcept {
    const ActiveCameraView cam = ResolveActiveCamera(world, viewportSize);
    const f32 halfW  = static_cast<f32>(viewportSize.width) * 0.5f;
    const f32 halfH  = static_cast<f32>(viewportSize.height) * 0.5f;
    const f32 worldX = cam.centerX + (mousePos.x - halfW) / cam.zoom;
    const f32 worldY = cam.centerY + (mousePos.y - halfH) / cam.zoom;

    world::EntityId bestEntity = world::kNullEntity;
    f32 bestArea = 1e30f;

    for (const world::EntityId id : world.View<gameplay::KinematicMotor2D>()) {
        const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(id);
        if (motor == nullptr) continue;
        const auto& wt = world.GetWorldTransform(id);
        const f32 cx = wt.x + UnpackQ8(motor->bounds.offsetX_q8);
        const f32 cy = wt.y + UnpackQ8(motor->bounds.offsetY_q8);
        const f32 hw = std::max(4.0f, UnpackQ8(motor->bounds.halfW_q8));
        const f32 hh = std::max(4.0f, UnpackQ8(motor->bounds.halfH_q8));

        if (std::abs(worldX - cx) <= hw && std::abs(worldY - cy) <= hh) {
            const f32 area = hw * hh;
            if (area < bestArea) {
                bestArea   = area;
                bestEntity = id;
            }
        }
    }

    m_pinnedEntity = bestEntity;
    return bestEntity;
}

void DebugInspector::EmitWorldAndInspectorOverlays(
    OverlayFlags flags,
    const world::World& world,
    const gameplay::GameplayPipeline& /*gameplay*/,
    const audio::AudioSystem& /*audio*/,
    const assets::AssetBank& /*assetBank*/,
    time::RenderTimeView /*timeView*/,
    const platform::input::FrameInput& frameInput,
    platform::Extent2i viewportSize,
    renderer::TextureHandle debugAtlas,
    renderer::RenderList& outList) const noexcept {

    DebugBatcher worldBatcher(outList, debugAtlas, kDebugWorldPassId, 0);
    DebugBatcher hudBatcher(outList, debugAtlas, kDebugHudPassId, kDebugHudViewSlot);

    const ActiveCameraView cam = ResolveActiveCamera(world, viewportSize);
    const f32 linePx = 1.0f / cam.zoom;

    if (HasOverlay(flags, OverlayFlags::KinematicBounds) ||
        HasOverlay(flags, OverlayFlags::VelocityVectors)) {
        for (const world::EntityId id : world.View<gameplay::KinematicMotor2D>()) {
            const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(id);
            if (motor == nullptr) continue;
            const auto& wt = world.GetWorldTransform(id);

            const f32 cx = wt.x + UnpackQ8(motor->bounds.offsetX_q8);
            const f32 cy = wt.y + UnpackQ8(motor->bounds.offsetY_q8);
            const f32 hw = UnpackQ8(motor->bounds.halfW_q8);
            const f32 hh = UnpackQ8(motor->bounds.halfH_q8);

            if (cx + hw < cam.minX || cx - hw > cam.maxX ||
                cy + hh < cam.minY || cy - hh > cam.maxY) {
                continue;
            }

            if (HasOverlay(flags, OverlayFlags::KinematicBounds)) {
                const renderer::FRect box{cx - hw, cy - hh, hw * 2.0f, hh * 2.0f};
                const renderer::Color col = (id.index == m_pinnedEntity.index && id.generation == m_pinnedEntity.generation)
                    ? renderer::Color::FromRGBA8(0x00FF88FFu)
                    : renderer::Color::FromRGBA8(0x00E5FFFFu);
                worldBatcher.PushBoxOutline(box, linePx, col);

                const u8 contacts = static_cast<u8>(motor->contacts);
                const renderer::Color contactCol = renderer::Color::FromRGBA8(0x00FF44FFu);
                if (contacts & static_cast<u8>(gameplay::ContactFlags::Ground)) {
                    worldBatcher.PushLine(cx - hw, cy + hh, cx + hw, cy + hh, linePx * 2.5f, contactCol);
                }
                if (contacts & static_cast<u8>(gameplay::ContactFlags::Ceiling)) {
                    worldBatcher.PushLine(cx - hw, cy - hh, cx + hw, cy - hh, linePx * 2.5f, contactCol);
                }
                if (contacts & static_cast<u8>(gameplay::ContactFlags::WallLeft)) {
                    worldBatcher.PushLine(cx - hw, cy - hh, cx - hw, cy + hh, linePx * 2.5f, contactCol);
                }
                if (contacts & static_cast<u8>(gameplay::ContactFlags::WallRight)) {
                    worldBatcher.PushLine(cx + hw, cy - hh, cx + hw, cy + hh, linePx * 2.5f, contactCol);
                }
                if (contacts & static_cast<u8>(gameplay::ContactFlags::OnSlope)) {
                    const f32 nx = UnpackQ14(motor->slopeNormalX_q14);
                    const f32 ny = UnpackQ14(motor->slopeNormalY_q14);
                    worldBatcher.PushArrow(cx, cy + hh, cx + nx * 12.0f, cy + hh + ny * 12.0f,
                                           3.0f * linePx, linePx, renderer::Color::FromRGBA8(0xFFAA00FFu));
                }
            }

            if (HasOverlay(flags, OverlayFlags::VelocityVectors)) {
                worldBatcher.PushArrow(cx, cy, cx + motor->vx * 0.1f, cy + motor->vy * 0.1f,
                                       4.0f * linePx, linePx, renderer::Color::FromRGBA8(0xFFFF00FFu));
            }
        }
    }

    if (HasOverlay(flags, OverlayFlags::CombatHitboxes)) {
        for (const world::EntityId id : world.View<gameplay::HitboxRig2D>()) {
            const auto* rig = world.TryGet<gameplay::HitboxRig2D>(id);
            if (rig == nullptr) continue;
            const auto& wt = world.GetWorldTransform(id);

            const f32 hcx = wt.x + UnpackQ8(rig->hurtbox.offsetX_q8);
            const f32 hcy = wt.y + UnpackQ8(rig->hurtbox.offsetY_q8);
            const f32 hhw = UnpackQ8(rig->hurtbox.halfW_q8);
            const f32 hhh = UnpackQ8(rig->hurtbox.halfH_q8);

            if (hhw > 0.0f && hhh > 0.0f) {
                bool invuln = false;
                if (const auto* dmg = world.TryGet<gameplay::DamageReceiver2D>(id)) {
                    invuln = (dmg->iframeTicks > 0);
                }
                const renderer::FRect hurtRect{hcx - hhw, hcy - hhh, hhw * 2.0f, hhh * 2.0f};
                worldBatcher.PushSolidRect(hurtRect, invuln
                    ? renderer::Color::FromRGBA8(0xFFD70044u)
                    : renderer::Color::FromRGBA8(0x2288FF44u));
                worldBatcher.PushBoxOutline(hurtRect, linePx, invuln
                    ? renderer::Color::FromRGBA8(0xFFD700FFu)
                    : renderer::Color::FromRGBA8(0x44AAFFFFu));
            }

            const f32 acx = wt.x + UnpackQ8(rig->primaryHitbox.offsetX_q8);
            const f32 acy = wt.y + UnpackQ8(rig->primaryHitbox.offsetY_q8);
            const f32 ahw = UnpackQ8(rig->primaryHitbox.halfW_q8);
            const f32 ahh = UnpackQ8(rig->primaryHitbox.halfH_q8);

            if (ahw > 0.0f && ahh > 0.0f) {
                const renderer::FRect hitRect{acx - ahw, acy - ahh, ahw * 2.0f, ahh * 2.0f};
                worldBatcher.PushSolidRect(hitRect, renderer::Color::FromRGBA8(0xFF222266u));
                worldBatcher.PushBoxOutline(hitRect, linePx * 1.5f, renderer::Color::FromRGBA8(0xFF4444FFu));
            }
        }
    }

    if (HasOverlay(flags, OverlayFlags::SafeBreadcrumbs)) {
        for (const world::EntityId id : world.View<gameplay::SafeGroundBreadcrumb2D>()) {
            const auto* bc = world.TryGet<gameplay::SafeGroundBreadcrumb2D>(id);
            if (bc == nullptr) continue;
            worldBatcher.PushLine(bc->prevSafeX, bc->prevSafeY, bc->safeX, bc->safeY,
                                  linePx, renderer::Color::FromRGBA8(0x44FF88AAu));
            worldBatcher.PushCircleOutline(bc->safeX, bc->safeY, 4.0f * linePx, 8,
                                           linePx, renderer::Color::FromRGBA8(0x00FF66FFu));
        }
    }

    if (HasOverlay(flags, OverlayFlags::CameraRigOverlay)) {
        for (const world::EntityId id : world.View<gameplay::CameraController2D>()) {
            const auto* ctrl = world.TryGet<gameplay::CameraController2D>(id);
            if (ctrl == nullptr) continue;
            const auto& wt = world.GetWorldTransform(id);
            const f32 dzW = static_cast<f32>(ctrl->deadzoneHalfW_q4) * (1.0f / 16.0f);
            const f32 dzH = static_cast<f32>(ctrl->deadzoneHalfH_q4) * (1.0f / 16.0f);
            worldBatcher.PushBoxOutline({wt.x - dzW, wt.y - dzH, dzW * 2.0f, dzH * 2.0f},
                                        linePx, renderer::Color::FromRGBA8(0xFFCC00CCu));
        }
    }

    if (HasOverlay(flags, OverlayFlags::EntityInspector)) {
        EmitEntityInspectorCard(world, viewportSize, hudBatcher);
    }

    if (HasOverlay(flags, OverlayFlags::FlightScrubber)) {
        EmitFlightScrubberOverlay(m_lastRecordedTick, viewportSize, worldBatcher, hudBatcher);
    }

    if (HasOverlay(flags, OverlayFlags::InputViewerHud)) {
        EmitInputViewerOverlay(frameInput, viewportSize, hudBatcher);
    }
}

void DebugInspector::EmitEntityInspectorCard(const world::World& world,
                                             platform::Extent2i /*viewportSize*/,
                                             DebugBatcher& hudBatcher) const noexcept {
    if (!world.IsAlive(m_pinnedEntity)) {
        return;
    }

    const f32 panelX = 12.0f;
    const f32 panelY = 112.0f;
    const f32 panelW = 264.0f;
    const f32 panelH = 148.0f;

    hudBatcher.PushSolidRect({panelX, panelY, panelW, panelH}, renderer::Color::FromRGBA8(0x10141CDD));
    hudBatcher.PushBoxOutline({panelX, panelY, panelW, panelH}, 1.0f, renderer::Color::FromRGBA8(0x00E5FFFF));

    std::array<char, 64> buf{};
    char* end = buf.data() + buf.size();

    auto emitLine = [&](f32 yOff, std::string_view prefix, i64 v0, i64 v1, renderer::Color col) {
        char* p = buf.data();
        for (char c : prefix) {
            if (p < end) *p++ = c;
        }
        p = DebugBatcher::FormatI64(p, end, v0);
        if (p < end) *p++ = ':';
        p = DebugBatcher::FormatI64(p, end, v1);
        hudBatcher.PushText(panelX + 8.0f, panelY + yOff,
                            std::string_view(buf.data(), static_cast<usize>(p - buf.data())), col);
    };

    emitLine(8.0f, "ENTITY ID:GEN  ", m_pinnedEntity.index, m_pinnedEntity.generation,
             renderer::Color::FromRGBA8(0x00FF88FF));

    const auto& wt = world.GetWorldTransform(m_pinnedEntity);
    {
        char* p = buf.data();
        for (char c : std::string_view("POS X/Y: ")) *p++ = c;
        p = DebugBatcher::FormatF32Fixed2(p, end, wt.x);
        if (p < end) *p++ = ' ';
        p = DebugBatcher::FormatF32Fixed2(p, end, wt.y);
        hudBatcher.PushText(panelX + 8.0f, panelY + 24.0f,
                            std::string_view(buf.data(), static_cast<usize>(p - buf.data())),
                            renderer::Color::FromRGBA8(0xE0E6F0FF));
    }

    if (const auto* motor = world.TryGet<gameplay::KinematicMotor2D>(m_pinnedEntity)) {
        char* p = buf.data();
        for (char c : std::string_view("VEL X/Y: ")) *p++ = c;
        p = DebugBatcher::FormatF32Fixed2(p, end, motor->vx);
        if (p < end) *p++ = ' ';
        p = DebugBatcher::FormatF32Fixed2(p, end, motor->vy);
        hudBatcher.PushText(panelX + 8.0f, panelY + 40.0f,
                            std::string_view(buf.data(), static_cast<usize>(p - buf.data())),
                            renderer::Color::FromRGBA8(0xFFFF66FF));

        emitLine(56.0f, "CONTACTS:CH    ", static_cast<u8>(motor->contacts), motor->timeChannel,
                 renderer::Color::FromRGBA8(0x88DDFFFF));
    }

    if (const auto* fsm = world.TryGet<gameplay::StateMachine2D>(m_pinnedEntity)) {
        emitLine(76.0f, "FSM STATE:TICK ", fsm->stateId, fsm->stateTicks,
                 renderer::Color::FromRGBA8(0xFFAA44FF));
        emitLine(92.0f, "CLIP:FRAME     ", fsm->animClipId, fsm->lastAnimFrame,
                 renderer::Color::FromRGBA8(0xFFCC88FF));
    }

    if (const auto* dmg = world.TryGet<gameplay::DamageReceiver2D>(m_pinnedEntity)) {
        emitLine(112.0f, "HP:MAX_HP      ", dmg->hp, dmg->maxHp,
                 renderer::Color::FromRGBA8(0xFF5555FF));
        emitLine(128.0f, "IFRAME:STUN    ", dmg->iframeTicks, dmg->stunTicks,
                 renderer::Color::FromRGBA8(0xFFD700FF));
    }
}

void DebugInspector::EmitFlightScrubberOverlay(u64 currentTickIndex,
                                               platform::Extent2i viewportSize,
                                               DebugBatcher& worldBatcher,
                                               DebugBatcher& hudBatcher) const noexcept {
    if (m_recordedCount == 0 || m_tickRing.empty()) {
        return;
    }

    const u64 targetTick = currentTickIndex - m_scrubOffset;
    const ActorTickRecord& rec = m_tickRing[targetTick & (kHistoryTicks - 1u)];

    // Render historical ghost silhouette in world space when scrubbing backward.
    if (m_scrubOffset > 0 && rec.entityId.index != 0) {
        const f32 hcx = rec.worldX + UnpackQ8(rec.hurtboxQ8[0]);
        const f32 hcy = rec.worldY + UnpackQ8(rec.hurtboxQ8[1]);
        const f32 hhw = UnpackQ8(rec.hurtboxQ8[2]);
        const f32 hhh = UnpackQ8(rec.hurtboxQ8[3]);
        if (hhw > 0.0f && hhh > 0.0f) {
            const renderer::FRect ghostHurt{hcx - hhw, hcy - hhh, hhw * 2.0f, hhh * 2.0f};
            worldBatcher.PushSolidRect(ghostHurt, renderer::Color::FromRGBA8(0x00FFFF33u));
            worldBatcher.PushBoxOutline(ghostHurt, 1.0f, renderer::Color::FromRGBA8(0x00FFFFFFu));
        }

        const f32 acx = rec.worldX + UnpackQ8(rec.hitboxQ8[0]);
        const f32 acy = rec.worldY + UnpackQ8(rec.hitboxQ8[1]);
        const f32 ahw = UnpackQ8(rec.hitboxQ8[2]);
        const f32 ahh = UnpackQ8(rec.hitboxQ8[3]);
        if (ahw > 0.0f && ahh > 0.0f) {
            const renderer::FRect ghostHit{acx - ahw, acy - ahh, ahw * 2.0f, ahh * 2.0f};
            worldBatcher.PushSolidRect(ghostHit, renderer::Color::FromRGBA8(0xFF005555u));
            worldBatcher.PushBoxOutline(ghostHit, 1.5f, renderer::Color::FromRGBA8(0xFF2266FFu));
        }
    }

    // Emit 64-tick bottom scrubber bar in screen space.
    const f32 barW = 384.0f;
    const f32 barH = 54.0f;
    const f32 barX = (static_cast<f32>(viewportSize.width) - barW) * 0.5f;
    const f32 barY = static_cast<f32>(viewportSize.height) - barH - 12.0f;

    hudBatcher.PushSolidRect({barX, barY, barW, barH}, renderer::Color::FromRGBA8(0x0D1117DD));
    hudBatcher.PushBoxOutline({barX, barY, barW, barH}, 1.0f, renderer::Color::FromRGBA8(0x4488FFFF));

    std::array<char, 64> hdr{};
    char* p = hdr.data();
    char* end = hdr.data() + hdr.size();
    for (char c : std::string_view("FLIGHT SCRUB [-")) *p++ = c;
    p = DebugBatcher::FormatI64(p, end, m_scrubOffset);
    for (char c : std::string_view("t] STATE:")) *p++ = c;
    p = DebugBatcher::FormatI64(p, end, rec.stateId);
    for (char c : std::string_view(" FRM:")) *p++ = c;
    p = DebugBatcher::FormatI64(p, end, rec.animFrame);
    hudBatcher.PushText(barX + 8.0f, barY + 6.0f,
                        std::string_view(hdr.data(), static_cast<usize>(p - hdr.data())),
                        renderer::Color::FromRGBA8(0xFFFFFFFF));

    const f32 cellW = 5.5f;
    const f32 stripX = barX + 14.0f;
    const f32 stripY = barY + 22.0f;

    for (u8 i = 0; i < m_recordedCount; ++i) {
        const u64 t = currentTickIndex - i;
        const ActorTickRecord& slot = m_tickRing[t & (kHistoryTicks - 1u)];
        const f32 x = stripX + static_cast<f32>(kHistoryTicks - 1u - i) * cellW;

        const bool hasHitbox = (slot.hitboxQ8[2] > 0 && slot.hitboxQ8[3] > 0);
        const bool hasCancel = (slot.cancelMask != 0);
        const bool hasPress  = (slot.inputPressed != 0);

        const renderer::Color stateCol = hasHitbox
            ? renderer::Color::FromRGBA8(0xFF3333FF)
            : (hasCancel ? renderer::Color::FromRGBA8(0xFFCC00FF)
                         : renderer::Color::FromRGBA8(0x336699FF));
        hudBatcher.PushSolidRect({x, stripY, cellW - 1.0f, 12.0f}, stateCol);

        if (hasPress) {
            hudBatcher.PushSolidRect({x, stripY + 14.0f, cellW - 1.0f, 8.0f},
                                     renderer::Color::FromRGBA8(0x00FF66FF));
        } else if (slot.actionBufferMask != 0) {
            hudBatcher.PushSolidRect({x, stripY + 16.0f, cellW - 1.0f, 4.0f},
                                     renderer::Color::FromRGBA8(0x00AA44AA));
        }

        if (i == m_scrubOffset) {
            hudBatcher.PushBoxOutline({x - 1.0f, stripY - 2.0f, cellW + 1.0f, 26.0f},
                                      1.0f, renderer::Color::FromRGBA8(0xFFFFFFFF));
        }
    }
}

void DebugInspector::EmitInputViewerOverlay(const platform::input::FrameInput& frameInput,
                                            platform::Extent2i viewportSize,
                                            DebugBatcher& hudBatcher) const noexcept {
    const f32 panelW = 240.0f;
    const f32 panelH = 68.0f;
    const f32 panelX = 12.0f;
    const f32 panelY = static_cast<f32>(viewportSize.height) - panelH - 12.0f;

    hudBatcher.PushSolidRect({panelX, panelY, panelW, panelH}, renderer::Color::FromRGBA8(0x10141CDD));
    hudBatcher.PushBoxOutline({panelX, panelY, panelW, panelH}, 1.0f, renderer::Color::FromRGBA8(0x557799FF));
    hudBatcher.PushText(panelX + 8.0f, panelY + 6.0f, "INPUT ACTIONS [0..15]",
                        renderer::Color::FromRGBA8(0xAACCFFFF));

    for (u8 a = 0; a < 16; ++a) {
        const f32 bx = panelX + 8.0f + static_cast<f32>(a) * 14.0f;
        const f32 by = panelY + 24.0f;
        const bool pressed  = (frameInput.pressed & (1u << a)) != 0;
        const bool held     = (frameInput.held & (1u << a)) != 0;
        const bool released = (frameInput.released & (1u << a)) != 0;

        renderer::Color col = renderer::Color::FromRGBA8(0x283040FF);
        if (pressed) {
            col = renderer::Color::FromRGBA8(0x00FF66FF);
        } else if (held) {
            col = renderer::Color::FromRGBA8(0x00B8FFFF);
        } else if (released) {
            col = renderer::Color::FromRGBA8(0xFFAA00FF);
        }
        hudBatcher.PushSolidRect({bx, by, 11.0f, 16.0f}, col);
    }

    if (m_recordedCount > 0 && !m_tickRing.empty()) {
        const ActorTickRecord& latest = m_tickRing[m_lastRecordedTick & (kHistoryTicks - 1u)];
        const f32 stickCx = panelX + 208.0f;
        const f32 stickCy = panelY + 48.0f;
        hudBatcher.PushCircleOutline(stickCx, stickCy, 14.0f, 16, 1.0f, renderer::Color::FromRGBA8(0x6688AAFF));
        const f32 sx = stickCx + (static_cast<f32>(latest.axisX) / 32767.0f) * 12.0f;
        const f32 sy = stickCy + (static_cast<f32>(latest.axisY) / 32767.0f) * 12.0f;
        hudBatcher.PushSolidRect({sx - 2.0f, sy - 2.0f, 4.0f, 4.0f}, renderer::Color::FromRGBA8(0x00FF88FF));
    }
}

} // namespace en::debug

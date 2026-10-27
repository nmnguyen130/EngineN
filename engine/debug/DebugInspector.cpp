#include "debug/DebugInspector.h"
#include "gameplay/GameplayPipeline.h"
#include "input/ActionBuffer.h"
#include "input/InputTypes.h"
#include "physics/PhysicsSystem.h"
#include "world/World.h"

namespace en {

void DebugInspector::RecordActorTick(
    const World& world,
    u32 tickIndex,
    const TickInput* tickInput,
    const ActionBuffer* actionBuffer) noexcept {
    if (!m_history || !m_pinnedEntity.IsValid() || !world.IsAlive(m_pinnedEntity)) {
        return;
    }

    const u16 eIdx = m_pinnedEntity.index;
    const u64 activeMask = world.RegisteredPoolsMask() & world.GetComponentMaskByIndex(eIdx);
    const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);

    ActorTickRecord rec{};
    rec.tickIndex = tickIndex;
    rec.entityIndex = eIdx;
    rec.worldX = wt.x;
    rec.worldY = wt.y;

    if ((activeMask & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
        const CharacterMotor2D& m = world.GetPool<CharacterMotor2D>().DirectAtEntityIndex(eIdx);
        rec.velX = m.vx;
        rec.velY = m.vy;
        rec.contactFlags = static_cast<u8>(m.contacts);
    } else if ((activeMask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
        const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(eIdx);
        rec.velX = rb.vx;
        rec.velY = rb.vy;
    }

    if ((activeMask & MakeComponentMask<StateMachine2D>()) != 0ULL) {
        rec.stateId = world.GetPool<StateMachine2D>().DirectAtEntityIndex(eIdx).stateId;
    }

    if ((activeMask & MakeComponentMask<DamageReceiver2D>()) != 0ULL) {
        const DamageReceiver2D& dr = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(eIdx);
        rec.hp = static_cast<i16>(std::clamp<i32>(dr.hp, -32768, 32767));
        rec.iframeTicks = static_cast<u8>(std::min<u16>(255U, dr.iframeTicks));
    }

    if (tickInput) {
        rec.inputHeld = static_cast<u8>(tickInput->held & 0xFFU);
        rec.inputPressed = static_cast<u8>(tickInput->pressed & 0xFFU);
        rec.axisX_q7 = static_cast<i8>(std::clamp(tickInput->axis[0] * 127.0f, -127.0f, 127.0f));
        rec.axisY_q7 = static_cast<i8>(std::clamp(tickInput->axis[1] * 127.0f, -127.0f, 127.0f));
    }
    if (actionBuffer) {
        rec.actionBufferMask = static_cast<u8>(actionBuffer->PendingPressMask() & 0xFFU);
    }

    const u16 slot = m_writeIdx & (kActorFlightHistoryTicks - 1U);
    m_history[slot] = rec;
    m_writeIdx = static_cast<u16>((slot + 1U) & (kActorFlightHistoryTicks - 1U));
    if (m_validCount < kActorFlightHistoryTicks) {
        ++m_validCount;
    }
}

const ActorTickRecord* DebugInspector::InspectedRecord() const noexcept {
    if (!m_history || m_validCount == 0U) {
        return nullptr;
    }
    const u8 clampedOff = std::min<u8>(m_scrubOffset, static_cast<u8>(m_validCount - 1U));
    const u32 idx = (static_cast<u32>(m_writeIdx) + kActorFlightHistoryTicks - 1U - clampedOff) &
                    (kActorFlightHistoryTicks - 1U);
    return &m_history[idx];
}

EntityId DebugInspector::PickEntityAtWorldPos(
    const World& world,
    Vec2 worldPos,
    f32 pickRadius) noexcept {
    const std::span<const u64> aliveWords = world.AliveWords();
    const u64 regMask = world.RegisteredPoolsMask();

    EntityId bestEntity = kNullEntity;
    f32 bestDistSq = pickRadius * pickRadius;

    for (usize w = 0U; w < aliveWords.size(); ++w) {
        u64 bits = aliveWords[w];
        while (bits != 0ULL) {
            const u32 b = static_cast<u32>(std::countr_zero(bits));
            bits &= (bits - 1ULL);
            const u16 eIdx = static_cast<u16>((w << 6U) + b);
            if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) {
                continue;
            }

            const EntityId e = world.EntityAtSlot(eIdx);
            const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
            const u64 activeMask = regMask & world.GetComponentMaskByIndex(eIdx);

            f32 cx = wt.x;
            f32 cy = wt.y;
            f32 hw = 0.0f;
            f32 hh = 0.0f;

            if ((activeMask & MakeComponentMask<Collider2D>()) != 0ULL) {
                const Collider2D& col = world.GetPool<Collider2D>().DirectAtEntityIndex(eIdx);
                if (col.shape != ShapeKind2D::Segment) {
                    const Vec2 c = wt.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
                    cx = c.x;
                    cy = c.y;
                    hw = col.localBounds.HalfW() * std::abs(wt.scaleX);
                    hh = (col.shape == ShapeKind2D::Circle) ? hw : (col.localBounds.HalfH() * std::abs(wt.scaleY));
                }
            } else if ((activeMask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
                const HitboxRig2D& hb = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(eIdx);
                cx = wt.x + hb.hurtbox.OffsetX();
                cy = wt.y + hb.hurtbox.OffsetY();
                hw = hb.hurtbox.HalfW();
                hh = hb.hurtbox.HalfH();
            }

            const f32 dxBox = std::max(0.0f, std::abs(worldPos.x - cx) - hw);
            const f32 dyBox = std::max(0.0f, std::abs(worldPos.y - cy) - hh);
            const f32 boxDistSq = dxBox * dxBox + dyBox * dyBox;
            if (boxDistSq <= bestDistSq) {
                const f32 centerDistSq =
                    ((worldPos.x - cx) * (worldPos.x - cx) + (worldPos.y - cy) * (worldPos.y - cy)) * 1e-3f;
                const f32 score = boxDistSq + centerDistSq;
                if (score <= bestDistSq) {
                    bestDistSq = score;
                    bestEntity = e;
                }
            }
        }
    }

    if (bestEntity.IsValid()) {
        SetPinnedEntity(bestEntity);
    }
    return bestEntity;
}

EntityId DebugInspector::PickEntityAtScreenPos(
    const World& world,
    Vec2 screenPos,
    ViewportExtent viewportSize,
    const Camera2D& worldCam) noexcept {
    return PickEntityAtWorldPos(world, worldCam.ViewportToWorld(screenPos, viewportSize), 1.25f);
}

void DebugInspector::EmitWorldAndInspectorOverlays(
    OverlayFlags flags,
    const DebugFrameContext& ctx,
    DebugBatcher& worldBatcher,
    DebugBatcher& hudBatcher) const noexcept {
    if (ctx.world) {
        const World& world = *ctx.world;
        const u64 regMask = world.RegisteredPoolsMask();
        const std::span<const u64> aliveWords = world.AliveWords();

        if (HasFlag(flags, OverlayFlags::SpatialTransforms)) {
            for (usize w = 0U; w < aliveWords.size(); ++w) {
                u64 bits = aliveWords[w];
                while (bits != 0ULL) {
                    const u32 b = static_cast<u32>(std::countr_zero(bits));
                    bits &= (bits - 1ULL);
                    const u16 eIdx = static_cast<u16>((w << 6U) + b);
                    if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) {
                        continue;
                    }
                    const EntityId e = world.EntityAtSlot(eIdx);
                    const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
                    const Color col = (e == m_pinnedEntity)
                                          ? Color{1.0f, 0.9f, 0.1f, 1.0f}
                                          : Color{0.2f, 0.85f, 1.0f, 0.85f};
                    worldBatcher.PushCrosshair(wt.x, wt.y, 0.25f, 0.04f, col);
                }
            }
        }

        if (HasFlag(flags, OverlayFlags::PhysicsColliders) &&
            (regMask & MakeComponentMask<Collider2D>()) != 0ULL) {
            const bool hasRbPool = (regMask & MakeComponentMask<RigidBody2D>()) != 0ULL;
            const auto& colPool = world.GetPool<Collider2D>();
            for (u16 i = 0U; i < colPool.Size(); ++i) {
                const EntityId e = colPool.Entities()[i];
                const u64 eMask = world.GetComponentMaskByIndex(e.index);
                const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                const Collider2D& col = colPool.ComponentAtDense(i);

                Color wireCol{0.45f, 0.75f, 0.95f, 0.85f};
                if (col.IsSensor()) {
                    wireCol = Color{1.0f, 0.85f, 0.15f, 0.85f};
                } else if (col.IsOneWay()) {
                    wireCol = Color{1.0f, 0.55f, 0.15f, 0.85f};
                } else if (hasRbPool && (eMask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
                    const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(e.index);
                    if (rb.IsSleeping()) {
                        wireCol = Color{0.25f, 0.55f, 0.55f, 0.7f};
                    } else if (rb.IsKinematic()) {
                        wireCol = Color{0.75f, 0.35f, 1.0f, 0.9f};
                    } else if (rb.IsDynamic()) {
                        wireCol = Color{0.15f, 0.95f, 0.45f, 0.9f};
                    }
                }

                const Vec2 center = wt.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
                const f32 cx = center.x;
                const f32 cy = center.y;
                const f32 hw = col.localBounds.HalfW() * std::abs(wt.scaleX);
                const f32 hh = col.localBounds.HalfH() * std::abs(wt.scaleY);

                switch (col.shape) {
                    case ShapeKind2D::Aabb:
                        worldBatcher.PushBoxOutline(Rect{cx - hw, cy - hh, hw * 2.0f, hh * 2.0f}, 0.04f, wireCol);
                        break;
                    case ShapeKind2D::Circle:
                        worldBatcher.PushCircleOutline(cx, cy, hw, 16U, 0.04f, wireCol);
                        break;
                    case ShapeKind2D::CapsuleY:
                        worldBatcher.PushCapsuleYOutline(cx, cy, hw, hh, 0.04f, wireCol);
                        break;
                    case ShapeKind2D::Obb:
                        worldBatcher.PushObbOutline({cx, cy}, {hw, hh}, wt.cosRot, wt.sinRot, 0.04f, wireCol);
                        break;
                    case ShapeKind2D::Segment: {
                        const Vec2 p0 = wt.TransformPoint(col.localBounds.SegmentP0());
                        const Vec2 p1 = wt.TransformPoint(col.localBounds.SegmentP1());
                        worldBatcher.PushLine(p0.x, p0.y, p1.x, p1.y, 0.05f, wireCol);
                        break;
                    }
                }
            }
        }

        if (HasFlag(flags, OverlayFlags::VelocityVectors)) {
            auto drawVel = [&](EntityId e, f32 vx, f32 vy, Color col) noexcept {
                if ((vx * vx + vy * vy) > (0.05f * 0.05f)) {
                    const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                    worldBatcher.PushArrow(wt.x, wt.y, wt.x + vx * 0.15f, wt.y + vy * 0.15f, 0.18f, 0.04f, col);
                }
            };
            if ((regMask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
                const auto& rbPool = world.GetPool<RigidBody2D>();
                for (u16 i = 0U; i < rbPool.Size(); ++i) {
                    const RigidBody2D& rb = rbPool.ComponentAtDense(i);
                    drawVel(rbPool.Entities()[i], rb.vx, rb.vy, Color{1.0f, 0.55f, 0.1f, 0.9f});
                }
            }
            if ((regMask & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
                const auto& motorPool = world.GetPool<CharacterMotor2D>();
                for (u16 i = 0U; i < motorPool.Size(); ++i) {
                    const CharacterMotor2D& m = motorPool.ComponentAtDense(i);
                    drawVel(motorPool.Entities()[i], m.vx, m.vy, Color{0.2f, 0.9f, 1.0f, 0.9f});
                }
            }
        }

        if (HasFlag(flags, OverlayFlags::CombatHitboxes) &&
            (regMask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
            const bool hasMotion = (regMask & MakeComponentMask<MotionStack2D>()) != 0ULL;
            const auto& hbPool = world.GetPool<HitboxRig2D>();
            auto drawBox = [&](const WorldTransform2D& wt, const CompactAabb2D& b, f32 facing, f32 thick, Color col) noexcept {
                const f32 cx = wt.x + b.OffsetX() * facing;
                const f32 cy = wt.y + b.OffsetY();
                const f32 hw = b.HalfW();
                const f32 hh = b.HalfH();
                worldBatcher.PushBoxOutline(Rect{cx - hw, cy - hh, hw * 2.0f, hh * 2.0f}, thick, col);
            };
            for (u16 i = 0U; i < hbPool.Size(); ++i) {
                const EntityId e = hbPool.Entities()[i];
                const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                const HitboxRig2D& hb = hbPool.ComponentAtDense(i);
                const f32 facing = (hasMotion && (world.GetComponentMaskByIndex(e.index) & MakeComponentMask<MotionStack2D>()) != 0ULL)
                                       ? static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(e.index).FacingSign())
                                       : 1.0f;

                if (HasFlag(hb.flags, HitboxFlags::HurtboxActive)) {
                    drawBox(wt, hb.hurtbox, facing, 0.04f, Color{0.1f, 0.95f, 0.35f, 0.85f});
                }
                if (HasFlag(hb.flags, HitboxFlags::HitboxActive)) {
                    drawBox(wt, hb.hitbox, facing, 0.05f, Color{1.0f, 0.15f, 0.2f, 0.95f});
                }
            }
        }

        if (HasFlag(flags, OverlayFlags::SafeAnchors) &&
            (regMask & MakeComponentMask<SafeAnchor2D>()) != 0ULL) {
            const auto& saPool = world.GetPool<SafeAnchor2D>();
            for (u16 i = 0U; i < saPool.Size(); ++i) {
                const SafeAnchor2D& sa = saPool.ComponentAtDense(i);
                worldBatcher.PushCrosshair(sa.safeX, sa.safeY, 0.3f, 0.05f, Color{0.2f, 1.0f, 0.6f, 0.9f});
            }
        }

        if (HasFlag(flags, OverlayFlags::CameraRigOverlay) &&
            (regMask & MakeComponentMask<CameraController2D>()) != 0ULL) {
            const auto& camPool = world.GetPool<CameraController2D>();
            for (u16 i = 0U; i < camPool.Size(); ++i) {
                const CameraController2D& cc = camPool.ComponentAtDense(i);
                const f32 dzW = cc.DeadzoneHalfW();
                const f32 dzH = cc.DeadzoneHalfH();
                if (dzW > 0.0f && dzH > 0.0f) {
                    worldBatcher.PushBoxOutline(
                        Rect{cc.anchorX - dzW, cc.anchorY - dzH, dzW * 2.0f, dzH * 2.0f},
                        0.04f, Color{1.0f, 0.8f, 0.2f, 0.75f});
                }
            }
        }
    }

    if (HasFlag(flags, OverlayFlags::SpatialBroadphase) && ctx.physics) {
        const SpatialIndex2D& sp = ctx.physics->Spatial();
        for (const SpatialBox2D& b : sp.ActiveBoxes()) {
            const f32 minX = b.BoundMinX();
            const f32 minY = b.BoundMinY();
            worldBatcher.PushBoxOutline(
                Rect{minX, minY, b.BoundMaxX() - minX, b.BoundMaxY() - minY},
                0.025f, Color{0.55f, 0.55f, 0.65f, 0.45f});
        }
        const u16 tw = std::min<u16>(sp.WidthTiles(), 64U);
        const u16 th = std::min<u16>(sp.HeightTiles(), 64U);
        const f32 ts = sp.TileSizeMeters();
        for (u16 ty = 0U; ty < th; ++ty) {
            for (u16 tx = 0U; tx < tw; ++tx) {
                const TileMaterialFlags tf = sp.SampleTileCoord(tx, ty);
                if (tf == TileMaterialFlags::Empty) {
                    continue;
                }
                const f32 wx = sp.OriginX() + static_cast<f32>(tx) * ts;
                const f32 wy = sp.OriginY() + static_cast<f32>(ty) * ts;
                const Color tCol = HasFlag(tf, TileMaterialFlags::OneWay)
                                       ? Color{1.0f, 0.7f, 0.2f, 0.55f}
                                       : Color{0.35f, 0.65f, 0.95f, 0.55f};
                worldBatcher.PushBoxOutline(Rect{wx, wy, ts, ts}, 0.03f, tCol);
            }
        }
    }

    if (HasFlag(flags, OverlayFlags::ContactManifolds) && ctx.physics) {
        for (const ContactManifold2D& m : ctx.physics->ActiveManifolds()) {
            for (u8 p = 0U; p < m.pointCount; ++p) {
                const f32 px = m.pointX[p];
                const f32 py = m.pointY[p];
                worldBatcher.PushCrosshair(px, py, 0.12f, 0.03f, Color{1.0f, 0.2f, 0.9f, 1.0f});
                worldBatcher.PushArrow(
                    px, py, px + m.normalX * 0.4f, py + m.normalY * 0.4f,
                    0.12f, 0.03f, Color{1.0f, 0.95f, 0.2f, 1.0f});
            }
        }
    }

    if (HasFlag(flags, OverlayFlags::AudioEmitters)) {
        for (const AudioEmitterView& em : ctx.audioEmitters) {
            const Color col = (em.occlusionFactor > 0.5f)
                                  ? Color{1.0f, 0.4f, 0.2f, 0.7f}
                                  : Color{0.3f, 0.85f, 1.0f, 0.7f};
            worldBatcher.PushCircleOutline(em.worldX, em.worldY, em.maxDist, 20U, 0.04f, col);
        }
    }

    if (HasFlag(flags, OverlayFlags::EntityInspector | OverlayFlags::FlightScrubber)) {
        if (const ActorTickRecord* rec = InspectedRecord()) {
            const f32 panelX = static_cast<f32>(ctx.viewportSize.w) - 310.0f;
            const f32 panelY = 12.0f;
            hudBatcher.PushSolidRect(Rect{panelX, panelY, 298.0f, 108.0f}, Color{0.06f, 0.08f, 0.12f, 0.85f});
            hudBatcher.PushBoxOutline(Rect{panelX, panelY, 298.0f, 108.0f}, 1.0f, Color{0.3f, 0.75f, 1.0f, 0.9f});

            char buf[96]{};
            char* const end = buf + sizeof(buf);

            char* p = DebugBatcher::AppendStr(buf, end, "ENT #");
            p = DebugBatcher::FormatI64(p, end, rec->entityIndex);
            p = DebugBatcher::AppendStr(p, end, " TICK ");
            p = DebugBatcher::FormatI64(p, end, rec->tickIndex);
            hudBatcher.PushText(panelX + 8.0f, panelY + 8.0f, {buf, static_cast<usize>(p - buf)}, Color{1.0f, 0.9f, 0.2f, 1.0f}, 1.25f);

            p = DebugBatcher::AppendStr(buf, end, "POS (");
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->worldX);
            p = DebugBatcher::AppendStr(p, end, ", ");
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->worldY);
            p = DebugBatcher::AppendStr(p, end, ")");
            hudBatcher.PushText(panelX + 8.0f, panelY + 26.0f, {buf, static_cast<usize>(p - buf)}, Color{0.9f, 0.95f, 1.0f, 1.0f}, 1.0f);

            p = DebugBatcher::AppendStr(buf, end, "VEL (");
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->velX);
            p = DebugBatcher::AppendStr(p, end, ", ");
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->velY);
            p = DebugBatcher::AppendStr(p, end, ")");
            hudBatcher.PushText(panelX + 8.0f, panelY + 42.0f, {buf, static_cast<usize>(p - buf)}, Color{0.7f, 0.9f, 1.0f, 1.0f}, 1.0f);

            p = DebugBatcher::AppendStr(buf, end, "STATE ");
            p = DebugBatcher::FormatI64(p, end, rec->stateId);
            p = DebugBatcher::AppendStr(p, end, " HP ");
            p = DebugBatcher::FormatI64(p, end, rec->hp);
            p = DebugBatcher::AppendStr(p, end, " IFR ");
            p = DebugBatcher::FormatI64(p, end, rec->iframeTicks);
            hudBatcher.PushText(panelX + 8.0f, panelY + 58.0f, {buf, static_cast<usize>(p - buf)}, Color{0.4f, 1.0f, 0.6f, 1.0f}, 1.0f);

            p = DebugBatcher::AppendStr(buf, end, "SCRUB -");
            p = DebugBatcher::FormatI64(p, end, m_scrubOffset);
            p = DebugBatcher::AppendStr(p, end, " / ");
            p = DebugBatcher::FormatI64(p, end, m_validCount);
            hudBatcher.PushText(panelX + 8.0f, panelY + 76.0f, {buf, static_cast<usize>(p - buf)}, Color{1.0f, 0.7f, 0.3f, 1.0f}, 1.0f);
        }
    }

    if (HasFlag(flags, OverlayFlags::InputViewerHud)) {
        if (const ActorTickRecord* rec = InspectedRecord()) {
            const f32 baseX = static_cast<f32>(ctx.viewportSize.w) - 310.0f;
            const f32 baseY = 128.0f;
            hudBatcher.PushSolidRect(Rect{baseX, baseY, 298.0f, 76.0f}, Color{0.05f, 0.07f, 0.11f, 0.85f});
            hudBatcher.PushBoxOutline(Rect{baseX, baseY, 298.0f, 76.0f}, 1.0f, Color{0.45f, 0.85f, 0.65f, 0.9f});

            const f32 gateCx = baseX + 38.0f;
            const f32 gateCy = baseY + 38.0f;
            hudBatcher.PushBoxOutline(Rect{gateCx - 24.0f, gateCy - 24.0f, 48.0f, 48.0f}, 1.0f, Color{0.4f, 0.5f, 0.6f, 0.8f});
            const f32 ax = static_cast<f32>(rec->axisX_q7) * (1.0f / 127.0f);
            const f32 ay = static_cast<f32>(rec->axisY_q7) * (1.0f / 127.0f);
            hudBatcher.PushCrosshair(gateCx + ax * 22.0f, gateCy - ay * 22.0f, 5.0f, 2.0f, Color{1.0f, 0.9f, 0.2f, 1.0f});

            for (u32 b = 0U; b < 8U; ++b) {
                const u8 bit = static_cast<u8>(1U << b);
                Color ledCol{0.18f, 0.22f, 0.28f, 0.9f};
                if ((rec->inputPressed & bit) != 0U) {
                    ledCol = Color{1.0f, 0.95f, 0.25f, 1.0f};
                } else if ((rec->inputHeld & bit) != 0U) {
                    ledCol = Color{0.25f, 0.95f, 0.55f, 1.0f};
                } else if ((rec->actionBufferMask & bit) != 0U) {
                    ledCol = Color{0.35f, 0.75f, 1.0f, 0.9f};
                }

                const f32 bx = baseX + 82.0f + static_cast<f32>(b & 3U) * 24.0f;
                const f32 by = baseY + 16.0f + static_cast<f32>(b >> 2U) * 24.0f;
                hudBatcher.PushSolidRect(Rect{bx, by, 18.0f, 18.0f}, ledCol);
            }
        }
    }
}

} // namespace en

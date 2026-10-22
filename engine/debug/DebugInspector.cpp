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
    if (!m_history || !m_pinnedEntity.IsValid() || !world.IsAlive(m_pinnedEntity)) return;

    const u16 eIdx = m_pinnedEntity.index;
    const u64 mask = world.GetComponentMaskByIndex(eIdx);
    const u64 regMask = world.RegisteredPoolsMask();
    const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);

    ActorTickRecord rec{};
    rec.tickIndex = tickIndex;
    rec.entityId = m_pinnedEntity;
    rec.worldX = wt.x;
    rec.worldY = wt.y;

    if ((regMask & MakeComponentMask<RigidBody2D>()) != 0ULL &&
        (mask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
        const RigidBody2D& rb = world.GetPool<RigidBody2D>().DirectAtEntityIndex(eIdx);
        rec.velX = rb.vx;
        rec.velY = rb.vy;
    }

    if ((regMask & MakeComponentMask<CharacterMotor2D>()) != 0ULL &&
        (mask & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
        const CharacterMotor2D& m = world.GetPool<CharacterMotor2D>().DirectAtEntityIndex(eIdx);
        rec.velX = m.vx;
        rec.velY = m.vy;
        rec.contactFlags = static_cast<u8>(m.contacts);
    }

    if ((regMask & MakeComponentMask<MotionStack2D>()) != 0ULL &&
        (mask & MakeComponentMask<MotionStack2D>()) != 0ULL) {
        const MotionStack2D& ms = world.GetPool<MotionStack2D>().DirectAtEntityIndex(eIdx);
        rec.groundCoyoteTicks = ms.groundCoyoteTicks;
    }

    if ((regMask & MakeComponentMask<StateMachine2D>()) != 0ULL &&
        (mask & MakeComponentMask<StateMachine2D>()) != 0ULL) {
        const StateMachine2D& sm = world.GetPool<StateMachine2D>().DirectAtEntityIndex(eIdx);
        rec.stateId = sm.stateId;
        rec.bufferedActionId = sm.bufferedActionId;
        rec.stateTicks = sm.stateTicks;
        rec.hitstopTicks = sm.hitstopTicks;
    }

    if ((regMask & MakeComponentMask<HitboxRig2D>()) != 0ULL &&
        (mask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
        const HitboxRig2D& hb = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(eIdx);
        rec.hurtboxQ8 = std::array<i16, 4>{
            hb.hurtbox.offsetX_q8,
            hb.hurtbox.offsetY_q8,
            static_cast<i16>(hb.hurtbox.halfW_q8),
            static_cast<i16>(hb.hurtbox.halfH_q8),
        };
        if (HasPhysicsFlag(hb.flags, HitboxFlags::HitboxActive)) {
            rec.hitboxQ8 = std::array<i16, 4>{
                hb.hitbox.offsetX_q8,
                hb.hitbox.offsetY_q8,
                static_cast<i16>(hb.hitbox.halfW_q8),
                static_cast<i16>(hb.hitbox.halfH_q8),
            };
        }
    }

    if ((regMask & MakeComponentMask<DamageReceiver2D>()) != 0ULL &&
        (mask & MakeComponentMask<DamageReceiver2D>()) != 0ULL) {
        const DamageReceiver2D& dr = world.GetPool<DamageReceiver2D>().DirectAtEntityIndex(eIdx);
        rec.hp = dr.hp;
        rec.iframeTicks = static_cast<u8>(std::min<u16>(255U, dr.iframeTicks));
    }

    if (tickInput) {
        rec.inputHeld = tickInput->held;
        rec.inputPressed = tickInput->pressed;
        rec.inputReleased = tickInput->released;
        rec.axisX_q8 = CompactAabb2D::QuantizeS16(tickInput->axis[0]);
        rec.axisY_q8 = CompactAabb2D::QuantizeS16(tickInput->axis[1]);
    }
    if (actionBuffer) {
        rec.actionBufferMask = actionBuffer->PendingPressMask();
    }

    const u16 slot = m_writeIdx & (kActorFlightHistoryTicks - 1U);
    m_history[slot] = rec;
    m_writeIdx = static_cast<u16>((slot + 1U) & (kActorFlightHistoryTicks - 1U));
    if (m_validCount < kActorFlightHistoryTicks) {
        ++m_validCount;
    }
}

const ActorTickRecord* DebugInspector::InspectedRecord() const noexcept {
    if (!m_history || m_validCount == 0U) return nullptr;
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
    const bool hasColliderPool = (regMask & MakeComponentMask<Collider2D>()) != 0ULL;
    const bool hasHitboxPool = (regMask & MakeComponentMask<HitboxRig2D>()) != 0ULL;

    EntityId bestEntity = kNullEntity;
    f32 bestDistSq = pickRadius * pickRadius;

    for (usize w = 0U; w < aliveWords.size(); ++w) {
        u64 bits = aliveWords[w];
        while (bits != 0ULL) {
            const u32 b = static_cast<u32>(std::countr_zero(bits));
            bits &= (bits - 1ULL);
            const u16 eIdx = static_cast<u16>((w << 6U) + b);
            if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) continue;

            const EntityId e = world.EntityAtSlot(eIdx);
            const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
            const u64 mask = world.GetComponentMaskByIndex(eIdx);

            if (hasColliderPool && (mask & MakeComponentMask<Collider2D>()) != 0ULL) {
                const Collider2D& col = world.GetPool<Collider2D>().DirectAtEntityIndex(eIdx);
                if (col.shape != ShapeKind2D::Segment) {
                    const Vec2 c = wt.TransformPoint({col.localBounds.OffsetX(), col.localBounds.OffsetY()});
                    const f32 cx = c.x;
                    const f32 cy = c.y;
                    const f32 hw = col.localBounds.HalfW() * std::abs(wt.scaleX);
                    const f32 hh = (col.shape == ShapeKind2D::Circle) ? hw : (col.localBounds.HalfH() * std::abs(wt.scaleY));
                    if (worldPos.x >= cx - hw && worldPos.x <= cx + hw &&
                        worldPos.y >= cy - hh && worldPos.y <= cy + hh) {
                        const f32 dSq = (worldPos.x - cx) * (worldPos.x - cx) + (worldPos.y - cy) * (worldPos.y - cy);
                        if (dSq <= bestDistSq) {
                            bestDistSq = dSq;
                            bestEntity = e;
                        }
                        continue;
                    }
                }
            } else if (hasHitboxPool && (mask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
                const HitboxRig2D& hb = world.GetPool<HitboxRig2D>().DirectAtEntityIndex(eIdx);
                const f32 cx = wt.x + hb.hurtbox.OffsetX();
                const f32 cy = wt.y + hb.hurtbox.OffsetY();
                const f32 hw = hb.hurtbox.HalfW();
                const f32 hh = hb.hurtbox.HalfH();
                if (worldPos.x >= cx - hw && worldPos.x <= cx + hw &&
                    worldPos.y >= cy - hh && worldPos.y <= cy + hh) {
                    const f32 dSq = (worldPos.x - cx) * (worldPos.x - cx) + (worldPos.y - cy) * (worldPos.y - cy);
                    if (dSq <= bestDistSq) {
                        bestDistSq = dSq;
                        bestEntity = e;
                    }
                    continue;
                }
            }

            const f32 dx = worldPos.x - wt.x;
            const f32 dy = worldPos.y - wt.y;
            const f32 dSq = dx * dx + dy * dy;
            if (dSq < bestDistSq) {
                bestDistSq = dSq;
                bestEntity = e;
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
    const Vec2 worldPos = worldCam.ViewportToWorld(screenPos, viewportSize);
    return PickEntityAtWorldPos(world, worldPos, 1.25f);
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

        // 1. Spatial transforms crosshairs
        if (HasOverlay(flags, OverlayFlags::SpatialTransforms)) {
            for (usize w = 0U; w < aliveWords.size(); ++w) {
                u64 bits = aliveWords[w];
                while (bits != 0ULL) {
                    const u32 b = static_cast<u32>(std::countr_zero(bits));
                    bits &= (bits - 1ULL);
                    const u16 eIdx = static_cast<u16>((w << 6U) + b);
                    if (eIdx == 0U || eIdx > world.MaxAllocatedIndex()) continue;
                    const EntityId e = world.EntityAtSlot(eIdx);
                    const WorldTransform2D& wt = world.GetWorldByIndex(eIdx);
                    const Color col = (e == m_pinnedEntity)
                                          ? Color{1.0f, 0.9f, 0.1f, 1.0f}
                                          : Color{0.2f, 0.85f, 1.0f, 0.85f};
                    worldBatcher.PushCrosshair(wt.x, wt.y, 0.25f, 0.04f, col);
                }
            }
        }

        // 2. Physics colliders (color-coded by Static, Kinematic, Dynamic Awake, Sleeping, Sensor, OneWay)
        if (HasOverlay(flags, OverlayFlags::PhysicsColliders) &&
            (regMask & MakeComponentMask<Collider2D>()) != 0ULL) {
            const bool hasRbPool = (regMask & MakeComponentMask<RigidBody2D>()) != 0ULL;
            const auto& colPool = world.GetPool<Collider2D>();
            for (u16 i = 0U; i < colPool.Size(); ++i) {
                const EntityId e = colPool.Entities()[i];
                const u64 eMask = world.GetComponentMaskByIndex(e.index);
                const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                const Collider2D& col = colPool.ComponentAtDense(i);

                Color wireCol{0.45f, 0.75f, 0.95f, 0.85f}; // Static default
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

        // 3. Physics DistanceJoint2D springs and rods
        if (HasOverlay(flags, OverlayFlags::PhysicsJoints) &&
            (regMask & MakeComponentMask<DistanceJoint2D>()) != 0ULL) {
            const auto& jointPool = world.GetPool<DistanceJoint2D>();
            for (u16 i = 0U; i < jointPool.Size(); ++i) {
                const DistanceJoint2D& j = jointPool.ComponentAtDense(i);
                if (j.enabled == 0U || !world.IsAlive(j.entityA) || !world.IsAlive(j.entityB)) continue;
                const Vec2 worldA = world.GetWorldByIndex(j.entityA.index).TransformPoint(j.LocalAnchorA());
                const Vec2 worldB = world.GetWorldByIndex(j.entityB.index).TransformPoint(j.LocalAnchorB());
                const Color jCol = (j.hertz_q6 > 0U)
                                       ? Color{0.95f, 0.65f, 0.2f, 0.9f}
                                       : Color{0.8f, 0.85f, 0.95f, 0.9f};
                worldBatcher.PushLine(worldA.x, worldA.y, worldB.x, worldB.y, 0.04f, jCol);
                worldBatcher.PushCrosshair(worldA.x, worldA.y, 0.1f, 0.03f, jCol);
                worldBatcher.PushCrosshair(worldB.x, worldB.y, 0.1f, 0.03f, jCol);
            }
        }

        // 4. Velocity vectors (RigidBody2D & CharacterMotor2D)
        if (HasOverlay(flags, OverlayFlags::VelocityVectors)) {
            if ((regMask & MakeComponentMask<RigidBody2D>()) != 0ULL) {
                const auto& rbPool = world.GetPool<RigidBody2D>();
                for (u16 i = 0U; i < rbPool.Size(); ++i) {
                    const EntityId e = rbPool.Entities()[i];
                    const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                    const RigidBody2D& rb = rbPool.ComponentAtDense(i);
                    if ((rb.vx * rb.vx + rb.vy * rb.vy) > (0.05f * 0.05f)) {
                        worldBatcher.PushArrow(
                            wt.x, wt.y, wt.x + rb.vx * 0.15f, wt.y + rb.vy * 0.15f,
                            0.18f, 0.04f, Color{1.0f, 0.55f, 0.1f, 0.9f});
                    }
                }
            }
            if ((regMask & MakeComponentMask<CharacterMotor2D>()) != 0ULL) {
                const auto& motorPool = world.GetPool<CharacterMotor2D>();
                for (u16 i = 0U; i < motorPool.Size(); ++i) {
                    const EntityId e = motorPool.Entities()[i];
                    const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                    const CharacterMotor2D& m = motorPool.ComponentAtDense(i);
                    if ((m.vx * m.vx + m.vy * m.vy) > (0.05f * 0.05f)) {
                        worldBatcher.PushArrow(
                            wt.x, wt.y, wt.x + m.vx * 0.15f, wt.y + m.vy * 0.15f,
                            0.18f, 0.04f, Color{0.2f, 0.9f, 1.0f, 0.9f});
                    }
                }
            }
        }

        // 5. Combat Hurtboxes & Active Attack Hitboxes
        if (HasOverlay(flags, OverlayFlags::CombatHitboxes) &&
            (regMask & MakeComponentMask<HitboxRig2D>()) != 0ULL) {
            const bool hasMotion = (regMask & MakeComponentMask<MotionStack2D>()) != 0ULL;
            const auto& hbPool = world.GetPool<HitboxRig2D>();
            for (u16 i = 0U; i < hbPool.Size(); ++i) {
                const EntityId e = hbPool.Entities()[i];
                const u64 mask = world.GetComponentMaskByIndex(e.index);
                const WorldTransform2D& wt = world.GetWorldByIndex(e.index);
                const HitboxRig2D& hb = hbPool.ComponentAtDense(i);
                const f32 facing = (hasMotion && (mask & MakeComponentMask<MotionStack2D>()) != 0ULL)
                                       ? static_cast<f32>(world.GetPool<MotionStack2D>().DirectAtEntityIndex(e.index).FacingSign())
                                       : 1.0f;

                if (HasPhysicsFlag(hb.flags, HitboxFlags::HurtboxActive)) {
                    const f32 hCx = wt.x + hb.hurtbox.OffsetX() * facing;
                    const f32 hCy = wt.y + hb.hurtbox.OffsetY();
                    const f32 hHw = hb.hurtbox.HalfW();
                    const f32 hHh = hb.hurtbox.HalfH();
                    worldBatcher.PushBoxOutline(
                        Rect{hCx - hHw, hCy - hHh, hHw * 2.0f, hHh * 2.0f},
                        0.04f, Color{0.1f, 0.95f, 0.35f, 0.85f});
                }

                if (HasPhysicsFlag(hb.flags, HitboxFlags::HitboxActive)) {
                    const f32 aCx = wt.x + hb.hitbox.OffsetX() * facing;
                    const f32 aCy = wt.y + hb.hitbox.OffsetY();
                    const f32 aHw = hb.hitbox.HalfW();
                    const f32 aHh = hb.hitbox.HalfH();
                    worldBatcher.PushBoxOutline(
                        Rect{aCx - aHw, aCy - aHh, aHw * 2.0f, aHh * 2.0f},
                        0.05f, Color{1.0f, 0.15f, 0.2f, 0.95f});
                }
            }
        }

        // 6. Safe anchors & Camera Rig overlay
        if (HasOverlay(flags, OverlayFlags::SafeAnchors) &&
            (regMask & MakeComponentMask<SafeAnchor2D>()) != 0ULL) {
            const auto& saPool = world.GetPool<SafeAnchor2D>();
            for (u16 i = 0U; i < saPool.Size(); ++i) {
                const SafeAnchor2D& sa = saPool.ComponentAtDense(i);
                worldBatcher.PushCrosshair(sa.safeX, sa.safeY, 0.3f, 0.05f, Color{0.2f, 1.0f, 0.6f, 0.9f});
            }
        }

        if (HasOverlay(flags, OverlayFlags::CameraRigOverlay) &&
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

    // 7. Spatial broadphase proxies & Tilemap collision grid
    if (HasOverlay(flags, OverlayFlags::SpatialBroadphase) && ctx.physics) {
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
                if (tf == TileMaterialFlags::Empty) continue;
                const f32 wx = sp.OriginX() + static_cast<f32>(tx) * ts;
                const f32 wy = sp.OriginY() + static_cast<f32>(ty) * ts;
                const Color tCol = HasPhysicsFlag(tf, TileMaterialFlags::OneWay)
                                       ? Color{1.0f, 0.7f, 0.2f, 0.55f}
                                       : Color{0.35f, 0.65f, 0.95f, 0.55f};
                worldBatcher.PushBoxOutline(Rect{wx, wy, ts, ts}, 0.03f, tCol);
            }
        }
    }

    // 8. Physics contact manifolds
    if (HasOverlay(flags, OverlayFlags::ContactManifolds) && ctx.physics) {
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

    // 9. Audio emitters
    if (HasOverlay(flags, OverlayFlags::AudioEmitters)) {
        for (const AudioEmitterView& em : ctx.audioEmitters) {
            const Color col = (em.occlusionFactor > 0.5f)
                                  ? Color{1.0f, 0.4f, 0.2f, 0.7f}
                                  : Color{0.3f, 0.85f, 1.0f, 0.7f};
            worldBatcher.PushCircleOutline(em.worldX, em.worldY, em.maxDist, 20U, 0.04f, col);
        }
    }

    // 10. Pinned Entity Inspector & Flight Scrubber HUD
    if (HasOverlay(flags, OverlayFlags::EntityInspector) || HasOverlay(flags, OverlayFlags::FlightScrubber)) {
        if (const ActorTickRecord* rec = InspectedRecord()) {
            const f32 panelX = static_cast<f32>(ctx.viewportSize.w) - 310.0f;
            const f32 panelY = 12.0f;
            hudBatcher.PushSolidRect(Rect{panelX, panelY, 298.0f, 108.0f}, Color{0.06f, 0.08f, 0.12f, 0.85f});
            hudBatcher.PushBoxOutline(Rect{panelX, panelY, 298.0f, 108.0f}, 1.0f, Color{0.3f, 0.75f, 1.0f, 0.9f});

            char buf[96]{};
            char* end = buf + sizeof(buf);

            char* p = buf;
            std::memcpy(p, "ENT #", 5U); p += 5;
            p = DebugBatcher::FormatI64(p, end, rec->entityId.index);
            std::memcpy(p, " TICK ", 6U); p += 6;
            p = DebugBatcher::FormatI64(p, end, rec->tickIndex);
            hudBatcher.PushText(panelX + 8.0f, panelY + 8.0f, {buf, static_cast<usize>(p - buf)}, Color{1.0f, 0.9f, 0.2f, 1.0f}, 1.25f);

            p = buf;
            std::memcpy(p, "POS (", 5U); p += 5;
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->worldX);
            std::memcpy(p, ", ", 2U); p += 2;
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->worldY);
            *p++ = ')';
            hudBatcher.PushText(panelX + 8.0f, panelY + 26.0f, {buf, static_cast<usize>(p - buf)}, Color{0.9f, 0.95f, 1.0f, 1.0f}, 1.0f);

            p = buf;
            std::memcpy(p, "VEL (", 5U); p += 5;
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->velX);
            std::memcpy(p, ", ", 2U); p += 2;
            p = DebugBatcher::FormatF32Fixed2(p, end, rec->velY);
            *p++ = ')';
            hudBatcher.PushText(panelX + 8.0f, panelY + 42.0f, {buf, static_cast<usize>(p - buf)}, Color{0.7f, 0.9f, 1.0f, 1.0f}, 1.0f);

            p = buf;
            std::memcpy(p, "STATE ", 6U); p += 6;
            p = DebugBatcher::FormatI64(p, end, rec->stateId);
            std::memcpy(p, " HP ", 4U); p += 4;
            p = DebugBatcher::FormatI64(p, end, rec->hp);
            std::memcpy(p, " IFR ", 5U); p += 5;
            p = DebugBatcher::FormatI64(p, end, rec->iframeTicks);
            hudBatcher.PushText(panelX + 8.0f, panelY + 58.0f, {buf, static_cast<usize>(p - buf)}, Color{0.4f, 1.0f, 0.6f, 1.0f}, 1.0f);

            p = buf;
            std::memcpy(p, "SCRUB -", 7U); p += 7;
            p = DebugBatcher::FormatI64(p, end, m_scrubOffset);
            std::memcpy(p, " / ", 3U); p += 3;
            p = DebugBatcher::FormatI64(p, end, m_validCount);
            hudBatcher.PushText(panelX + 8.0f, panelY + 76.0f, {buf, static_cast<usize>(p - buf)}, Color{1.0f, 0.7f, 0.3f, 1.0f}, 1.0f);
        }
    }

    // 11. 2D Arcade Stick Gate & Input Button LED Viewer HUD
    if (HasOverlay(flags, OverlayFlags::InputViewerHud)) {
        if (const ActorTickRecord* rec = InspectedRecord()) {
            const f32 baseX = static_cast<f32>(ctx.viewportSize.w) - 310.0f;
            const f32 baseY = 128.0f;
            hudBatcher.PushSolidRect(Rect{baseX, baseY, 298.0f, 76.0f}, Color{0.05f, 0.07f, 0.11f, 0.85f});
            hudBatcher.PushBoxOutline(Rect{baseX, baseY, 298.0f, 76.0f}, 1.0f, Color{0.45f, 0.85f, 0.65f, 0.9f});

            // Stick gate box + live stick knob crosshair
            const f32 gateCx = baseX + 38.0f;
            const f32 gateCy = baseY + 38.0f;
            hudBatcher.PushBoxOutline(Rect{gateCx - 24.0f, gateCy - 24.0f, 48.0f, 48.0f}, 1.0f, Color{0.4f, 0.5f, 0.6f, 0.8f});
            const f32 ax = static_cast<f32>(rec->axisX_q8) * (1.0f / 256.0f);
            const f32 ay = static_cast<f32>(rec->axisY_q8) * (1.0f / 256.0f);
            hudBatcher.PushCrosshair(gateCx + ax * 22.0f, gateCy - ay * 22.0f, 5.0f, 2.0f, Color{1.0f, 0.9f, 0.2f, 1.0f});

            // 8 primary action button LEDs (2 rows of 4)
            for (u32 b = 0U; b < 8U; ++b) {
                const u16 bit = static_cast<u16>(1U << b);
                const bool pressed = (rec->inputPressed & bit) != 0U;
                const bool held = (rec->inputHeld & bit) != 0U;
                const bool buffered = (rec->actionBufferMask & bit) != 0U;
                Color ledCol{0.18f, 0.22f, 0.28f, 0.9f};
                if (pressed) ledCol = Color{1.0f, 0.95f, 0.25f, 1.0f};
                else if (held) ledCol = Color{0.25f, 0.95f, 0.55f, 1.0f};
                else if (buffered) ledCol = Color{0.35f, 0.75f, 1.0f, 0.9f};

                const f32 bx = baseX + 82.0f + static_cast<f32>(b & 3U) * 24.0f;
                const f32 by = baseY + 16.0f + static_cast<f32>(b >> 2U) * 24.0f;
                hudBatcher.PushSolidRect(Rect{bx, by, 18.0f, 18.0f}, ledCol);
            }
        }
    }
}

u64 DebugInspector::ComputeAuditHash() const noexcept {
    u64 h = 14695981039346656037ULL;
    h ^= static_cast<u64>(m_pinnedEntity.Packed());
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_validCount);
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_scrubOffset);
    h *= 1099511628211ULL;
    if (const ActorTickRecord* rec = InspectedRecord()) {
        h ^= static_cast<u64>(rec->tickIndex);
        h *= 1099511628211ULL;
        h ^= static_cast<u64>(rec->stateId);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace en

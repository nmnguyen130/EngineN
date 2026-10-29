#include "world/WorldSystems.h"

namespace en {

void WorldSystems::StepVelocities(World& world, const TickStep& step) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << Velocity2D::kTypeId)) == 0ULL) return;
    auto& velPool       = world.GetPool<Velocity2D>();
    const auto entities = velPool.Entities();
    auto vels           = velPool.Components();

    for (usize i = 0U; i < entities.size(); ++i) {
        const u16 idx = entities[i].index;
        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx));
        if (step.IsFrozen(ch)) continue;
        const f32 dt    = step.Dt(ch);
        Velocity2D& vel = vels[i];
        if (vel.linearDamping > 0.0f) {
            const f32 damp = std::max(0.0f, 1.0f - vel.linearDamping * dt);
            vel.vx *= damp;
            vel.vy *= damp;
        }
        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(idx);
        loc.x           += vel.vx * dt;
        loc.y           += vel.vy * dt;
        loc.rotationRad += vel.angularVelRad * dt;
    }
}

void WorldSystems::StepVisualsAndLifetimes(World& world, const TickStep& step) noexcept {
    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
        auto& animPool      = world.GetPool<SpriteAnimator2D>();
        const auto entities = animPool.Entities();
        auto anims          = animPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            SpriteAnimator2D& anim = anims[i];
            if (!anim.playing || anim.frameCount <= 1U) continue;
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(entities[i].index));
            if (step.IsFrozen(ch)) continue;
            if (++anim.tickAccum >= std::max<u16>(1U, anim.ticksPerFrame)) {
                anim.tickAccum = 0U;
                if (anim.currentFrame + 1U < anim.frameCount) {
                    ++anim.currentFrame;
                } else if (anim.loop) {
                    anim.currentFrame = 0U;
                } else {
                    anim.playing = false;
                }
            }
        }
    }

    if ((regMask & (1ULL << Lifetime2D::kTypeId)) != 0ULL) {
        auto& lifePool      = world.GetPool<Lifetime2D>();
        auto& spritePool    = world.GetPool<Sprite2D>();
        const auto entities = lifePool.Entities();
        auto lives          = lifePool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            Lifetime2D& life = lives[i];
            if (!life.active) continue;
            const EntityId id = entities[i];
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(id.index));
            if (step.IsFrozen(ch)) continue;
            if (life.remainingTicks > 0U) --life.remainingTicks;
            if (life.fadeSpriteAlpha && life.initialTicks > 0U) {
                life.alphaScale =
                    static_cast<f32>(life.remainingTicks) / static_cast<f32>(life.initialTicks);
                if ((world.GetComponentMaskByIndex(id.index) & (1ULL << Sprite2D::kTypeId)) != 0ULL) {
                    spritePool.DirectAtEntityIndex(id.index).tint.a = life.alphaScale;
                }
            }
            if (life.remainingTicks == 0U) {
                (void)world.DeferDestroy(id);
            }
        }
    }

    if ((regMask & (1ULL << PointLight2D::kTypeId)) != 0ULL) {
        auto& lightPool     = world.GetPool<PointLight2D>();
        const auto entities = lightPool.Entities();
        auto lights         = lightPool.Components();
        for (usize i = 0U; i < entities.size(); ++i) {
            PointLight2D& light = lights[i];
            const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(entities[i].index));
            if (!step.IsFrozen(ch) && light.pulseSpeed != 0.0f) {
                light.pulsePhase += light.pulseSpeed * step.Dt(ch);
            }
        }
    }
}

SweepHit WorldSystems::SweepMove(World& world, EntityId mover, Vec2 delta) noexcept {
    if (!world.Has<Collider2D>(mover)) return {};
    world.PropagateTransforms();

    const Collider2D& moverCol = *world.TryGet<Collider2D>(mover);
    LocalTransform2D& loc      = world.ModifyLocalTransformByIndex(mover.index);
    const Rect startBox        = moverCol.WorldBounds({loc.x, loc.y}, {loc.scaleX, loc.scaleY});

    const auto& colPool   = world.GetPool<Collider2D>();
    const auto entities   = colPool.Entities();
    const auto colliders  = colPool.Components();

    SweepHit bestHit{};
    for (usize i = 0U; i < entities.size(); ++i) {
        const EntityId other = entities[i];
        if (other == mover) continue;
        const Collider2D& col = colliders[i];
        if (col.isTrigger != 0U || (moverCol.queryMask & col.layerMask) == 0U) continue;

        const WorldTransform2D& wt = world.GetWorldByIndex(other.index);
        const Rect targetBox       = col.WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY});
        const SweepHit hit         = SweepRect(startBox, delta, targetBox);
        if (hit && hit.time < bestHit.time) {
            bestHit = hit;
        }
    }

    if (bestHit) {
        constexpr f32 kSkin = 1e-3f;
        const f32 safeTime = std::max(0.0f, bestHit.time - kSkin);
        loc.x += delta.x * safeTime;
        loc.y += delta.y * safeTime;
        const f32 rem = 1.0f - bestHit.time;
        if (rem > 0.0f) {
            const Vec2 tangent = bestHit.normal.PerpCCW();
            const f32 slideMag = delta.Dot(tangent) * rem;
            const Vec2 slide   = tangent * slideMag;
            const Rect midBox  = moverCol.WorldBounds({loc.x, loc.y}, {loc.scaleX, loc.scaleY});
            f32 slideScale = 1.0f;
            for (usize i = 0U; i < entities.size(); ++i) {
                const EntityId other = entities[i];
                if (other == mover) continue;
                const Collider2D& col = colliders[i];
                if (col.isTrigger != 0U || (moverCol.queryMask & col.layerMask) == 0U) continue;
                const WorldTransform2D& wt = world.GetWorldByIndex(other.index);
                const SweepHit sh = SweepRect(midBox, slide, col.WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY}));
                if (sh && sh.time < slideScale) slideScale = std::max(0.0f, sh.time - kSkin);
            }
            loc.x += slide.x * slideScale;
            loc.y += slide.y * slideScale;
        }
    } else {
        loc.x += delta.x;
        loc.y += delta.y;
    }
    world.PropagateTransforms();
    return bestHit;
}

u32 WorldSystems::QueryBoxOverlaps(
    const World& world,
    const Rect& queryBox,
    u16 layerMask,
    std::span<EntityId> outHits,
    EntityId ignoreEntity) noexcept {
    if (queryBox.IsEmpty() || outHits.empty() ||
        (world.RegisteredPoolsMask() & (1ULL << Collider2D::kTypeId)) == 0ULL) {
        return 0U;
    }

    const auto& colPool  = world.GetPool<Collider2D>();
    const auto entities  = colPool.Entities();
    const auto colliders = colPool.Components();
    u32 count = 0U;

    for (usize i = 0U; i < entities.size() && count < outHits.size(); ++i) {
        const EntityId id = entities[i];
        if (id == ignoreEntity) continue;
        const Collider2D& col = colliders[i];
        if ((col.layerMask & layerMask) == 0U) continue;
        const WorldTransform2D& wt = world.GetWorldByIndex(id.index);
        if (queryBox.Overlaps(col.WorldBounds({wt.x, wt.y}, {wt.scaleX, wt.scaleY}))) {
            outHits[count++] = id;
        }
    }
    return count;
}

void WorldSystems::ExtractRender(
    const World& world,
    const AssetBank& assetBank,
    const RenderStep& renderStep,
    RenderList& outRenderList,
    f32 defaultPixelsPerMeter) noexcept {
    Camera2D defaultCam{};
    defaultCam.pixelsPerMeter = defaultPixelsPerMeter;
    defaultCam.yUp            = true;
    outRenderList.SetView(0U, defaultCam);

    const u64 regMask = world.RegisteredPoolsMask();
    if ((regMask & (1ULL << CameraRig2D::kTypeId)) != 0ULL) {
        const auto& camPool    = world.GetPool<CameraRig2D>();
        const auto camEntities = camPool.Entities();
        const auto camRigs     = camPool.Components();
        const f32 camAlpha     = renderStep.Alpha(TimeChannel::Camera);

        for (usize i = 0U; i < camEntities.size(); ++i) {
            const CameraRig2D& rig = camRigs[i];
            if (!rig.active || rig.viewSlot >= RenderList::kMaxViews) continue;
            const u16 idx                  = camEntities[i].index;
            const WorldTransform2D& wt     = world.GetWorldByIndex(idx);
            const PrevWorldSnapshot2D& prv = world.GetPrevWorldByIndex(idx);

            Camera2D cam{};
            cam.viewport       = rig.viewport;
            cam.cx             = Lerp(prv.x, wt.x, camAlpha);
            cam.cy             = Lerp(prv.y, wt.y, camAlpha);
            cam.zoom           = rig.zoom;
            cam.pixelsPerMeter = (rig.pixelsPerMeter > 0.0f) ? rig.pixelsPerMeter : defaultPixelsPerMeter;
            cam.screenSpace    = rig.screenSpace;
            cam.yUp            = !rig.screenSpace;
            if (rig.trauma > 0.0f) {
                const f32 traumaSq = rig.trauma * rig.trauma;
                const f32 t        = static_cast<f32>(renderStep.tick) * 0.05f;
                f32 c = 1.0f, s = 0.0f;
                FastSinCos(t * 17.0f, c, s);
                cam.cx += s * (0.5f * traumaSq);
                FastSinCos(t * 29.0f, c, s);
                cam.cy += c * (0.5f * traumaSq);
            }
            outRenderList.SetView(rig.viewSlot, cam);
        }
    }

    const std::span<const u64> movedWords = world.MovedWords();

    if ((regMask & (1ULL << Sprite2D::kTypeId)) != 0ULL) {
        const auto& spritePool     = world.GetPool<Sprite2D>();
        const auto& animPool       = world.GetPool<SpriteAnimator2D>();
        const auto spriteEntities  = spritePool.Entities();
        const auto sprites         = spritePool.Components();

        for (usize i = 0U; i < spriteEntities.size(); ++i) {
            const Sprite2D& sp = sprites[i];
            if (!sp.Visible()) continue;

            const u16 idx              = spriteEntities[i].index;
            const WorldTransform2D& wt = world.GetWorldByIndex(idx);
            f32 rx     = wt.x;
            f32 ry     = wt.y;
            f32 sx     = wt.scaleX;
            f32 sy     = wt.scaleY;
            f32 c      = wt.cosRot;
            f32 s      = wt.sinRot;
            f32 rDepth = wt.worldDepth;

            if ((movedWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL) {
                const PrevWorldSnapshot2D& pv = world.GetPrevWorldByIndex(idx);
                const f32 alpha = renderStep.Alpha(
                    static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx)));
                rx     = Lerp(pv.x, wt.x, alpha);
                ry     = Lerp(pv.y, wt.y, alpha);
                sx     = Lerp(pv.scaleX, wt.scaleX, alpha);
                sy     = Lerp(pv.scaleY, wt.scaleY, alpha);
                rDepth = Lerp(pv.worldDepth, wt.worldDepth, alpha);
                if (wt.worldRotRad != pv.worldRotRad) {
                    const f32 dRot = WrapAngle(wt.worldRotRad - pv.worldRotRad);
                    FastSinCos(pv.worldRotRad + dRot * alpha, c, s);
                }
            }

            const SpriteFrame* framePtr = &assetBank.GetSprite(sp.sprite);
            if ((world.GetComponentMaskByIndex(idx) & (1ULL << SpriteAnimator2D::kTypeId)) != 0ULL) {
                const SpriteAnimator2D& anim = animPool.DirectAtEntityIndex(idx);
                if (anim.atlas.IsValid()) {
                    framePtr = &assetBank.GetAtlasFrame(
                        anim.atlas, static_cast<u16>(anim.firstFrame + anim.currentFrame));
                }
            }

            const i32 depthOffset = static_cast<i32>(rDepth * 100.0f);
            const u16 resolvedDepth = sp.YSort()
                ? SortKey::YDepth(ry + rDepth)
                : static_cast<u16>(std::clamp(static_cast<i32>(sp.baseDepth) + depthOffset, 0, 65535));

            outRenderList.Push({
                .center  = {rx, ry},
                .size    = {sp.width * sx, sp.height * sy},
                .pivot   = framePtr->pivot,
                .cosRot  = c,
                .sinRot  = s,
                .uv      = framePtr->uv,
                .color   = sp.tint,
                .texture = framePtr->gpuTexture,
                .depth   = resolvedDepth,
                .layer   = sp.layer,
                .view    = sp.ViewSlot(),
                .blend   = sp.Blend(),
                .flipX   = sp.FlipX(),
                .flipY   = sp.FlipY(),
            });
        }
    }

    if ((regMask & (1ULL << PointLight2D::kTypeId)) != 0ULL) {
        const auto& lightPool     = world.GetPool<PointLight2D>();
        const auto lightEntities  = lightPool.Entities();
        const auto lights         = lightPool.Components();

        for (usize i = 0U; i < lightEntities.size(); ++i) {
            const PointLight2D& light = lights[i];
            if (light.intensity <= 0.0f || light.radius <= 0.0f) continue;
            const u16 idx              = lightEntities[i].index;
            const WorldTransform2D& wt = world.GetWorldByIndex(idx);
            f32 lx = wt.x;
            f32 ly = wt.y;
            if ((movedWords[idx >> 6U] & (1ULL << (idx & 63U))) != 0ULL) {
                const PrevWorldSnapshot2D& pv = world.GetPrevWorldByIndex(idx);
                const f32 alpha = renderStep.Alpha(
                    static_cast<TimeChannel>(world.GetTimeChannelByIndex(idx)));
                lx = Lerp(pv.x, wt.x, alpha);
                ly = Lerp(pv.y, wt.y, alpha);
            }

            f32 pulse = 1.0f;
            if (light.pulseSpeed != 0.0f) {
                f32 c = 1.0f, s = 0.0f;
                FastSinCos(light.pulsePhase, c, s);
                pulse = 0.85f + 0.15f * s;
            }
            const f32 diameter  = light.radius * pulse * 2.0f;
            const f32 intensity = light.intensity * pulse;

            outRenderList.Push({
                .center  = {lx, ly},
                .size    = {diameter, diameter},
                .color   = {
                    light.color.r * intensity,
                    light.color.g * intensity,
                    light.color.b * intensity,
                    light.color.a * intensity,
                },
                .texture = kWhiteTexture,
                .layer   = 200U,
                .blend   = BlendMode::Additive,
            });
        }
    }
}

} // namespace en

#include "gameplay/GameplaySystem.h"

#include "core/Serialization.h"

namespace en {

void GameplaySystem::StepCameras(World& world, const TickStep& step) noexcept {
    if (!m_cues || !step) return;

    auto& camPool = world.GetPool<CameraFollow2D>();
    if (camPool.Size() == 0U) return;

    bool anyUpdated = false;
    for (usize i = 0U; i < camPool.Size(); ++i) {
        const EntityId camId = camPool.Entities()[i];
        CameraFollow2D& cam  = camPool.ComponentAtDense(i);
        if (!world.IsAlive(cam.target)) continue;

        const auto ch = static_cast<TimeChannel>(world.GetTimeChannelByIndex(camId.index));
        if (step.IsFrozen(ch)) continue;
        const f32 dt = step.Dt(ch);
        if (dt <= 0.0f) continue;

        const WorldTransform2D& targetWt = world.GetWorldByIndex(cam.target.index);
        Vec2 focus{targetWt.x, targetWt.y};

        if (HasFlag(cam.flags, CameraFlags::LatchGroundY)) {
            if (const auto* body = world.TryGet<PhysicsBody2D>(cam.target)) {
                if (body->IsGrounded()) cam.groundLatchY = targetWt.y;
                if (std::abs(targetWt.y - cam.groundLatchY) <= 2.5f) {
                    focus.y = cam.groundLatchY;
                }
            }
        }

        if (HasFlag(cam.flags, CameraFlags::EnableLookahead)) {
            if (const auto* cc = world.TryGet<CharacterController2D>(cam.target)) {
                const f32 targetLook = static_cast<f32>(cc->facing) * cam.lookaheadDist;
                cam.lookaheadX       = Lerp(cam.lookaheadX, targetLook, std::min(1.0f, dt * 6.0f));
                focus.x             += cam.lookaheadX;
            }
        }

        if (cam.secondaryTargetIndex != 0U && cam.secondaryTargetIndex <= world.MaxAllocatedIndex()) {
            const EntityId secId = world.EntityAtSlot(cam.secondaryTargetIndex);
            if (world.IsAlive(secId)) {
                const WorldTransform2D& secWt = world.GetWorldByIndex(secId.index);
                focus.x = Lerp(focus.x, secWt.x, 0.35f);
                focus.y = Lerp(focus.y, secWt.y, 0.35f);
            } else {
                cam.secondaryTargetIndex = 0U;
            }
        }

        if (HasFlag(cam.flags, CameraFlags::SnapNextStep)) {
            cam.anchor   = focus;
            cam.velocity = {};
            cam.flags   &= ~CameraFlags::SnapNextStep;
        } else {
            const f32 targetX = std::clamp(cam.anchor.x, focus.x - cam.deadzone.x, focus.x + cam.deadzone.x);
            const f32 targetY = std::clamp(cam.anchor.y, focus.y - cam.deadzone.y, focus.y + cam.deadzone.y);
            const f32 omega   = static_cast<f32>(cam.followOmega);
            StepSpring(cam.anchor.x, cam.velocity.x, targetX, omega, dt);
            StepSpring(cam.anchor.y, cam.velocity.y, targetY, omega * 1.2f, dt);
        }

        CameraRig2D* rig = world.TryGet<CameraRig2D>(camId);
        if (HasFlag(cam.flags, CameraFlags::ConfineToRoom) && !cam.roomBounds.IsEmpty()) {
            f32 halfW = 0.0f;
            f32 halfH = 0.0f;
            if (rig) {
                const f32 invZoom = (rig->zoom > 1e-4f) ? (1.0f / rig->zoom) : 1.0f;
                const f32 invPpm  = (rig->pixelsPerMeter > 1e-4f) ? (1.0f / rig->pixelsPerMeter) : (1.0f / 48.0f);
                halfW = 640.0f * rig->viewport.w * invPpm * invZoom;
                halfH = 360.0f * rig->viewport.h * invPpm * invZoom;
            }
            const f32 minX = cam.roomBounds.x + halfW;
            const f32 maxX = cam.roomBounds.Right() - halfW;
            const f32 minY = cam.roomBounds.y + halfH;
            const f32 maxY = cam.roomBounds.Bottom() - halfH;
            cam.anchor.x = (minX <= maxX) ? std::clamp(cam.anchor.x, minX, maxX) : cam.roomBounds.Center().x;
            cam.anchor.y = (minY <= maxY) ? std::clamp(cam.anchor.y, minY, maxY) : cam.roomBounds.Center().y;
        }

        if (cam.trauma > 0.0f) {
            cam.trauma = std::max(0.0f, cam.trauma - 1.5f * dt);
        }
        if (rig) rig->trauma = cam.trauma;

        LocalTransform2D& loc = world.ModifyLocalTransformByIndex(camId.index);
        loc.x = cam.anchor.x;
        loc.y = cam.anchor.y;
        anyUpdated = true;
    }

    if (anyUpdated) world.PropagateTransforms();
}

u32 GameplaySystem::ApplyRoomMutations(World& world) noexcept {
    if ((world.RegisteredPoolsMask() & (1ULL << WorldMutation2D::kTypeId)) == 0ULL) return 0U;

    const auto& mutPool = world.GetPool<WorldMutation2D>();
    if (mutPool.Size() == 0U) return 0U;

    std::array<EntityId, 256> toDestroy{};
    u32 destroyCount = 0U;
    u32 appliedCount = 0U;

    for (usize i = 0U; i < mutPool.Size(); ++i) {
        const EntityId         id = mutPool.Entities()[i];
        const WorldMutation2D& wm = mutPool.ComponentAtDense(i);
        const bool mutated = (wm.guid != 0ULL && HasMutation(wm.guid)) ||
                             (wm.flagId < kMaxFlags && TestWorldFlag(wm.flagId));
        if (!mutated) continue;

        ++appliedCount;
        if (HasFlag(wm.flags, MutationFlags::DestroyIfMutated)) {
            if (destroyCount < toDestroy.size()) toDestroy[destroyCount++] = id;
            continue;
        }
        if (HasFlag(wm.flags, MutationFlags::DisableCollider)) {
            (void)world.Remove<Collider2D>(id);
        }
        if (HasFlag(wm.flags, MutationFlags::HideSprite)) {
            if (auto* sp = world.TryGet<Sprite2D>(id)) sp->SetVisible(false);
        }
    }

    for (u32 i = 0U; i < destroyCount; ++i) {
        (void)world.Destroy(toDestroy[i]);
    }
    return appliedCount;
}

Result<usize, GameplayErrc> GameplaySystem::SerializePersistence(std::span<u8> outBuf) const noexcept {
    const usize needed = RequiredSaveBytes();
    if (outBuf.size() < needed) return Err(GameplayErrc::OutOfMemory);

    BinaryWriter writer(outBuf);
    SaveHeader hdr{
        .magic         = kSaveBlobMagic,
        .version       = 1U,
        .mutationCount = static_cast<u16>(m_mutations.Size()),
        .tickCount     = m_tickCount,
        .payloadCrc32  = 0U,
    };
    if (!writer.Write(hdr)) return Err(GameplayErrc::OutOfMemory);
    if (!writer.WriteBytes({reinterpret_cast<const u8*>(m_worldFlags.data()), sizeof(m_worldFlags)})) {
        return Err(GameplayErrc::OutOfMemory);
    }

    for (const auto& slot : m_mutations.Slots()) {
        if (slot.dist == 0U || slot.key == 0ULL) continue;
        if (!writer.Write(slot.key) || !writer.Write(slot.value)) {
            return Err(GameplayErrc::OutOfMemory);
        }
    }

    const usize total = writer.Offset();
    hdr.payloadCrc32  = Crc32({outBuf.data() + sizeof(SaveHeader), total - sizeof(SaveHeader)});
    std::memcpy(outBuf.data(), &hdr, sizeof(SaveHeader));
    return total;
}

Result<void, GameplayErrc> GameplaySystem::DeserializePersistence(std::span<const u8> inBuf) noexcept {
    if (inBuf.size() < sizeof(SaveHeader) + sizeof(m_worldFlags)) {
        return Err(GameplayErrc::InvalidConfig);
    }

    BinaryReader reader(inBuf);
    SaveHeader hdr{};
    if (!reader.Read(hdr) || hdr.magic != kSaveBlobMagic || hdr.version != 1U) {
        return Err(GameplayErrc::InvalidConfig);
    }

    const usize expectedSize = sizeof(SaveHeader) + sizeof(m_worldFlags) +
                               static_cast<usize>(hdr.mutationCount) * (sizeof(u64) + sizeof(u32));
    if (inBuf.size() < expectedSize) return Err(GameplayErrc::InvalidConfig);

    const u32 actualCrc = Crc32({inBuf.data() + sizeof(SaveHeader), expectedSize - sizeof(SaveHeader)});
    if (actualCrc != hdr.payloadCrc32) return Err(GameplayErrc::InvalidConfig);

    std::array<u64, kFlagWords> loadedFlags{};
    if (!reader.ReadBytes({reinterpret_cast<u8*>(loadedFlags.data()), sizeof(loadedFlags)})) {
        return Err(GameplayErrc::InvalidConfig);
    }

    m_mutations.Clear();
    for (u16 i = 0U; i < hdr.mutationCount; ++i) {
        u64 key = 0ULL;
        u32 val = 0U;
        if (!reader.Read(key) || !reader.Read(val) || key == 0ULL || !m_mutations.Insert(key, val)) {
            return Err(GameplayErrc::InvalidConfig);
        }
    }

    m_worldFlags = loadedFlags;
    m_tickCount  = hdr.tickCount;
    return {};
}

} // namespace en

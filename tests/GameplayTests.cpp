#include "gameplay/GameplayPipeline.h"

#include "assets/AssetBank.h"
#include "core/time/TimePipeline.h"
#include "input/ActionBuffer.h"
#include "world/World.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace en::gameplay::tests {

namespace {

void VerifyStaticLayouts() noexcept {
    static_assert(sizeof(CompactAabb2D) == 8);
    static_assert(sizeof(KinematicMotor2D) == 32 && alignof(KinematicMotor2D) == 32);
    static_assert(sizeof(KinematicCarrier2D) == 16 && alignof(KinematicCarrier2D) == 16);
    static_assert(sizeof(MotionStack2D) == 32 && alignof(MotionStack2D) == 32);
    static_assert(sizeof(StateMachine2D) == 16 && alignof(StateMachine2D) == 16);
    static_assert(sizeof(HitboxRig2D) == 32 && alignof(HitboxRig2D) == 32);
    static_assert(sizeof(DamageReceiver2D) == 16 && alignof(DamageReceiver2D) == 16);
    static_assert(sizeof(SafeGroundBreadcrumb2D) == 16 && alignof(SafeGroundBreadcrumb2D) == 16);
    static_assert(sizeof(CameraController2D) == 64 && alignof(CameraController2D) == 64);
    static_assert(sizeof(SimCue) == 32 && alignof(SimCue) == 32);
    static_assert(sizeof(SpatialIndex2D::GridBox) == 24 && alignof(SpatialIndex2D::GridBox) == 8);
    static_assert(sizeof(SpatialIndex2D) == 64 && alignof(SpatialIndex2D) == 64);
    static_assert(sizeof(GameplayPipeline) == 704 && alignof(GameplayPipeline) == 64);
}

std::vector<u8> MakeFlatFloorCollisionBlob() {
    const u32 chunkBytes = sizeof(CookedCollisionChunk8x8);
    const u32 fineOffset = sizeof(CookedCollisionMapHeader) + chunkBytes;
    const u32 fineBytes  = sizeof(CookedTileCell) * 64u;
    std::vector<u8> buf(fineOffset + fineBytes, 0);

    auto* hdr = reinterpret_cast<CookedCollisionMapHeader*>(buf.data());
    hdr->magic               = CookedCollisionMapHeader::kMagic;
    hdr->version             = CookedCollisionMapHeader::kVersion;
    hdr->chunkCountX         = 1;
    hdr->chunkCountY         = 1;
    hdr->tileSize_q8         = 256;
    hdr->fineTileTableOffset = fineOffset;
    hdr->originX             = 0.0f;
    hdr->originY             = 0.0f;

    auto* chunk = reinterpret_cast<CookedCollisionChunk8x8*>(buf.data() + sizeof(CookedCollisionMapHeader));
    chunk->solidBitmask         = 0xFFULL;
    chunk->pureFullBlockBitmask = 0xFFULL;
    chunk->fineTileBaseIndex    = 0;
    return buf;
}

std::vector<u8> MakeSlashAnimRigBlob() {
    const u32 dirOffset    = sizeof(CookedAnimRigHeader);
    const u32 hurtOffset   = dirOffset + sizeof(CookedFrameDirective) * 2u;
    const u32 sliceOffset  = hurtOffset + sizeof(CompactAabb2D);
    const u32 socketOffset = sliceOffset + sizeof(CookedHitboxSlice);
    const u32 totalBytes   = socketOffset + sizeof(CookedHitboxSocket);

    std::vector<u8> buf(totalBytes, 0);
    auto* hdr = reinterpret_cast<CookedAnimRigHeader*>(buf.data());
    hdr->magic                = CookedAnimRigHeader::kMagic;
    hdr->version              = CookedAnimRigHeader::kVersion;
    hdr->directiveCount       = 2;
    hdr->hurtboxProfileCount  = 1;
    hdr->hitboxSliceCount     = 1;
    hdr->hitboxSocketCount    = 1;
    hdr->directiveOffset      = dirOffset;
    hdr->hurtboxProfileOffset = hurtOffset;
    hdr->hitboxSliceOffset    = sliceOffset;
    hdr->hitboxSocketOffset   = socketOffset;

    auto* dirs = reinterpret_cast<CookedFrameDirective*>(buf.data() + dirOffset);
    dirs[0] = CookedFrameDirective{
        .clipId     = 1,
        .frameIndex = 0,
        .kind       = FrameDirectiveKind::ActivateHitboxSlice,
        .paramU8    = 0,
        .paramU16   = 1,
        .paramI16   = 0,
    };
    dirs[1] = CookedFrameDirective{
        .clipId     = 1,
        .frameIndex = 0,
        .kind       = FrameDirectiveKind::SetOnHitCancelMask,
        .paramU8    = 0,
        .paramU16   = static_cast<u16>(CancelCategory::Jump),
        .paramI16   = 0,
    };

    auto* hurt = reinterpret_cast<CompactAabb2D*>(buf.data() + hurtOffset);
    *hurt = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.5f, 1.0f);

    auto* slice = reinterpret_cast<CookedHitboxSlice*>(buf.data() + sliceOffset);
    slice->boundingUnion    = CompactAabb2D::FromMeters(1.0f, 0.0f, 1.2f, 0.8f);
    slice->firstSocketIndex = 0;
    slice->socketCount      = 1;

    auto* sock = reinterpret_cast<CookedHitboxSocket*>(buf.data() + socketOffset);
    sock->localBox            = CompactAabb2D::FromMeters(1.0f, 0.0f, 1.2f, 0.8f);
    sock->damageMultiplier_q8 = 256;
    sock->knockbackAngleDeg   = 30;
    sock->knockbackSpeed_q2   = 48;
    sock->hitstopTicks        = 6;
    sock->priority            = 1;
    sock->socketFlags         = 0x01;
    return buf;
}

void TestCarrierGroundedPreservationAndCombatAndCamera() {
    world::World w{};
    auto wInit = w.Init();
    assert(wInit.has_value());

    assets::AssetBank bank{};
    auto bInit = bank.Init();
    assert(bInit.has_value());

    const auto colBytes = MakeFlatFloorCollisionBlob();
    const auto rigBytes = MakeSlashAnimRigBlob();
    auto registerBlob = [&](std::string_view name, std::span<const u8> bytes) -> assets::BlobHandle {
        bool needsLoad = false;
        auto slotRes = bank.AcquireSlot(assets::AssetId::Hash(name), assets::AssetKind::Blob, 0, assets::Residency::Sim, needsLoad);
        assert(slotRes.has_value());
        auto commitRes = bank.CommitBlob(*slotRes, bytes, 0, false);
        assert(commitRes.has_value());
        return bank.ToBlobHandle(*slotRes);
    };

    const auto colBlob  = registerBlob("col/floor.bin", colBytes);
    const auto rigBlob  = registerBlob("rig/slash.bin", rigBytes);

    static bool observedGroundedOnAscendingCarrier = false;
    static u32 resolvedHitsTotal = 0;

    const GameplayPolicyTable policies{
        .evaluateActor = [](const ActorPolicyContext&,
                            KinematicMotor2D& motor,
                            MotionStack2D&,
                            StateMachine2D&,
                            HitboxRig2D*) noexcept {
            if (HasContact(motor.contacts, ContactFlags::Ground) && motor.carrierId != world::kNullEntity) {
                observedGroundedOnAscendingCarrier = true;
            }
        },
        .resolveCombatHits = [](world::World&,
                                time::SimStepView,
                                std::span<const ResolvedHitEvent> hits,
                                GameplayPipeline&) noexcept {
            resolvedHitsTotal += static_cast<u32>(hits.size());
        }
    };

    GameplayPipeline gp{};
    auto gpInit = gp.Init(w, policies);
    assert(gpInit.has_value());
    assert(gp.SetActiveRoomCollision(bank, colBlob).has_value());

    const auto carrierId = w.Spawn({.localTransform = {.x = 2.0f, .y = 2.0f}}).value();
    (void)w.Emplace<KinematicCarrier2D>(carrierId, KinematicCarrier2D{
        .deltaX = 0.0f,
        .deltaY = 0.05f,
        .bounds = CompactAabb2D::FromMeters(0.0f, 0.0f, 1.5f, 0.25f)
    });

    const auto playerId = w.Spawn({.localTransform = {.x = 2.0f, .y = 3.251f}}).value();
    auto* playerMotor = w.Emplace<KinematicMotor2D>(playerId, KinematicMotor2D{
        .bounds    = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 1.0f),
        .carrierId = carrierId,
        .contacts  = ContactFlags::Ground
    }).value();
    (void)w.Emplace<MotionStack2D>(playerId, MotionStack2D{});
    auto* playerFsm = w.Emplace<StateMachine2D>(playerId, StateMachine2D{
        .rigBlob    = rigBlob,
        .cancelMask = 0,
        .animClipId = 1
    }).value();
    (void)w.Emplace<world::SpriteAnimator2D>(playerId, world::SpriteAnimator2D{
        .frameCount    = 4,
        .currentFrame  = 0,
        .ticksPerFrame = 4,
        .loop          = 0,
        .playing       = 1
    });
    (void)w.Emplace<HitboxRig2D>(playerId, HitboxRig2D{
        .hurtbox       = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 1.0f),
        .hitLayerMask  = 0x02u,
        .hurtLayerMask = 0x01u
    });

    const auto enemyId = w.Spawn({.localTransform = {.x = 3.8f, .y = 3.3f}}).value();
    (void)w.Emplace<HitboxRig2D>(enemyId, HitboxRig2D{
        .hurtbox       = CompactAabb2D::FromMeters(0.0f, 0.0f, 0.6f, 1.0f),
        .hitLayerMask  = 0x01u,
        .hurtLayerMask = 0x02u
    });
    (void)w.Emplace<DamageReceiver2D>(enemyId, DamageReceiver2D{.hp = 100, .maxHp = 100});

    const auto camId = w.Spawn({.localTransform = {.x = 2.0f, .y = 3.0f}}).value();
    auto* camCtrl = w.Emplace<CameraController2D>(camId, CameraController2D{
        .primaryTarget  = playerId,
        .latchedGroundY = 3.0f,
        .deadzoneHalfW_q4 = 0,
        .deadzoneHalfH_q4 = 0,
        .cameraFlags    = 0
    }).value();

    w.PropagateTransforms();

    time::TimePipeline<time::ManualClock> timePipe{};
    input::TickInput tickIn{};
    input::ActionBuffer actBuf{};

    timePipe.BeginFrameManual(time::kFixedDtNs);
    const auto step1 = timePipe.PollTick();
    assert(static_cast<bool>(step1));

    w.BeginTick(step1);
    gp.Tick(w, bank, step1, tickIn, actBuf);
    w.EndTick(step1);

    assert(observedGroundedOnAscendingCarrier);
    assert(HasContact(playerMotor->contacts, ContactFlags::Ground));
    assert(resolvedHitsTotal == 1u);
    assert(playerFsm->CanCancelInto(CancelCategory::Jump));

    const auto cues = gp.DrainCues();
    assert(cues.size() == 2u);
    assert(gp.DrainCues().empty());

    // Stop carrier and inject mid-tick trauma repeatedly, then let it decay and verify 0 drift
    w.GetPool<KinematicCarrier2D>().Components()[0].deltaY = 0.0f;
    camCtrl->AddTrauma(0.8f);
    for (u32 t = 0; t < 240; ++t) {
        if (t == 5 || t == 15) {
            camCtrl->AddTrauma(0.5f);
            camCtrl->AddDirectionalKick(2.0f, -1.5f);
        }
        timePipe.BeginFrameManual(time::kFixedDtNs);
        const auto s = timePipe.PollTick();
        w.BeginTick(s);
        gp.Tick(w, bank, s, tickIn, actBuf);
        w.EndTick(s);
        (void)gp.DrainCues();
    }

    assert(camCtrl->trauma_q16 == 0);
    const auto& playerFinalWt = w.GetWorldByIndex(playerId.index);
    const auto& finalCamWt    = w.GetWorldByIndex(camId.index);
    assert(std::abs(finalCamWt.x - playerFinalWt.x) < 1e-3f);
    assert(std::abs(finalCamWt.y - playerFinalWt.y) < 1e-3f);

    assert(!gp.TestWorldFlag(1337));
    gp.SetWorldFlag(1337, true);
    assert(gp.TestWorldFlag(1337));
    const u64 hashWithFlag = gp.ComputeAuditHash(w);
    gp.ClearWorldFlags();
    assert(!gp.TestWorldFlag(1337));
    const u64 hashCleared = gp.ComputeAuditHash(w);
    assert(hashWithFlag != hashCleared);
}

} // namespace

} // namespace en::gameplay::tests

int main() {
    en::gameplay::tests::VerifyStaticLayouts();
    en::gameplay::tests::TestCarrierGroundedPreservationAndCombatAndCamera();
    std::puts("[GameplayTests] All Phase 7 gameplay unit tests and static layout checks passed!");
    return 0;
}

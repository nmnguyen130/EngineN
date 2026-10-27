#include "framework/SceneDirector.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "physics/PhysicsSystem.h"
#include "renderer/RenderList.h"
#include "world/World.h"
#include "world/WorldExtractor.h"

#include <algorithm>
#include <bit>

namespace en {

Result<void, FrameworkErrc> SceneDirector::Init(AssetBank& assetBank) noexcept {
    *this = SceneDirector{};

    auto pScope = assetBank.BeginScope();
    if (!pScope) {
        return Err(FrameworkErrc::ScopeAllocationFailed);
    }
    m_persistentScope = *pScope;

    auto sScope = assetBank.BeginScope();
    if (!sScope) {
        assetBank.EndScope(m_persistentScope);
        m_persistentScope = 0xFFU;
        return Err(FrameworkErrc::ScopeAllocationFailed);
    }
    m_activeScope = *sScope;
    return {};
}

void SceneDirector::Shutdown(
    AssetBank& assetBank,
    AudioSystem& audio,
    SceneLoadContext* optionalLoadCtx) noexcept {
    if (optionalLoadCtx != nullptr && optionalLoadCtx->world != nullptr) {
        while (m_stackDepth > 0U) {
            const u8 top = --m_stackDepth;
            if (m_exitFns[top] != nullptr) {
                m_exitFns[top](*optionalLoadCtx, m_userStates[top]);
            }
        }
        if (m_activeSceneDeactivateFn != nullptr) {
            m_activeSceneDeactivateFn(*optionalLoadCtx);
        }
    }

    for (u8 scope : {m_stagingScope, m_activeScope, m_persistentScope}) {
        if (scope < kMaxAssetScopes) {
            audio.RequestScopeRelease(scope);
            assetBank.EndScope(scope);
        }
    }

    *this = SceneDirector{};
}

Result<void, FrameworkErrc> SceneDirector::RequestSceneTransition(
    const SceneTransitionDesc& desc,
    AssetBank& assetBank) noexcept {
    if (m_phase != TransitionPhase::Idle) {
        return Err(FrameworkErrc::SceneTransitionBusy);
    }
    if (!desc.sceneId.IsValid()) {
        return Err(FrameworkErrc::InvalidConfig);
    }

    auto newScope = assetBank.BeginScope();
    if (!newScope) {
        return Err(FrameworkErrc::ScopeAllocationFailed);
    }

    m_stagingScope = *newScope;
    m_pendingScene = desc;
    m_activeWipe = desc.wipe;
    m_wipeElapsedTicks = 0U;
    m_wipeDurationTicks = desc.transitionDurationTicks;

    bool needsLoad = false;
    for (AssetId id : {desc.manifestBlobId, desc.collisionMapId, desc.rootPrefabId, desc.cueBankId}) {
        if (id.IsValid() && !assetBank.FindBlobHandle(id).IsValid()) {
            (void)assetBank.AcquireSlot(id, AssetKind::Blob, m_stagingScope, AssetResidency::Sim, needsLoad);
        }
    }

    m_phase = (desc.wipe != TransitionWipeKind::None && desc.transitionDurationTicks > 0U)
                  ? TransitionPhase::FadingOut
                  : TransitionPhase::StreamingSceneAssets;
    return {};
}

Result<void, FrameworkErrc> SceneDirector::ReloadActiveScene(AssetBank& assetBank) noexcept {
    if (!m_activeSceneId.IsValid() || !m_pendingScene.sceneId.IsValid()) {
        return Err(FrameworkErrc::InvalidConfig);
    }
    SceneTransitionDesc reloadDesc = m_pendingScene;
    reloadDesc.mode = SceneTransitionMode::FullReset;
    reloadDesc.transitionDurationTicks = 0U;
    reloadDesc.wipe = TransitionWipeKind::None;
    return RequestSceneTransition(reloadDesc, assetBank);
}

void SceneDirector::StepTransitionWipeTick() noexcept {
    if (m_phase == TransitionPhase::FadingOut) {
        if (++m_wipeElapsedTicks >= m_wipeDurationTicks) {
            m_wipeElapsedTicks = m_wipeDurationTicks;
            m_phase = TransitionPhase::StreamingSceneAssets;
        }
    } else if (m_phase == TransitionPhase::FadingIn) {
        if (m_wipeElapsedTicks == 0U || --m_wipeElapsedTicks == 0U) {
            m_phase = TransitionPhase::Idle;
            m_activeWipe = TransitionWipeKind::None;
        }
    }
}

Result<bool, FrameworkErrc> SceneDirector::ActivatePendingScene(SceneLoadContext& loadCtx) noexcept {
    if (!loadCtx.assetBank || !loadCtx.world || !loadCtx.audio) {
        return Err(FrameworkErrc::InvalidConfig);
    }
    AssetBank& assetBank = *loadCtx.assetBank;

    const auto rollbackStaging = [&](FrameworkErrc err) noexcept -> Result<bool, FrameworkErrc> {
        if (m_stagingScope < kMaxAssetScopes && m_stagingScope != m_persistentScope && m_stagingScope != m_activeScope) {
            assetBank.EndScope(m_stagingScope);
        }
        m_stagingScope = 0xFFU;
        m_phase = TransitionPhase::Idle;
        m_activeWipe = TransitionWipeKind::None;
        m_wipeElapsedTicks = 0U;
        return Err(err);
    };

    u32 manifestMusicTrackId = 0U;

    // AssetBank blob slices are 64-byte aligned; read headers and entries directly without memcpy.
    if (m_pendingScene.manifestBlobId.IsValid()) {
        const BlobHandle manifestBlob = assetBank.FindBlobHandle(m_pendingScene.manifestBlobId);
        if (!manifestBlob.IsValid()) {
            return rollbackStaging(FrameworkErrc::CorruptedSceneManifest);
        }
        const BlobRecord& rec = assetBank.GetBlob(manifestBlob);
        if (rec.data == nullptr || rec.size < sizeof(CookedSceneManifestHeader)) {
            return rollbackStaging(FrameworkErrc::CorruptedSceneManifest);
        }
        const auto& hdr = *reinterpret_cast<const CookedSceneManifestHeader*>(rec.data);
        const usize expectedBytes = sizeof(CookedSceneManifestHeader) +
                                    static_cast<usize>(hdr.assetEntryCount) * sizeof(CookedSceneAssetEntry);
        if (hdr.magic != CookedSceneManifestHeader::kMagic ||
            hdr.version != CookedSceneManifestHeader::kVersion ||
            rec.size < expectedBytes) {
            return rollbackStaging(FrameworkErrc::CorruptedSceneManifest);
        }
        const auto* entriesPtr = reinterpret_cast<const CookedSceneAssetEntry*>(
            rec.data + sizeof(CookedSceneManifestHeader));
        const std::span<const CookedSceneAssetEntry> entries{entriesPtr, hdr.assetEntryCount};
        if (hdr.checksum != 0U && ComputeSceneManifestChecksum(entries) != hdr.checksum) {
            return rollbackStaging(FrameworkErrc::CorruptedSceneManifest);
        }
        manifestMusicTrackId = hdr.musicTrackId;
        bool needsLoad = false;
        for (const CookedSceneAssetEntry& e : entries) {
            if (e.id.IsValid()) {
                (void)assetBank.AcquireSlot(e.id, e.kind, m_stagingScope, e.residency, needsLoad);
            }
        }
    }

    if (!assetBank.IsScopeSimReady(m_stagingScope)) {
        return false;
    }

    BlobHandle validColBlob{};
    if (m_pendingScene.collisionMapId.IsValid() && loadCtx.physics != nullptr) {
        validColBlob = assetBank.FindBlobHandle(m_pendingScene.collisionMapId);
        if (!validColBlob.IsValid()) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
        const BlobRecord& colRec = assetBank.GetBlob(validColBlob);
        if (!colRec.IsValid() || colRec.size < sizeof(CookedCollisionMapHeader)) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
        const auto& cmHdr = *reinterpret_cast<const CookedCollisionMapHeader*>(colRec.data);
        const usize tilesBytes = static_cast<usize>(cmHdr.widthTiles) * cmHdr.heightTiles;
        if (cmHdr.magic != CookedCollisionMapHeader::kMagic ||
            cmHdr.version != CookedCollisionMapHeader::kVersion ||
            cmHdr.widthTiles == 0U || cmHdr.heightTiles == 0U ||
            colRec.size < sizeof(CookedCollisionMapHeader) + tilesBytes) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    BlobHandle validPrefabBlob{};
    if (m_pendingScene.rootPrefabId.IsValid()) {
        validPrefabBlob = assetBank.FindBlobHandle(m_pendingScene.rootPrefabId);
        if (!validPrefabBlob.IsValid()) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
        const BlobRecord& prefRec = assetBank.GetBlob(validPrefabBlob);
        if (!prefRec.IsValid() || prefRec.size < sizeof(CookedPrefabHeader)) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
        const auto& pfHdr = *reinterpret_cast<const CookedPrefabHeader*>(prefRec.data);
        if (pfHdr.magic != kCookedPrefabMagic || pfHdr.version != 1U || pfHdr.entityCount == 0U) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    BlobHandle validCueBlob{};
    if (m_pendingScene.cueBankId.IsValid()) {
        validCueBlob = assetBank.FindBlobHandle(m_pendingScene.cueBankId);
        if (!validCueBlob.IsValid() || !assetBank.GetBlob(validCueBlob).IsValid()) {
            return rollbackStaging(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    // Carve temporary rollback snapshot from AssetBank payload pool before mutating World.
    u8* rollbackSlicePtr = nullptr;
    usize rollbackAllocCap = 0U;
    usize rollbackWrittenBytes = 0U;
    if (loadCtx.world->AliveCount() > 0U) {
        rollbackAllocCap = loadCtx.world->RequiredSnapshotBytes();
        const std::span<u8> sliceSpan = assetBank.AllocatePayloadSlice(rollbackAllocCap);
        if (!sliceSpan.empty()) {
            rollbackSlicePtr = sliceSpan.data();
            if (auto sr = loadCtx.world->SerializeWorldSnapshot(sliceSpan); sr) {
                rollbackWrittenBytes = *sr;
            } else {
                assetBank.FreePayloadSlice(rollbackSlicePtr, rollbackAllocCap);
                rollbackSlicePtr = nullptr;
                rollbackAllocCap = 0U;
            }
        }
    }

    const auto freeRollbackSlice = [&]() noexcept {
        if (rollbackSlicePtr != nullptr) {
            assetBank.FreePayloadSlice(rollbackSlicePtr, rollbackAllocCap);
            rollbackSlicePtr = nullptr;
            rollbackAllocCap = 0U;
            rollbackWrittenBytes = 0U;
        }
    };

    const u8 oldScope = m_activeScope;
    const AssetId oldSceneId = m_activeSceneId;
    auto* const oldDeactivateFn = m_activeSceneDeactivateFn;

    const auto rollbackPostSwap = [&](FrameworkErrc err) noexcept -> Result<bool, FrameworkErrc> {
        loadCtx.world->Clear();
        if (rollbackSlicePtr != nullptr && rollbackWrittenBytes > 0U) {
            (void)loadCtx.world->DeserializeWorldSnapshot(std::span<const u8>{rollbackSlicePtr, rollbackWrittenBytes});
        }
        freeRollbackSlice();
        if (m_activeScope < kMaxAssetScopes && m_activeScope != oldScope && m_activeScope != m_persistentScope) {
            assetBank.EndScope(m_activeScope);
        }
        m_activeScope = oldScope;
        m_stagingScope = 0xFFU;
        m_activeSceneId = oldSceneId;
        m_activeSceneDeactivateFn = oldDeactivateFn;
        loadCtx.sceneScope = oldScope;
        m_phase = TransitionPhase::Idle;
        m_activeWipe = TransitionWipeKind::None;
        m_wipeElapsedTicks = 0U;
        return Err(err);
    };

    loadCtx.sceneScope = oldScope;
    loadCtx.transitionTicks = m_wipeDurationTicks;
    if (m_activeSceneDeactivateFn != nullptr) {
        m_activeSceneDeactivateFn(loadCtx);
        m_activeSceneDeactivateFn = nullptr;
    }

    if (m_pendingScene.mode == SceneTransitionMode::FullReset) {
        loadCtx.world->Clear();
    } else {
        // Clamp index on each iteration because cascading LCRS Destroy() may remove multiple pool entries.
        const u64 regMask = loadCtx.world->RegisteredPoolsMask();
        if ((regMask & MakeComponentMask<Lifetime2D>()) != 0ULL) {
            const auto& lifePool = loadCtx.world->GetPool<Lifetime2D>();
            while (lifePool.Size() > 0U) {
                (void)loadCtx.world->Destroy(lifePool.Entities()[lifePool.Size() - 1U], true);
            }
        }
        if ((regMask & MakeComponentMask<Tag2D>()) != 0ULL) {
            const auto& tagPool = loadCtx.world->GetPool<Tag2D>();
            for (u16 i = tagPool.Size(); i > 0U;) {
                i = std::min<u16>(i, tagPool.Size());
                if (i == 0U) {
                    break;
                }
                --i;
                if ((tagPool.ComponentAtDense(i).categoryMask & kRoomTransientCategoryBit) != 0U) {
                    (void)loadCtx.world->Destroy(tagPool.Entities()[i], true);
                }
            }
        }
    }

    if (loadCtx.physics != nullptr) {
        loadCtx.physics->Clear();
    }
    if (loadCtx.gameplay != nullptr) {
        loadCtx.gameplay->Clear();
    }

    m_activeScope = m_stagingScope;
    m_stagingScope = 0xFFU;
    m_activeSceneId = m_pendingScene.sceneId;
    m_activeSceneDeactivateFn = m_pendingScene.onDeactivating;
    loadCtx.sceneScope = m_activeScope;

    if (validColBlob.IsValid() && loadCtx.physics != nullptr) {
        if (auto r = loadCtx.physics->BindCollisionMapBlob(assetBank, validColBlob); !r) {
            return rollbackPostSwap(FrameworkErrc::SimAssetWaitFailed);
        }
    }
    if (validCueBlob.IsValid()) {
        if (auto r = loadCtx.audio->RegisterCueBank(assetBank.GetBlob(validCueBlob), m_activeScope); !r) {
            return rollbackPostSwap(FrameworkErrc::SimAssetWaitFailed);
        }
    }
    if (validPrefabBlob.IsValid()) {
        if (auto r = WorldExtractor::InstantiatePrefab(*loadCtx.world, assetBank, validPrefabBlob); !r) {
            return rollbackPostSwap(FrameworkErrc::SimAssetWaitFailed);
        }
    }
    if (m_pendingScene.onActivated != nullptr) {
        if (auto r = m_pendingScene.onActivated(loadCtx); !r) {
            return rollbackPostSwap(r.error());
        }
    }

    freeRollbackSlice();

    if (oldScope < kMaxAssetScopes && oldScope != m_persistentScope && oldScope != m_activeScope) {
        loadCtx.audio->RequestScopeRelease(oldScope);
        if ((loadCtx.audio->PollReleasableScopeMask() & (1U << oldScope)) != 0U) {
            assetBank.EndScope(oldScope);
        }
    }

    const u32 targetMusicId = (m_pendingScene.musicTrackId != 0U) ? m_pendingScene.musicTrackId : manifestMusicTrackId;
    if (targetMusicId != 0U) {
        (void)loadCtx.audio->TransitionMusic(targetMusicId, assetBank, m_pendingScene.musicCrossfadeBlocks);
    }

    ++m_transitionCount;

    if (m_activeWipe != TransitionWipeKind::None && m_wipeDurationTicks > 0U) {
        m_wipeElapsedTicks = m_wipeDurationTicks;
        m_phase = TransitionPhase::FadingIn;
    } else {
        m_wipeElapsedTicks = 0U;
        m_phase = TransitionPhase::Idle;
        m_activeWipe = TransitionWipeKind::None;
    }
    return true;
}

Result<bool, FrameworkErrc> SceneDirector::CommitSafeFramePoint(SceneLoadContext& loadCtx) noexcept {
    if (!HasPendingWork()) {
        return false;
    }

    bool didMutate = false;

    if (m_phase == TransitionPhase::StreamingSceneAssets) {
        auto actRes = ActivatePendingScene(loadCtx);
        if (!actRes) {
            return Err(actRes.error());
        }
        if (*actRes) {
            didMutate = true;
            loadCtx.sceneScope = m_activeScope;
        }
    }

    const u8 cmdCount = m_cmdCount;
    m_cmdCount = 0U;

    const auto popTopLayer = [&]() noexcept {
        const u8 idx = --m_stackDepth;
        if (m_exitFns[idx] != nullptr) {
            m_exitFns[idx](loadCtx, m_userStates[idx]);
        }
        didMutate = true;
    };

    for (u8 i = 0U; i < cmdCount; ++i) {
        const StateLayerDesc& cmd = m_pendingCmds[i];
        if (cmd.cmdKind == StateCmdKind::Pop) {
            if (m_stackDepth == 0U) {
                RebakeStackMasks();
                return Err(FrameworkErrc::StateStackUnderflow);
            }
            popTopLayer();
            continue;
        }
        if (cmd.cmdKind == StateCmdKind::Switch && m_stackDepth > 0U) {
            popTopLayer();
        } else if (cmd.cmdKind == StateCmdKind::ClearAndPush) {
            while (m_stackDepth > 0U) {
                popTopLayer();
            }
        }
        if (m_stackDepth >= kMaxStateStackDepth) {
            RebakeStackMasks();
            return Err(FrameworkErrc::StateStackOverflow);
        }
        const u8 idx = m_stackDepth++;
        m_layerFlags[idx] = cmd.flags;
        m_stateIds[idx] = cmd.id;
        m_inputMasks[idx] = cmd.inputActionMask;
        m_simFns[idx] = cmd.onSimTick;
        m_renderFns[idx] = cmd.onRenderExtract;
        m_userStates[idx] = cmd.stateData;
        m_exitFns[idx] = cmd.onExit;
        if (cmd.onEnter != nullptr) {
            cmd.onEnter(loadCtx, cmd.stateData);
        }
        didMutate = true;
    }

    if (didMutate) {
        RebakeStackMasks();
    }

    return didMutate;
}

void SceneDirector::RebakeStackMasks() noexcept {
    u8 simMask = 0U;
    u8 renderMask = 0U;
    u8 pauseMask = 0U;
    u16 topInput = 0xFFFFU;

    bool simBlocked = false;
    bool renderBlocked = false;
    bool inputResolved = false;

    for (i32 i = static_cast<i32>(m_stackDepth) - 1; i >= 0; --i) {
        const usize idx = static_cast<usize>(i);
        const StateLayerFlags flags = m_layerFlags[idx];
        const u8 bit = static_cast<u8>(1U << static_cast<u8>(i));

        if (!inputResolved) {
            topInput = m_inputMasks[idx];
            if (HasFlag(flags, StateLayerFlags::BlockLowerInput)) {
                inputResolved = true;
            }
        }
        if (HasFlag(flags, StateLayerFlags::PauseSimClock)) {
            pauseMask |= bit;
        }
        if (!simBlocked) {
            if (m_simFns[idx] != nullptr) {
                simMask |= bit;
            }
            if (HasFlag(flags, StateLayerFlags::BlockLowerSimTicks)) {
                simBlocked = true;
            }
        }
        if (!renderBlocked) {
            if (m_renderFns[idx] != nullptr) {
                renderMask |= bit;
            }
            if (HasFlag(flags, StateLayerFlags::BlockLowerRender)) {
                renderBlocked = true;
            }
        }
    }

    m_activeSimMask = simMask;
    m_activeRenderMask = renderMask;
    m_pausedClockMask = pauseMask;
    m_topInputMask = topInput;
}

void SceneDirector::DispatchSimTick(SimContext& simCtx, const SimStepView& step) const noexcept {
    for (u8 mask = m_activeSimMask; mask != 0U; mask &= static_cast<u8>(mask - 1U)) {
        const u8 idx = static_cast<u8>(std::countr_zero(mask));
        m_simFns[idx](simCtx, step, m_userStates[idx]);
    }
}

void SceneDirector::DispatchRenderExtract(RenderContext& renderCtx) const noexcept {
    for (u8 mask = m_activeRenderMask; mask != 0U; mask &= static_cast<u8>(mask - 1U)) {
        const u8 idx = static_cast<u8>(std::countr_zero(mask));
        m_renderFns[idx](renderCtx, m_userStates[idx]);
    }
}

void SceneDirector::EmitTransitionOverlay(RenderList& outList, ViewportExtent viewportSize) const noexcept {
    const f32 progress = (m_phase == TransitionPhase::StreamingSceneAssets) ? 1.0f : TransitionProgress();
    if (progress <= 1e-4f || m_activeWipe == TransitionWipeKind::None || !viewportSize.IsValid()) {
        return;
    }

    const f32 vw = static_cast<f32>(viewportSize.w);
    const f32 vh = static_cast<f32>(viewportSize.h);
    const Vec2 center{vw * 0.5f, vh * 0.5f};

    QuadCmd cmd{};
    cmd.center = center;
    cmd.size = {vw, vh};
    cmd.color = Color{0.03f, 0.03f, 0.05f, 1.0f};
    cmd.texture = kWhiteTexture;
    cmd.depth = 65500U;
    cmd.layer = 250U;
    cmd.viewSlot = RenderList::kHudViewSlot;
    cmd.blend = BlendMode::Alpha;

    switch (m_activeWipe) {
        case TransitionWipeKind::FadeToBlack: {
            cmd.color = Color{0.0f, 0.0f, 0.0f, progress};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::HorizontalCurtain: {
            const f32 panelW = center.x * progress;
            cmd.size.x = panelW;
            cmd.center.x = panelW * 0.5f;
            outList.PushQuad(cmd);
            cmd.center.x = vw - panelW * 0.5f;
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::VerticalCurtain: {
            const f32 panelH = center.y * progress;
            cmd.size.y = panelH;
            cmd.center.y = panelH * 0.5f;
            outList.PushQuad(cmd);
            cmd.center.y = vh - panelH * 0.5f;
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::DiamondIris: {
            const f32 curDim = std::sqrt(vw * vw + vh * vh) * 1.42f * progress;
            cmd.size = {curDim, curDim};
            cmd.cosRot = 0.70710678f;
            cmd.sinRot = 0.70710678f;
            cmd.color = Color{0.02f, 0.02f, 0.04f, 1.0f};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::None:
            break;
    }
}

} // namespace en

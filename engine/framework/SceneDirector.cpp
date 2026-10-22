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
    m_activeSimMask = 0U;
    m_activeRenderMask = 0U;
    m_pausedClockMask = 0U;
    m_stackDepth = 0U;
    m_cmdCount = 0U;
    m_phase = TransitionPhase::Idle;
    m_persistentScope = 0xFFU;
    m_activeScope = 0xFFU;
    m_stagingScope = 0xFFU;
    m_wipeElapsedTicks = 0U;
    m_wipeDurationTicks = 0U;
    m_activeWipe = TransitionWipeKind::None;
    m_topInputMask = 0xFFFFU;
    m_transitionCount = 0U;
    m_layerFlags.fill(StateLayerFlags::None);
    m_stateIds.fill(kInvalidStateId);
    m_inputMasks.fill(0xFFFFU);
    m_activeSceneId = AssetId{};
    m_activeSceneDeactivateFn = nullptr;
    m_simFns.fill(nullptr);
    m_renderFns.fill(nullptr);
    m_userStates.fill(nullptr);
    m_exitFns.fill(nullptr);
    m_pendingScene = SceneTransitionDesc{};

    auto pScope = assetBank.BeginScope();
    if (!pScope) return Err(FrameworkErrc::ScopeAllocationFailed);
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
    if (optionalLoadCtx) {
        while (m_stackDepth > 0U) {
            const u8 top = --m_stackDepth;
            if (m_exitFns[top]) {
                m_exitFns[top](*optionalLoadCtx, m_userStates[top]);
            }
        }
        if (m_activeSceneDeactivateFn) {
            m_activeSceneDeactivateFn(*optionalLoadCtx);
            m_activeSceneDeactivateFn = nullptr;
        }
    } else {
        m_stackDepth = 0U;
        m_activeSceneDeactivateFn = nullptr;
    }

    m_cmdCount = 0U;
    m_activeSimMask = 0U;
    m_activeRenderMask = 0U;
    m_pausedClockMask = 0U;
    m_topInputMask = 0xFFFFU;

    const auto releaseScope = [&](u8& scope) noexcept {
        if (scope < kMaxAssetScopes) {
            audio.RequestScopeRelease(scope);
            assetBank.EndScope(scope);
            scope = 0xFFU;
        }
    };
    releaseScope(m_stagingScope);
    releaseScope(m_activeScope);
    releaseScope(m_persistentScope);

    m_phase = TransitionPhase::Idle;
    m_activeWipe = TransitionWipeKind::None;
    m_activeSceneId = AssetId{};
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
    const auto stageBlob = [&](AssetId id) noexcept {
        if (id.IsValid() && !assetBank.FindBlobHandle(id).IsValid()) {
            (void)assetBank.AcquireSlot(id, AssetKind::Blob, m_stagingScope, AssetResidency::Sim, needsLoad);
        }
    };

    stageBlob(desc.manifestBlobId);
    stageBlob(desc.collisionMapId);
    stageBlob(desc.rootPrefabId);
    stageBlob(desc.cueBankId);

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
        if (m_wipeElapsedTicks < m_wipeDurationTicks) {
            ++m_wipeElapsedTicks;
        }
        if (m_wipeElapsedTicks >= m_wipeDurationTicks) {
            m_phase = TransitionPhase::StreamingSceneAssets;
        }
    } else if (m_phase == TransitionPhase::FadingIn) {
        if (m_wipeElapsedTicks > 0U) {
            --m_wipeElapsedTicks;
        }
        if (m_wipeElapsedTicks == 0U) {
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
    u32 manifestMusicTrackId = 0U;

    // 1. Parse and verify optional CookedSceneManifestHeader ("ESCN" v1) if present
    if (m_pendingScene.manifestBlobId.IsValid()) {
        const BlobHandle manifestBlob = assetBank.FindBlobHandle(m_pendingScene.manifestBlobId);
        if (manifestBlob.IsValid()) {
            const BlobRecord& rec = assetBank.GetBlob(manifestBlob);
            if (rec.data != nullptr && rec.size >= sizeof(CookedSceneManifestHeader)) {
                CookedSceneManifestHeader hdr{};
                std::memcpy(&hdr, rec.data, sizeof(CookedSceneManifestHeader));
                const usize expectedBytes = sizeof(CookedSceneManifestHeader) +
                                            static_cast<usize>(hdr.assetEntryCount) * sizeof(CookedSceneAssetEntry);
                if (hdr.magic != CookedSceneManifestHeader::kMagic ||
                    hdr.version != CookedSceneManifestHeader::kVersion ||
                    rec.size < expectedBytes) {
                    return Err(FrameworkErrc::CorruptedSceneManifest);
                }
                const u8* entriesBytes = rec.data + sizeof(CookedSceneManifestHeader);
                if (hdr.checksum != 0U) {
                    u32 h = 2166136261U;
                    for (u32 i = 0U; i < hdr.assetEntryCount; ++i) {
                        CookedSceneAssetEntry e{};
                        std::memcpy(&e, entriesBytes + static_cast<usize>(i) * sizeof(CookedSceneAssetEntry), sizeof(CookedSceneAssetEntry));
                        const u64 idVal = e.id.value;
                        for (u32 b = 0U; b < 8U; ++b) {
                            h ^= static_cast<u32>((idVal >> (b * 8U)) & 0xFFU);
                            h *= 16777619U;
                        }
                        h ^= static_cast<u32>(e.kind);
                        h *= 16777619U;
                        h ^= static_cast<u32>(e.residency);
                        h *= 16777619U;
                    }
                    const u32 actualChecksum = (h == 0U) ? 1U : h;
                    if (actualChecksum != hdr.checksum) {
                        return Err(FrameworkErrc::CorruptedSceneManifest);
                    }
                }
                manifestMusicTrackId = hdr.musicTrackId;
                bool needsLoad = false;
                for (u32 i = 0U; i < hdr.assetEntryCount; ++i) {
                    CookedSceneAssetEntry e{};
                    std::memcpy(&e, entriesBytes + static_cast<usize>(i) * sizeof(CookedSceneAssetEntry), sizeof(CookedSceneAssetEntry));
                    if (e.id.IsValid()) {
                        (void)assetBank.AcquireSlot(e.id, e.kind, m_stagingScope, e.residency, needsLoad);
                    }
                }
            }
        }
    }

    // 2. Non-blocking check: wait until all Residency::Sim assets in m_stagingScope are ready
    if (!assetBank.IsScopeSimReady(m_stagingScope)) {
        return false;
    }

    // 3. Deactivate old scene at safe frame barrier
    const u8 oldScope = m_activeScope;
    loadCtx.sceneScope = oldScope;
    if (m_activeSceneDeactivateFn) {
        m_activeSceneDeactivateFn(loadCtx);
        m_activeSceneDeactivateFn = nullptr;
    }

    if (m_pendingScene.mode == SceneTransitionMode::FullReset) {
        loadCtx.world->Clear();
        if (loadCtx.physics) loadCtx.physics->Clear();
        if (loadCtx.gameplay) loadCtx.gameplay->Clear();
    } else {
        // RoomSwap: destroy temporary Lifetime2D entities and kRoomTransientCategoryBit actors while preserving persistent entities
        const u64 regMask = loadCtx.world->RegisteredPoolsMask();
        if ((regMask & MakeComponentMask<Lifetime2D>()) != 0ULL) {
            const auto& lifePool = loadCtx.world->GetPool<Lifetime2D>();
            for (u16 i = lifePool.Size(); i > 0U; --i) {
                (void)loadCtx.world->Destroy(lifePool.Entities()[i - 1U], true);
            }
        }
        if ((regMask & MakeComponentMask<Tag2D>()) != 0ULL) {
            const auto& tagPool = loadCtx.world->GetPool<Tag2D>();
            for (u16 i = tagPool.Size(); i > 0U; --i) {
                if ((tagPool.ComponentAtDense(i - 1U).categoryMask & kRoomTransientCategoryBit) != 0U) {
                    (void)loadCtx.world->Destroy(tagPool.Entities()[i - 1U], true);
                }
            }
        }
        if (loadCtx.physics) loadCtx.physics->Clear();
        if (loadCtx.gameplay) loadCtx.gameplay->Clear();
    }

    if (oldScope < kMaxAssetScopes && oldScope != m_persistentScope) {
        loadCtx.audio->RequestScopeRelease(oldScope);
        const u32 releasable = loadCtx.audio->PollReleasableScopeMask();
        if ((releasable & (1U << oldScope)) != 0U) {
            assetBank.EndScope(oldScope);
        }
    }

    m_activeScope = m_stagingScope;
    m_stagingScope = 0xFFU;
    m_activeSceneId = m_pendingScene.sceneId;
    m_activeSceneDeactivateFn = m_pendingScene.onDeactivating;
    ++m_transitionCount;
    loadCtx.sceneScope = m_activeScope;

    // 4. Bind new scene resources (CollisionMap, SoundCueBank, RootPrefab, and Music Track)
    if (m_pendingScene.collisionMapId.IsValid() && loadCtx.physics) {
        const BlobHandle colBlob = assetBank.FindBlobHandle(m_pendingScene.collisionMapId);
        if (colBlob.IsValid()) {
            if (auto r = loadCtx.physics->BindCollisionMapBlob(assetBank, colBlob); !r) {
                return Err(FrameworkErrc::SimAssetWaitFailed);
            }
        }
    }
    if (m_pendingScene.cueBankId.IsValid()) {
        const BlobHandle cueBlob = assetBank.FindBlobHandle(m_pendingScene.cueBankId);
        if (cueBlob.IsValid()) {
            const BlobRecord& cueRec = assetBank.GetBlob(cueBlob);
            if (cueRec.IsValid()) {
                (void)loadCtx.audio->RegisterCueBank(cueRec, m_activeScope);
            }
        }
    }
    if (m_pendingScene.rootPrefabId.IsValid()) {
        const BlobHandle prefBlob = assetBank.FindBlobHandle(m_pendingScene.rootPrefabId);
        if (prefBlob.IsValid()) {
            const BlobRecord& prefRec = assetBank.GetBlob(prefBlob);
            if (prefRec.IsValid()) {
                if (auto r = WorldExtractor::InstantiatePrefab(*loadCtx.world, assetBank, prefBlob); !r) {
                    return Err(FrameworkErrc::SimAssetWaitFailed);
                }
            }
        }
    }

    const u32 targetMusicId = (m_pendingScene.musicTrackId != 0U) ? m_pendingScene.musicTrackId : manifestMusicTrackId;
    if (targetMusicId != 0U) {
        (void)loadCtx.audio->TransitionMusic(targetMusicId, assetBank, m_pendingScene.musicCrossfadeBlocks);
    }

    // 5. Invoke client scene activation callback
    if (m_pendingScene.onActivated) {
        if (auto r = m_pendingScene.onActivated(loadCtx); !r) {
            return Err(r.error());
        }
    }

    // 6. Transition to FadingIn or Idle
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
    bool didMutate = false;
    const u8 cmdCount = m_cmdCount;
    m_cmdCount = 0U;

    const auto pushLayer = [&](const StateLayerDesc& d) noexcept -> Result<void, FrameworkErrc> {
        if (m_stackDepth >= kMaxStateStackDepth) {
            return Err(FrameworkErrc::StateStackOverflow);
        }
        const u8 idx = m_stackDepth++;
        m_layerFlags[idx] = d.flags;
        m_stateIds[idx] = d.id;
        m_inputMasks[idx] = d.inputActionMask;
        m_simFns[idx] = d.onSimTick;
        m_renderFns[idx] = d.onRenderExtract;
        m_userStates[idx] = d.stateData;
        m_exitFns[idx] = d.onExit;
        if (d.onEnter) {
            d.onEnter(loadCtx, d.stateData);
        }
        didMutate = true;
        return {};
    };

    const auto popTopLayer = [&]() noexcept -> Result<void, FrameworkErrc> {
        if (m_stackDepth == 0U) {
            return Err(FrameworkErrc::StateStackUnderflow);
        }
        const u8 idx = --m_stackDepth;
        if (m_exitFns[idx]) {
            m_exitFns[idx](loadCtx, m_userStates[idx]);
        }
        m_layerFlags[idx] = StateLayerFlags::None;
        m_stateIds[idx] = kInvalidStateId;
        m_inputMasks[idx] = 0xFFFFU;
        m_simFns[idx] = nullptr;
        m_renderFns[idx] = nullptr;
        m_userStates[idx] = nullptr;
        m_exitFns[idx] = nullptr;
        didMutate = true;
        return {};
    };

    for (u8 i = 0U; i < cmdCount; ++i) {
        const StateLayerDesc& cmd = m_pendingCmds[i];
        switch (cmd.cmdKind) {
            case StateCmdKind::Push: {
                if (auto r = pushLayer(cmd); !r) {
                    RebakeStackMasks();
                    return Err(r.error());
                }
                break;
            }
            case StateCmdKind::Pop: {
                if (auto r = popTopLayer(); !r) {
                    RebakeStackMasks();
                    return Err(r.error());
                }
                break;
            }
            case StateCmdKind::Switch: {
                if (m_stackDepth > 0U) {
                    (void)popTopLayer();
                }
                if (auto r = pushLayer(cmd); !r) {
                    RebakeStackMasks();
                    return Err(r.error());
                }
                break;
            }
            case StateCmdKind::ClearAndPush: {
                while (m_stackDepth > 0U) {
                    (void)popTopLayer();
                }
                if (auto r = pushLayer(cmd); !r) {
                    RebakeStackMasks();
                    return Err(r.error());
                }
                break;
            }
        }
    }

    if (didMutate) {
        RebakeStackMasks();
    }

    if (m_phase == TransitionPhase::StreamingSceneAssets) {
        auto actRes = ActivatePendingScene(loadCtx);
        if (!actRes) return Err(actRes.error());
        if (*actRes) didMutate = true;
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
            if (HasStateFlag(flags, StateLayerFlags::BlockLowerInput)) {
                inputResolved = true;
            }
        }
        if (HasStateFlag(flags, StateLayerFlags::PauseSimClock)) {
            pauseMask |= bit;
        }
        if (!simBlocked) {
            if (m_simFns[idx] != nullptr) simMask |= bit;
            if (HasStateFlag(flags, StateLayerFlags::BlockLowerSimTicks)) {
                simBlocked = true;
            }
        }
        if (!renderBlocked) {
            if (m_renderFns[idx] != nullptr) renderMask |= bit;
            if (HasStateFlag(flags, StateLayerFlags::BlockLowerRender)) {
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
    u8 mask = m_activeSimMask;
    while (mask != 0U) {
        const u8 idx = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_simFns[idx]) {
            m_simFns[idx](simCtx, step, m_userStates[idx]);
        }
    }
}

void SceneDirector::DispatchRenderExtract(RenderContext& renderCtx) const noexcept {
    u8 mask = m_activeRenderMask;
    while (mask != 0U) {
        const u8 idx = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        if (m_renderFns[idx]) {
            m_renderFns[idx](renderCtx, m_userStates[idx]);
        }
    }
}

void SceneDirector::EmitTransitionOverlay(RenderList& outList, ViewportExtent viewportSize) const noexcept {
    const f32 progress = (m_phase == TransitionPhase::StreamingSceneAssets) ? 1.0f : TransitionProgress();
    if (progress <= 1e-4f || m_activeWipe == TransitionWipeKind::None || !viewportSize.IsValid()) {
        return;
    }

    const f32 vw = static_cast<f32>(viewportSize.w);
    const f32 vh = static_cast<f32>(viewportSize.h);

    QuadCmd cmd{};
    cmd.texture = kWhiteTexture;
    cmd.depth = 65500U;
    cmd.layer = 250U;
    cmd.viewSlot = RenderList::kHudViewSlot;
    cmd.blend = BlendMode::Alpha;

    switch (m_activeWipe) {
        case TransitionWipeKind::FadeToBlack: {
            cmd.center = {vw * 0.5f, vh * 0.5f};
            cmd.size = {vw, vh};
            cmd.color = Color{0.0f, 0.0f, 0.0f, progress};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::HorizontalCurtain: {
            const f32 curW = vw * progress;
            cmd.center = {curW * 0.5f, vh * 0.5f};
            cmd.size = {curW, vh};
            cmd.color = Color{0.0f, 0.0f, 0.0f, 1.0f};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::VerticalCurtain: {
            const f32 curH = vh * progress;
            cmd.center = {vw * 0.5f, curH * 0.5f};
            cmd.size = {vw, curH};
            cmd.color = Color{0.0f, 0.0f, 0.0f, 1.0f};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::DiamondIris: {
            const f32 maxDim = std::max(vw, vh) * 1.42f * progress;
            cmd.center = {vw * 0.5f, vh * 0.5f};
            cmd.size = {maxDim, maxDim};
            cmd.cosRot = 0.70710678f;
            cmd.sinRot = 0.70710678f;
            cmd.color = Color{0.0f, 0.0f, 0.0f, 1.0f};
            outList.PushQuad(cmd);
            break;
        }
        case TransitionWipeKind::None:
            break;
    }
}

} // namespace en

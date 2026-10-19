#include "framework/SceneDirector.h"

#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "world/World.h"
#include "world/WorldExtractor.h"

#include <bit>
#include <cstring>

namespace en::framework {

namespace {

[[nodiscard]] constexpr bool IsStagedBlobValid(
    const assets::AssetBank& bank,
    assets::BlobHandle handle) noexcept {
    if (handle.index == 0) {
        return true;
    }
    const auto& rec = bank.GetBlob(handle);
    return rec.data != nullptr && rec.size > 0;
}

} // namespace

Result<void, FrameworkErrc> SceneDirector::Init(assets::AssetSystem& assets) noexcept {
    auto scopeRes = assets.BeginScope();
    if (!scopeRes) {
        return Err(FrameworkErrc::ScopeAllocationFailed);
    }
    m_persistentScope = *scopeRes;
    m_activeScope = assets::kInvalidScope;
    m_stagingScope = assets::kInvalidScope;
    m_activeSimMask = 0;
    m_activeRenderMask = 0;
    m_pausedClockMask = 0;
    m_stackDepth = 0;
    m_cmdCount = 0;
    m_topInputMask = 0xFFFF;
    m_phase = TransitionPhase::Idle;
    m_activeSceneId = {};
    m_activeSceneDeactivateFn = nullptr;
    return {};
}

void SceneDirector::Shutdown(
    assets::AssetSystem& assets,
    audio::AudioSystem& audio,
    SceneLoadContext* optionalLoadCtx) noexcept {
    while (m_stackDepth > 0) {
        const u8 idx = --m_stackDepth;
        if (optionalLoadCtx && m_exitFns[idx]) {
            optionalLoadCtx->sceneScope = m_activeScope;
            optionalLoadCtx->persistentScope = m_persistentScope;
            m_exitFns[idx](*optionalLoadCtx, m_userStates[idx]);
        }
    }

    if (m_activeScope != assets::kInvalidScope && optionalLoadCtx && m_activeSceneDeactivateFn) {
        optionalLoadCtx->sceneScope = m_activeScope;
        optionalLoadCtx->persistentScope = m_persistentScope;
        m_activeSceneDeactivateFn(*optionalLoadCtx, m_activeSceneId);
    }
    m_activeSceneDeactivateFn = nullptr;

    auto releaseScope = [&](assets::ScopeId& scope) noexcept {
        if (scope != assets::kInvalidScope) {
            audio.RequestScopeRelease(scope);
            assets.EndScope(scope);
            scope = assets::kInvalidScope;
        }
    };

    releaseScope(m_stagingScope);
    releaseScope(m_activeScope);
    releaseScope(m_persistentScope);

    m_activeSimMask = 0;
    m_activeRenderMask = 0;
    m_pausedClockMask = 0;
    m_cmdCount = 0;
    m_topInputMask = 0xFFFF;
    m_phase = TransitionPhase::Idle;
    m_activeSceneId = {};
}

void SceneDirector::AbortStagingScope(assets::AssetSystem& assets) noexcept {
    if (m_stagingScope != assets::kInvalidScope) {
        assets.EndScope(m_stagingScope);
        m_stagingScope = assets::kInvalidScope;
    }
    m_stagedManifestBlob = {};
    m_stagedCollisionBlob = {};
    m_stagedPrefabBlob = {};
    m_stagedCueBankBlob = {};
    m_phase = TransitionPhase::Idle;
}

Result<void, FrameworkErrc> SceneDirector::RequestSceneTransition(
    const SceneTransitionDesc& desc,
    assets::AssetSystem& assets) noexcept {
    if (m_phase != TransitionPhase::Idle) {
        return Err(FrameworkErrc::SceneTransitionBusy);
    }

    auto scopeRes = assets.BeginScope();
    if (!scopeRes) {
        return Err(FrameworkErrc::ScopeAllocationFailed);
    }
    m_stagingScope = *scopeRes;
    m_pendingScene = desc;
    m_stagedManifestBlob = {};
    m_stagedCollisionBlob = {};
    m_stagedPrefabBlob = {};
    m_stagedCueBankBlob = {};

    if (desc.manifestBlobId.value != 0) {
        auto mh = assets.PinBlob(m_stagingScope, desc.manifestBlobId, assets::Residency::Sim);
        if (!mh) {
            AbortStagingScope(assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
        m_stagedManifestBlob = *mh;
        m_phase = TransitionPhase::WaitingManifestBlob;
        return {};
    }

    return PinScenePayloads(nullptr, 0, assets);
}

Result<void, FrameworkErrc> SceneDirector::PinScenePayloads(
    const u8* rawEntries,
    u16 entryCount,
    assets::AssetSystem& assets) noexcept {
    auto pinSimBlob = [&](assets::AssetId id, assets::BlobHandle& outHandle) noexcept -> bool {
        if (id.value == 0) {
            return true;
        }
        auto h = assets.PinBlob(m_stagingScope, id, assets::Residency::Sim);
        if (!h) {
            AbortStagingScope(assets);
            return false;
        }
        outHandle = *h;
        return true;
    };

    if (!pinSimBlob(m_pendingScene.collisionMapId, m_stagedCollisionBlob) ||
        !pinSimBlob(m_pendingScene.rootPrefabId, m_stagedPrefabBlob) ||
        !pinSimBlob(m_pendingScene.cueBankId, m_stagedCueBankBlob)) {
        return Err(FrameworkErrc::SimAssetWaitFailed);
    }

    for (u16 i = 0; i < entryCount; ++i) {
        CookedSceneAssetEntry entry{};
        std::memcpy(&entry, rawEntries + static_cast<usize>(i) * sizeof(CookedSceneAssetEntry),
                    sizeof(CookedSceneAssetEntry));
        if (entry.kind != assets::AssetKind::None && entry.id.value != 0) {
            (void)assets.Pin(m_stagingScope, entry.id, entry.kind, entry.residency);
        }
    }

    m_phase = TransitionPhase::StreamingSceneAssets;
    return {};
}

Result<bool, FrameworkErrc> SceneDirector::StepSceneTransition(
    SceneLoadContext& loadCtx) noexcept {
    const assets::AssetBank& bank = loadCtx.assets.Bank();

    auto ensureSimReady = [&]() noexcept -> Result<bool, FrameworkErrc> {
        if (bank.IsScopeSimReady(m_stagingScope)) {
            return true;
        }
        if (m_pendingScene.waitPolicy != SceneWaitPolicy::ImmediateBarrier) {
            return false;
        }
        if (auto w = loadCtx.assets.WaitSimReady(m_stagingScope, loadCtx.renderer); !w) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
        return true;
    };

    if (m_phase == TransitionPhase::WaitingManifestBlob) {
        auto ready = ensureSimReady();
        if (!ready || !*ready) {
            return ready;
        }

        const auto& blob = bank.GetBlob(m_stagedManifestBlob);
        if (blob.data == nullptr || blob.size < sizeof(CookedSceneManifestHeader)) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::CorruptedSceneManifest);
        }

        CookedSceneManifestHeader hdr{};
        std::memcpy(&hdr, blob.data, sizeof(CookedSceneManifestHeader));
        const usize expectedBytes =
            sizeof(CookedSceneManifestHeader) +
            static_cast<usize>(hdr.assetCount) * sizeof(CookedSceneAssetEntry);
        if (hdr.magic != kSceneManifestMagic ||
            hdr.version != kSceneManifestVersion ||
            blob.size < expectedBytes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::CorruptedSceneManifest);
        }

        if (m_pendingScene.collisionMapId.value == 0) { m_pendingScene.collisionMapId = hdr.collisionMapId; }
        if (m_pendingScene.rootPrefabId.value == 0)   { m_pendingScene.rootPrefabId = hdr.rootPrefabId; }
        if (m_pendingScene.cueBankId.value == 0)      { m_pendingScene.cueBankId = hdr.cueBankId; }
        if (m_pendingScene.musicTrackId == 0 && hdr.musicTrackId != 0) {
            m_pendingScene.musicTrackId = hdr.musicTrackId;
            m_pendingScene.musicCrossfadeBlocks = hdr.musicCrossfadeBlocks;
        }

        if (auto pinRes = PinScenePayloads(
                blob.data + sizeof(CookedSceneManifestHeader), hdr.assetCount, loadCtx.assets); !pinRes) {
            return Err(pinRes.error());
        }
        if (m_pendingScene.waitPolicy == SceneWaitPolicy::StreamNonBlocking) {
            return false;
        }
    }

    if (m_phase == TransitionPhase::StreamingSceneAssets) {
        auto ready = ensureSimReady();
        if (!ready || !*ready) {
            return ready;
        }
        if (auto actRes = ActivatePendingScene(loadCtx); !actRes) {
            return Err(actRes.error());
        }
        return true;
    }

    return false;
}

Result<bool, FrameworkErrc> SceneDirector::CommitSafeFramePoint(
    SceneLoadContext& loadCtx) noexcept {
    loadCtx.director = this;
    bool sceneActivated = false;

    if (m_phase != TransitionPhase::Idle) {
        auto stepRes = StepSceneTransition(loadCtx);
        if (!stepRes) {
            return stepRes;
        }
        sceneActivated = *stepRes;
    }

    if (m_cmdCount > 0) {
        loadCtx.sceneScope = m_activeScope;
        loadCtx.persistentScope = m_persistentScope;

        auto pushSlot = [&](const StateLayerDesc& d) noexcept {
            const u8 idx = m_stackDepth++;
            m_simFns[idx] = d.onSimTick;
            m_renderFns[idx] = d.onRenderExtract;
            m_userStates[idx] = d.stateData;
            m_exitFns[idx] = d.onExit;
            m_layerFlags[idx] = d.flags;
            m_stateIds[idx] = d.id;
            m_inputMasks[idx] = d.inputActionMask;
            if (d.onEnter) {
                d.onEnter(loadCtx, d.stateData);
            }
        };

        auto popSlot = [&]() noexcept {
            const u8 idx = --m_stackDepth;
            if (m_exitFns[idx]) {
                m_exitFns[idx](loadCtx, m_userStates[idx]);
            }
        };

        for (u8 i = 0; i < m_cmdCount; ++i) {
            const StateLayerDesc cmd = m_cmdQueue[i];
            switch (static_cast<StateCmdKind>(cmd.cmdKind)) {
                case StateCmdKind::Push:
                    if (m_stackDepth >= kMaxStateStackDepth) {
                        m_cmdCount = 0;
                        RebakeStackMasks();
                        return Err(FrameworkErrc::StateStackOverflow);
                    }
                    pushSlot(cmd);
                    break;

                case StateCmdKind::Pop:
                    if (m_stackDepth == 0) {
                        m_cmdCount = 0;
                        RebakeStackMasks();
                        return Err(FrameworkErrc::StateStackUnderflow);
                    }
                    popSlot();
                    break;

                case StateCmdKind::Switch:
                    if (m_stackDepth > 0) {
                        popSlot();
                    }
                    pushSlot(cmd);
                    break;

                case StateCmdKind::ClearAndPush:
                    while (m_stackDepth > 0) {
                        popSlot();
                    }
                    pushSlot(cmd);
                    break;
            }
        }
        m_cmdCount = 0;
        RebakeStackMasks();

        if (m_phase != TransitionPhase::Idle) {
            auto stepRes = StepSceneTransition(loadCtx);
            if (!stepRes) {
                return stepRes;
            }
            sceneActivated |= *stepRes;
        }
    }

    return sceneActivated;
}

constexpr void SceneDirector::RebakeStackMasks() noexcept {
    u8 simMask = 0;
    u8 renderMask = 0;
    u8 pauseMask = 0;
    u16 inputMask = 0;
    bool simBlocked = false;
    bool renderBlocked = false;
    bool inputBlocked = false;

    for (u8 i = m_stackDepth; i > 0; --i) {
        const u8 slot = static_cast<u8>(i - 1);
        const u8 bit = static_cast<u8>(1u << slot);
        const StateLayerFlags f = m_layerFlags[slot];

        if (!simBlocked && m_simFns[slot] != nullptr)       { simMask |= bit; }
        if (!renderBlocked && m_renderFns[slot] != nullptr) { renderMask |= bit; }
        if (!inputBlocked)                                  { inputMask |= m_inputMasks[slot]; }
        if (HasStateFlag(f, StateLayerFlags::PauseSimClock))      { pauseMask |= bit; }
        if (HasStateFlag(f, StateLayerFlags::BlockLowerSimTicks)) { simBlocked = true; }
        if (HasStateFlag(f, StateLayerFlags::BlockLowerRender))   { renderBlocked = true; }
        if (HasStateFlag(f, StateLayerFlags::BlockLowerInput))    { inputBlocked = true; }
    }

    m_activeSimMask = simMask;
    m_activeRenderMask = renderMask;
    m_pausedClockMask = pauseMask;
    m_topInputMask = (m_stackDepth == 0) ? 0xFFFFu : inputMask;
}

Result<void, FrameworkErrc> SceneDirector::ActivatePendingScene(
    SceneLoadContext& loadCtx) noexcept {
    const assets::AssetBank& bank = loadCtx.assets.Bank();

    if (!IsStagedBlobValid(bank, m_stagedCueBankBlob) ||
        !IsStagedBlobValid(bank, m_stagedCollisionBlob) ||
        !IsStagedBlobValid(bank, m_stagedPrefabBlob)) {
        AbortStagingScope(loadCtx.assets);
        return Err(FrameworkErrc::SimAssetWaitFailed);
    }

    if (m_stagedCueBankBlob.index != 0) {
        if (auto cueRes = loadCtx.audio.RegisterCueBank(
                bank.GetBlob(m_stagedCueBankBlob), m_stagingScope); !cueRes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    if (m_stagedCollisionBlob.index != 0) {
        if (auto colRes = loadCtx.gameplay.SetActiveRoomCollision(
                bank, m_stagedCollisionBlob); !colRes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    if (m_activeScope != assets::kInvalidScope) {
        const SceneDeactivateFn deactFn =
            m_activeSceneDeactivateFn ? m_activeSceneDeactivateFn : m_pendingScene.onDeactivating;
        if (deactFn) {
            loadCtx.sceneScope = m_activeScope;
            deactFn(loadCtx, m_activeSceneId);
        }
    }

    if (m_pendingScene.mode == SceneTransitionMode::FullReset) {
        loadCtx.world.Clear();
    }

    if (m_stagedPrefabBlob.index != 0) {
        if (auto pfRes = world::WorldExtractor::InstantiatePrefab(
                loadCtx.world, bank, m_stagedPrefabBlob); !pfRes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    if (m_pendingScene.musicTrackId != 0) {
        loadCtx.audio.TransitionMusic(
            m_pendingScene.musicTrackId,
            bank,
            m_pendingScene.musicCrossfadeBlocks);
    }

    const assets::ScopeId oldScope = m_activeScope;
    m_activeScope = m_stagingScope;
    m_stagingScope = assets::kInvalidScope;
    m_activeSceneId = m_pendingScene.sceneId;
    m_activeSceneDeactivateFn = m_pendingScene.onDeactivating;
    m_phase = TransitionPhase::Idle;

    if (oldScope != assets::kInvalidScope) {
        loadCtx.audio.RequestScopeRelease(oldScope);
    }

    if (m_pendingScene.onActivated) {
        loadCtx.sceneScope = m_activeScope;
        if (!m_pendingScene.onActivated(loadCtx, m_activeSceneId)) {
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    return {};
}

void SceneDirector::DispatchSimTick(SimContext& simCtx) const noexcept {
    for (u32 mask = m_activeSimMask; mask != 0; mask &= (mask - 1u)) {
        const u32 slot = static_cast<u32>(std::countr_zero(mask));
        simCtx.userState = m_userStates[slot];
        m_simFns[slot](simCtx);
    }
}

void SceneDirector::DispatchRenderExtract(RenderContext& renderCtx) const noexcept {
    for (u32 mask = m_activeRenderMask; mask != 0; mask &= (mask - 1u)) {
        const u32 slot = static_cast<u32>(std::countr_zero(mask));
        renderCtx.userState = m_userStates[slot];
        m_renderFns[slot](renderCtx);
    }
}

} // namespace en::framework

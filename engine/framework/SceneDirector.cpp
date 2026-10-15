#include "framework/SceneDirector.h"

#include <bit>
#include <cstring>

#include "world/WorldExtractor.h"

namespace en::framework {

namespace {

[[nodiscard]] constexpr u64 SplitMix64(u64 x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

[[nodiscard]] constexpr bool IsBlobReady(
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
    m_stackDepth = 0;
    m_activeSimMask = 0;
    m_activeRenderMask = 0;
    m_pausedClockMask = 0;
    m_cmdCount = 0;
    m_phase = TransitionPhase::Idle;
    return Ok;
}

void SceneDirector::Shutdown(assets::AssetSystem& assets, audio::AudioSystem& audio) noexcept {
    if (m_stagingScope != assets::kInvalidScope) {
        audio.RequestScopeRelease(m_stagingScope, assets);
        m_stagingScope = assets::kInvalidScope;
    }
    if (m_activeScope != assets::kInvalidScope) {
        audio.RequestScopeRelease(m_activeScope, assets);
        m_activeScope = assets::kInvalidScope;
    }
    if (m_persistentScope != assets::kInvalidScope) {
        audio.RequestScopeRelease(m_persistentScope, assets);
        m_persistentScope = assets::kInvalidScope;
    }
    m_stackDepth = 0;
    m_activeSimMask = 0;
    m_activeRenderMask = 0;
    m_pausedClockMask = 0;
    m_cmdCount = 0;
    m_phase = TransitionPhase::Idle;
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

Result<void, FrameworkErrc> SceneDirector::PushState(const StateLayerDesc& desc) noexcept {
    if (m_cmdCount >= kMaxPendingStateCmds) {
        return Err(FrameworkErrc::StateStackOverflow);
    }
    m_cmdQueue[m_cmdCount++] = PendingStateCmd{.desc = desc, .kind = StateCmdKind::Push};
    return Ok;
}

Result<void, FrameworkErrc> SceneDirector::PopState() noexcept {
    if (m_cmdCount >= kMaxPendingStateCmds) {
        return Err(FrameworkErrc::StateStackOverflow);
    }
    m_cmdQueue[m_cmdCount++] = PendingStateCmd{.desc = {}, .kind = StateCmdKind::Pop};
    return Ok;
}

Result<void, FrameworkErrc> SceneDirector::SwitchState(const StateLayerDesc& desc) noexcept {
    if (m_cmdCount >= kMaxPendingStateCmds) {
        return Err(FrameworkErrc::StateStackOverflow);
    }
    m_cmdQueue[m_cmdCount++] = PendingStateCmd{.desc = desc, .kind = StateCmdKind::Switch};
    return Ok;
}

Result<void, FrameworkErrc> SceneDirector::ClearAndPushState(const StateLayerDesc& desc) noexcept {
    if (m_cmdCount >= kMaxPendingStateCmds) {
        return Err(FrameworkErrc::StateStackOverflow);
    }
    m_cmdQueue[m_cmdCount++] = PendingStateCmd{.desc = desc, .kind = StateCmdKind::ClearAndPush};
    return Ok;
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
        return Ok;
    }

    CookedSceneManifestHeader inlineHdr{
        .magic = kSceneManifestMagic,
        .version = kSceneManifestVersion,
        .assetCount = 0,
        .collisionMapId = desc.collisionMapId,
        .rootPrefabId = desc.rootPrefabId,
        .cueBankId = desc.cueBankId,
        .musicTrackId = desc.musicTrackId,
        .musicCrossfadeBlocks = desc.musicCrossfadeBlocks
    };
    return PinManifestPayload(inlineHdr, {}, assets);
}

Result<void, FrameworkErrc> SceneDirector::PinManifestPayload(
    const CookedSceneManifestHeader& hdr,
    std::span<const CookedSceneAssetEntry> entries,
    assets::AssetSystem& assets) noexcept {
    if (hdr.collisionMapId.value != 0) {
        auto h = assets.PinBlob(m_stagingScope, hdr.collisionMapId, assets::Residency::Sim);
        if (!h) {
            AbortStagingScope(assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
        m_stagedCollisionBlob = *h;
    }

    if (hdr.rootPrefabId.value != 0) {
        auto h = assets.PinBlob(m_stagingScope, hdr.rootPrefabId, assets::Residency::Sim);
        if (!h) {
            AbortStagingScope(assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
        m_stagedPrefabBlob = *h;
    }

    if (hdr.cueBankId.value != 0) {
        auto h = assets.PinBlob(m_stagingScope, hdr.cueBankId, assets::Residency::Sim);
        if (!h) {
            AbortStagingScope(assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
        m_stagedCueBankBlob = *h;
    }

    if (hdr.musicTrackId != 0 && m_pendingScene.musicTrackId == 0) {
        m_pendingScene.musicTrackId = hdr.musicTrackId;
        m_pendingScene.musicCrossfadeBlocks = hdr.musicCrossfadeBlocks;
    }

    for (const auto& entry : entries) {
        switch (entry.kind) {
            case assets::AssetKind::Texture:
                (void)assets.PinTexture(m_stagingScope, entry.id, entry.residency);
                break;
            case assets::AssetKind::Atlas:
                (void)assets.PinAtlas(m_stagingScope, entry.id, entry.residency);
                break;
            case assets::AssetKind::Blob:
                (void)assets.PinBlob(m_stagingScope, entry.id, entry.residency);
                break;
            case assets::AssetKind::Audio:
                (void)assets.PinAudio(m_stagingScope, entry.id, entry.residency);
                break;
            case assets::AssetKind::None:
                break;
        }
    }

    m_phase = (m_pendingScene.waitPolicy == SceneWaitPolicy::ImmediateBarrier)
                  ? TransitionPhase::ReadyToActivate
                  : TransitionPhase::StreamingSceneAssets;
    return Ok;
}

Result<void, FrameworkErrc> SceneDirector::AdvanceSceneStreaming(
    SceneLoadContext& loadCtx) noexcept {
    const assets::AssetBank& bank = loadCtx.assets.Bank();

    if (m_phase == TransitionPhase::WaitingManifestBlob) {
        if (m_pendingScene.waitPolicy == SceneWaitPolicy::ImmediateBarrier) {
            if (auto w = loadCtx.assets.WaitSimReady(m_stagingScope, loadCtx.renderer); !w) {
                AbortStagingScope(loadCtx.assets);
                return Err(FrameworkErrc::SimAssetWaitFailed);
            }
        } else if (!IsBlobReady(bank, m_stagedManifestBlob)) {
            return Ok;
        }

        const auto& blob = bank.GetBlob(m_stagedManifestBlob);
        if (blob.size < sizeof(CookedSceneManifestHeader)) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::CorruptedSceneManifest);
        }

        CookedSceneManifestHeader hdr{};
        std::memcpy(&hdr, blob.data, sizeof(CookedSceneManifestHeader));
        if (hdr.magic != kSceneManifestMagic || hdr.version != kSceneManifestVersion) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::CorruptedSceneManifest);
        }

        const usize expectedBytes = sizeof(CookedSceneManifestHeader) +
                                    static_cast<usize>(hdr.assetCount) * sizeof(CookedSceneAssetEntry);
        if (blob.size < expectedBytes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::CorruptedSceneManifest);
        }

        const auto* entryPtr = reinterpret_cast<const CookedSceneAssetEntry*>(
            blob.data + sizeof(CookedSceneManifestHeader));
        auto pinRes = PinManifestPayload(hdr, {entryPtr, hdr.assetCount}, loadCtx.assets);
        if (!pinRes) {
            return pinRes;
        }
    }

    if (m_phase == TransitionPhase::StreamingSceneAssets) {
        const bool allSimBlobsResident =
            IsBlobReady(bank, m_stagedCollisionBlob) &&
            IsBlobReady(bank, m_stagedPrefabBlob) &&
            IsBlobReady(bank, m_stagedCueBankBlob);
        if (allSimBlobsResident) {
            m_phase = TransitionPhase::ReadyToActivate;
        }
    }

    return Ok;
}

Result<bool, FrameworkErrc> SceneDirector::CommitSafeFramePoint(
    SceneLoadContext& loadCtx,
    platform::input::InputPipeline& input,
    time::TimeManager& time) noexcept {
    bool sceneActivated = false;

    if (m_phase != TransitionPhase::Idle) {
        if (auto streamRes = AdvanceSceneStreaming(loadCtx); !streamRes) {
            return Err(streamRes.error());
        }
        if (m_phase == TransitionPhase::ReadyToActivate) {
            if (auto actRes = ActivatePendingScene(loadCtx); !actRes) {
                return Err(actRes.error());
            }
            sceneActivated = true;
        }
    }

    if (m_cmdCount > 0) {
        loadCtx.sceneScope = m_activeScope;
        loadCtx.persistentScope = m_persistentScope;

        auto pushSlot = [&](const StateLayerDesc& d) noexcept {
            const u8 idx = m_stackDepth++;
            m_simFns[idx] = d.onSimTick;
            m_renderFns[idx] = d.onRenderExtract;
            m_userStates[idx] = d.stateData;
            m_enterFns[idx] = d.onEnter;
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
            m_simFns[idx] = nullptr;
            m_renderFns[idx] = nullptr;
            m_userStates[idx] = nullptr;
            m_enterFns[idx] = nullptr;
            m_exitFns[idx] = nullptr;
            m_layerFlags[idx] = StateLayerFlags::None;
            m_stateIds[idx] = kInvalidStateId;
            m_inputMasks[idx] = 0xFFFF;
        };

        for (u8 i = 0; i < m_cmdCount; ++i) {
            const auto& cmd = m_cmdQueue[i];
            switch (cmd.kind) {
                case StateCmdKind::Push:
                    if (m_stackDepth >= kMaxStateStackDepth) {
                        m_cmdCount = 0;
                        return Err(FrameworkErrc::StateStackOverflow);
                    }
                    pushSlot(cmd.desc);
                    break;

                case StateCmdKind::Pop:
                    if (m_stackDepth == 0) {
                        m_cmdCount = 0;
                        return Err(FrameworkErrc::StateStackUnderflow);
                    }
                    popSlot();
                    break;

                case StateCmdKind::Switch:
                    if (m_stackDepth > 0) {
                        popSlot();
                    }
                    pushSlot(cmd.desc);
                    break;

                case StateCmdKind::ClearAndPush:
                    while (m_stackDepth > 0) {
                        popSlot();
                    }
                    pushSlot(cmd.desc);
                    break;
            }
        }
        m_cmdCount = 0;
        RebakeStackMasks(input, time);
    }

    return sceneActivated;
}

void SceneDirector::RebakeStackMasks(
    platform::input::InputPipeline& input,
    time::TimeManager& time) noexcept {
    if (m_stackDepth == 0) {
        m_activeSimMask = 0;
        m_activeRenderMask = 0;
        m_pausedClockMask = 0;
        m_topInputMask = 0xFFFF;
        input.SetActionMask(0xFFFF);
        time.SetPaused(false);
        return;
    }

    u8 simBlockBits = 0;
    u8 renderBlockBits = 0;
    u8 pauseBits = 0;
    u8 hasSimFnBits = 0;
    u8 hasRenderFnBits = 0;

    for (u8 i = 0; i < m_stackDepth; ++i) {
        const u8 bit = static_cast<u8>(1u << i);
        const StateLayerFlags f = m_layerFlags[i];
        if (HasStateFlag(f, StateLayerFlags::BlockLowerSimTicks)) {
            simBlockBits |= bit;
        }
        if (HasStateFlag(f, StateLayerFlags::BlockLowerRender)) {
            renderBlockBits |= bit;
        }
        if (HasStateFlag(f, StateLayerFlags::PauseSimClock)) {
            pauseBits |= bit;
        }
        if (m_simFns[i] != nullptr) {
            hasSimFnBits |= bit;
        }
        if (m_renderFns[i] != nullptr) {
            hasRenderFnBits |= bit;
        }
    }

    // Highest bit in simBlockBits determines the lowest layer allowed to tick:
    // If layer k has BlockLowerSimTicks set, bits [0..k-1] are masked out in O(1).
    const u8 validLayerMask = static_cast<u8>((1u << m_stackDepth) - 1u);
    const u8 minSimLayer = (simBlockBits == 0) ? 0u : static_cast<u8>(std::bit_width(simBlockBits) - 1);
    const u8 minRenderLayer = (renderBlockBits == 0) ? 0u : static_cast<u8>(std::bit_width(renderBlockBits) - 1);

    const u8 unblockedSimRange = static_cast<u8>(validLayerMask & ~((1u << minSimLayer) - 1u));
    const u8 unblockedRenderRange = static_cast<u8>(validLayerMask & ~((1u << minRenderLayer) - 1u));

    m_activeSimMask = static_cast<u8>(unblockedSimRange & hasSimFnBits);
    m_activeRenderMask = static_cast<u8>(unblockedRenderRange & hasRenderFnBits);
    m_pausedClockMask = pauseBits;

    // Combine inputActionMasks from the top layer down to the first layer with BlockLowerInput.
    u16 combinedInputMask = 0;
    for (u8 i = m_stackDepth; i > 0; --i) {
        const u8 slot = static_cast<u8>(i - 1);
        combinedInputMask |= m_inputMasks[slot];
        if (HasStateFlag(m_layerFlags[slot], StateLayerFlags::BlockLowerInput)) {
            break;
        }
    }
    m_topInputMask = combinedInputMask;

    input.SetActionMask(m_topInputMask);
    time.SetPaused(m_pausedClockMask != 0);
}

Result<void, FrameworkErrc> SceneDirector::ActivatePendingScene(
    SceneLoadContext& loadCtx) noexcept {
    // Enforce deterministic barrier for any remaining Residency::Sim assets in the staging scope.
    if (auto waitRes = loadCtx.assets.WaitSimReady(m_stagingScope, loadCtx.renderer); !waitRes) {
        AbortStagingScope(loadCtx.assets);
        return Err(FrameworkErrc::SimAssetWaitFailed);
    }

    if (m_activeScope != assets::kInvalidScope && m_pendingScene.onDeactivating) {
        loadCtx.sceneScope = m_activeScope;
        m_pendingScene.onDeactivating(loadCtx, m_activeSceneId);
    }

    if (m_pendingScene.mode == SceneTransitionMode::FullReset) {
        loadCtx.world.Clear();
    }

    const assets::AssetBank& bank = loadCtx.assets.Bank();

    if (m_stagedCueBankBlob.index != 0) {
        const auto& cueBlob = bank.GetBlob(m_stagedCueBankBlob);
        if (auto cueRes = loadCtx.audio.RegisterCueBank(cueBlob, m_stagingScope); !cueRes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    if (m_stagedCollisionBlob.index != 0) {
        if (auto colRes = loadCtx.gameplay.SetActiveRoomCollision(bank, m_stagedCollisionBlob); !colRes) {
            AbortStagingScope(loadCtx.assets);
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    if (m_stagedPrefabBlob.index != 0) {
        if (auto pfRes = world::WorldExtractor::InstantiatePrefab(loadCtx.world, bank, m_stagedPrefabBlob); !pfRes) {
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
    m_phase = TransitionPhase::Idle;

    if (oldScope != assets::kInvalidScope) {
        loadCtx.audio.RequestScopeRelease(oldScope, loadCtx.assets);
    }

    if (m_pendingScene.onActivated) {
        loadCtx.sceneScope = m_activeScope;
        if (!m_pendingScene.onActivated(loadCtx, m_activeSceneId)) {
            return Err(FrameworkErrc::SimAssetWaitFailed);
        }
    }

    return Ok;
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

u64 SceneDirector::ComputeAuditHash() const noexcept {
    u64 h = SplitMix64(m_activeSceneId.value);
    h ^= SplitMix64(
        (static_cast<u64>(m_stackDepth) << 32) |
        (static_cast<u64>(m_activeSimMask) << 24) |
        (static_cast<u64>(m_activeRenderMask) << 16) |
        static_cast<u64>(m_topInputMask));
    for (u8 i = 0; i < m_stackDepth; ++i) {
        h ^= SplitMix64(
            (static_cast<u64>(m_stateIds[i]) << 16) |
            static_cast<u64>(m_layerFlags[i]));
    }
    return h;
}

constexpr u64 SceneDirector::CombineAuditHashes(
    u64 tickIndex,
    u64 worldHash,
    u64 gameplayHash,
    u64 assetHash,
    u64 audioHash,
    u64 renderHash,
    u64 directorHash) noexcept {
    auto fold = [](u64 seed, u64 val) constexpr noexcept -> u64 {
        return seed ^ ( SplitMix64(val) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2) );
    };

    u64 seed = 0x517cc1b727220a95ULL;
    seed = fold(seed, tickIndex);
    seed = fold(seed, worldHash);
    seed = fold(seed, gameplayHash);
    seed = fold(seed, assetHash);
    seed = fold(seed, audioHash);
    seed = fold(seed, renderHash);
    seed = fold(seed, directorHash);
    return seed;
}

} // namespace en::framework
#include "framework/SceneDirector.h"
#include "core/Serialization.h"

namespace en {

Result<usize, FrameworkErrc> SaveGameManager::SerializeSaveSlot(
    const SaveSlotHeader& meta, const GameplayPipeline& gameplay, std::span<u8> outBuffer) noexcept {
    if (outBuffer.size() < sizeof(SaveSlotHeader) + 512U) return Err(FrameworkErrc::InvalidConfig);
    const auto flagsSpan = gameplay.RawWorldFlags();
    const std::span<const u8> flagBytes(reinterpret_cast<const u8*>(flagsSpan.data()), 512U);

    SaveSlotHeader hdr = meta;
    hdr.magic = kSaveSlotMagic;
    hdr.version = 1;
    hdr.payloadCrc32 = ComputeCrc32(flagBytes);

    BinaryWriter w(outBuffer);
    if (!w.Write(hdr) || !w.WriteBytes(flagBytes)) return Err(FrameworkErrc::InvalidConfig);
    return w.Offset();
}

Result<SaveSlotHeader, FrameworkErrc> SaveGameManager::DeserializeSaveSlot(
    std::span<const u8> inBuffer, GameplayPipeline& outGameplay) noexcept {
    if (inBuffer.size() < sizeof(SaveSlotHeader) + 512U) return Err(FrameworkErrc::SaveGameCorrupted);
    BinaryReader r(inBuffer);
    SaveSlotHeader hdr{};
    if (!r.Read(hdr) || hdr.magic != kSaveSlotMagic) return Err(FrameworkErrc::SaveGameCorrupted);

    std::array<u64, 64> words{};
    if (!r.ReadBytes(std::span<u8>(reinterpret_cast<u8*>(words.data()), 512U))) {
        return Err(FrameworkErrc::SaveGameCorrupted);
    }
    const u32 crc = ComputeCrc32(std::span<const u8>(reinterpret_cast<const u8*>(words.data()), 512U));
    if (crc != hdr.payloadCrc32) return Err(FrameworkErrc::SaveGameCorrupted);

    outGameplay.LoadRawWorldFlags(words);
    return hdr;
}

Result<void, FrameworkErrc> SceneDirector::Init(AssetSystem& assets) noexcept {
    auto scopeRes = assets.BeginScope();
    if (!scopeRes) return Err(FrameworkErrc::AssetInitFailed);
    m_persistentScope = *scopeRes;
    m_activeScope = kInvalidScope;
    m_stagingScope = kInvalidScope;
    m_stackDepth = 0;
    m_pendingCmdCount = 0;
    m_hasPendingScene = false;
    m_activeSceneDeactivateFn = nullptr;
    RebakeStackMasks();
    return {};
}

void SceneDirector::Shutdown(
    AssetSystem& assets, AudioEngine& audio, SceneLoadContext* optionalLoadCtx) noexcept {
    if (optionalLoadCtx) {
        while (m_stackDepth > 0) {
            const u8 top = --m_stackDepth;
            if (m_exitFns[top]) m_exitFns[top](*optionalLoadCtx, m_userStates[top]);
        }
        if (m_activeSceneDeactivateFn) {
            m_activeSceneDeactivateFn(*optionalLoadCtx);
            m_activeSceneDeactivateFn = nullptr;
        }
    }
    m_stackDepth = 0;
    m_pendingCmdCount = 0;
    m_hasPendingScene = false;
    if (m_activeScope != kInvalidScope) {
        audio.RequestScopeRelease(m_activeScope);
        assets.EndScope(m_activeScope);
        m_activeScope = kInvalidScope;
    }
    if (m_persistentScope != kInvalidScope) {
        assets.EndScope(m_persistentScope);
        m_persistentScope = kInvalidScope;
    }
    RebakeStackMasks();
}

Result<void, FrameworkErrc> SceneDirector::PushState(const StateLayerDesc& desc) noexcept {
    if (m_pendingCmdCount >= m_pendingCmds.size()) return Err(FrameworkErrc::StateStackOverflow);
    m_pendingCmds[m_pendingCmdCount++] = PendingCmd{desc, CmdKind::Push};
    return {};
}

Result<void, FrameworkErrc> SceneDirector::PopState() noexcept {
    if (m_pendingCmdCount >= m_pendingCmds.size()) return Err(FrameworkErrc::StateStackOverflow);
    m_pendingCmds[m_pendingCmdCount++] = PendingCmd{{}, CmdKind::Pop};
    return {};
}

Result<void, FrameworkErrc> SceneDirector::SwitchState(const StateLayerDesc& desc) noexcept {
    if (m_pendingCmdCount >= m_pendingCmds.size()) return Err(FrameworkErrc::StateStackOverflow);
    m_pendingCmds[m_pendingCmdCount++] = PendingCmd{desc, CmdKind::Switch};
    return {};
}

Result<void, FrameworkErrc> SceneDirector::ClearAndPushState(const StateLayerDesc& desc) noexcept {
    if (m_pendingCmdCount >= m_pendingCmds.size()) return Err(FrameworkErrc::StateStackOverflow);
    m_pendingCmds[m_pendingCmdCount++] = PendingCmd{desc, CmdKind::ClearAndPush};
    return {};
}

void SceneDirector::RebakeStackMasks() noexcept {
    u8 simMask = 0;
    u8 renderMask = 0;
    u8 pauseMask = 0;
    u16 topInput = 0xFFFFU;
    bool simBlocked = false;
    bool renderBlocked = false;
    bool inputBlocked = false;

    for (u8 i = m_stackDepth; i > 0; --i) {
        const u8 idx = static_cast<u8>(i - 1U);
        const u8 bit = static_cast<u8>(1U << idx);
        const StateLayerFlags fl = m_layerFlags[idx];
        if (!simBlocked) simMask |= bit;
        if (!renderBlocked) renderMask |= bit;
        if (HasFlag(fl, StateLayerFlags::PauseSimClock)) pauseMask |= bit;
        if (!inputBlocked) {
            topInput = m_inputMasks[idx];
            if (HasFlag(fl, StateLayerFlags::BlockLowerInput)) inputBlocked = true;
        }
        if (HasFlag(fl, StateLayerFlags::BlockLowerSimTicks)) simBlocked = true;
        if (HasFlag(fl, StateLayerFlags::BlockLowerRender)) renderBlocked = true;
    }

    m_activeSimMask = simMask;
    m_activeRenderMask = renderMask;
    m_pausedClockMask = pauseMask;
    m_topInputMask = topInput;
}

Result<void, FrameworkErrc> SceneDirector::RequestSceneTransition(
    const SceneTransitionDesc& desc, AssetSystem& assets) noexcept {
    if (m_hasPendingScene) return Err(FrameworkErrc::SceneTransitionBusy);
    auto scopeRes = assets.BeginScope();
    if (!scopeRes) return Err(FrameworkErrc::AssetInitFailed);
    m_stagingScope = *scopeRes;
    m_pendingScene = desc;
    m_hasPendingScene = true;

    if (desc.manifestBlobId.IsValid()) {
        (void)assets.PinBlob(m_stagingScope, desc.manifestBlobId, AssetResidency::Sim);
    }
    if (desc.collisionMapId.IsValid()) {
        (void)assets.PinBlob(m_stagingScope, desc.collisionMapId, AssetResidency::Sim);
    }
    if (desc.rootPrefabId.IsValid()) {
        (void)assets.PinBlob(m_stagingScope, desc.rootPrefabId, AssetResidency::Sim);
    }
    if (desc.cueBankId.IsValid()) {
        (void)assets.PinBlob(m_stagingScope, desc.cueBankId, AssetResidency::Sim);
    }
    return {};
}

Result<bool, FrameworkErrc> SceneDirector::CommitSafeFramePoint(SceneLoadContext& loadCtx) noexcept {
    bool changed = false;

    if (m_hasPendingScene && loadCtx.assets.Bank().IsScopeSimReady(m_stagingScope)) {
        const SceneTransitionDesc scene = m_pendingScene;
        m_hasPendingScene = false;

        AssetId colId = scene.collisionMapId;
        AssetId pfbId = scene.rootPrefabId;
        AssetId cueId = scene.cueBankId;

        if (scene.manifestBlobId.IsValid()) {
            const BlobHandle mh = loadCtx.assets.PinBlob(m_stagingScope, scene.manifestBlobId, AssetResidency::Sim);
            const BlobRecord& mrec = loadCtx.assets.Bank().GetBlob(mh);
            if (!mrec.IsValid() || mrec.size < sizeof(CookedSceneManifestHeader)) {
                return Err(FrameworkErrc::CorruptedSceneManifest);
            }
            BinaryReader r(mrec.Bytes());
            CookedSceneManifestHeader hdr{};
            (void)r.Read(hdr);
            if (hdr.magic != kCookedSceneManifestMagic) return Err(FrameworkErrc::CorruptedSceneManifest);
            if (hdr.collisionMapId != 0) colId = AssetId{hdr.collisionMapId};
            if (hdr.rootPrefabId != 0) pfbId = AssetId{hdr.rootPrefabId};
            if (hdr.cueBankId != 0) cueId = AssetId{hdr.cueBankId};
            for (u16 i = 0; i < hdr.assetCount; ++i) {
                CookedSceneAssetEntry e{};
                if (r.Read(e)) {
                    (void)loadCtx.assets.Pin(m_stagingScope, AssetId{e.assetId}, e.kind, e.residency);
                }
            }
        }

        if (m_activeSceneDeactivateFn) {
            m_activeSceneDeactivateFn(loadCtx);
        }
        if (scene.mode == SceneTransitionMode::FullReset) {
            loadCtx.world.Clear();
            loadCtx.gameplay.Particles().Clear();
        }
        if (m_activeScope != kInvalidScope) {
            loadCtx.audio.RequestScopeRelease(m_activeScope);
        }

        m_activeScope = m_stagingScope;
        m_stagingScope = kInvalidScope;
        m_activeSceneId = scene.sceneId;
        m_activeSceneDeactivateFn = scene.onDeactivating;
        loadCtx.sceneScope = m_activeScope;

        if (colId.IsValid()) {
            const BlobHandle ch = loadCtx.assets.PinBlob(m_activeScope, colId, AssetResidency::Sim);
            (void)loadCtx.gameplay.SetActiveRoomCollision(loadCtx.assets.Bank(), ch);
        }
        if (cueId.IsValid()) {
            const BlobHandle ah = loadCtx.assets.PinBlob(m_activeScope, cueId, AssetResidency::Sim);
            (void)loadCtx.audio.RegisterCueBank(loadCtx.assets.Bank().GetBlob(ah));
        }
        if (pfbId.IsValid()) {
            const BlobHandle ph = loadCtx.assets.PinBlob(m_activeScope, pfbId, AssetResidency::Sim);
            (void)loadCtx.world.InstantiatePrefab(loadCtx.assets.Bank(), ph);
        }
        if (scene.onActivated) {
            scene.onActivated(loadCtx);
        }
        changed = true;
    }

    while (m_pendingCmdCount > 0) {
        const u8 count = m_pendingCmdCount;
        std::array<PendingCmd, 8> localCmds = m_pendingCmds;
        m_pendingCmdCount = 0;

        for (u8 c = 0; c < count; ++c) {
            const PendingCmd& cmd = localCmds[c];
            auto popTop = [&]() noexcept {
                if (m_stackDepth > 0) {
                    const u8 top = --m_stackDepth;
                    if (m_exitFns[top]) m_exitFns[top](loadCtx, m_userStates[top]);
                }
            };
            auto pushLayer = [&](const StateLayerDesc& d) noexcept {
                if (m_stackDepth >= kMaxStateStackDepth) return;
                const u8 idx = m_stackDepth++;
                m_layerFlags[idx] = d.flags;
                m_stateIds[idx] = d.id;
                m_inputMasks[idx] = d.inputActionMask;
                m_simFns[idx] = d.onSimTick;
                m_renderFns[idx] = d.onRenderExtract;
                m_exitFns[idx] = d.onExit;
                m_userStates[idx] = d.stateData;
                RebakeStackMasks();
                if (d.onEnter) d.onEnter(loadCtx, d.stateData);
            };

            switch (cmd.kind) {
                case CmdKind::Push:
                    pushLayer(cmd.desc);
                    break;
                case CmdKind::Pop:
                    popTop();
                    RebakeStackMasks();
                    break;
                case CmdKind::Switch:
                    popTop();
                    pushLayer(cmd.desc);
                    break;
                case CmdKind::ClearAndPush:
                    while (m_stackDepth > 0) popTop();
                    pushLayer(cmd.desc);
                    break;
            }
            changed = true;
        }
    }
    return changed;
}

void SceneDirector::DispatchSimTick(SimContext& simCtx) const noexcept {
    for (u8 i = 0; i < m_stackDepth; ++i) {
        if (((m_activeSimMask & (1U << i)) != 0) && m_simFns[i]) {
            simCtx.userState = m_userStates[i];
            m_simFns[i](simCtx);
        }
    }
}

void SceneDirector::DispatchRenderExtract(RenderContext& renderCtx) const noexcept {
    for (u8 i = 0; i < m_stackDepth; ++i) {
        if (((m_activeRenderMask & (1U << i)) != 0) && m_renderFns[i]) {
            renderCtx.userState = m_userStates[i];
            m_renderFns[i](renderCtx);
        }
    }
}

u64 SceneDirector::ComputeAuditHash() const noexcept {
    u64 h = Mix64(static_cast<u64>(m_stackDepth) |
                  (static_cast<u64>(m_activeSimMask) << 8U) |
                  (static_cast<u64>(m_activeRenderMask) << 16U));
    h = HashCombine64(h, m_activeSceneId.value);
    for (u8 i = 0; i < m_stackDepth; ++i) {
        h = HashCombine64(h, m_stateIds[i]);
    }
    return h;
}

} // namespace en

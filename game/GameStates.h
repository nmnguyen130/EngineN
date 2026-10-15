#pragma once

#include "game/GameTypes.h"

namespace game {

// StateLayerDesc factories for SceneDirector::PushState / SwitchState / ClearAndPushState
[[nodiscard]] framework::StateLayerDesc MakeTitleStateDesc(GameSessionState* session) noexcept;
[[nodiscard]] framework::StateLayerDesc MakeGameplayStateDesc(GameSessionState* session) noexcept;
[[nodiscard]] framework::StateLayerDesc MakePauseModalStateDesc(GameSessionState* session) noexcept;
[[nodiscard]] framework::StateLayerDesc MakeGameOverStateDesc(GameSessionState* session) noexcept;

// Room lifecycle callbacks wired into SceneTransitionDesc
void OnRoomDeactivating(framework::SceneLoadContext& ctx, assets::AssetId sceneId) noexcept;
bool OnRoomActivated(framework::SceneLoadContext& ctx, assets::AssetId sceneId) noexcept;

// Helper to issue a RoomSwap or FullReset transition to a target room
void RequestRoomLoad(
    GameSessionState& session,
    assets::AssetSystem& assets,
    assets::AssetId targetRoomId,
    f32 spawnX,
    f32 spawnY,
    framework::SceneTransitionMode mode,
    framework::SceneWaitPolicy waitPolicy = framework::SceneWaitPolicy::StreamNonBlocking) noexcept;

// Registers custom game CVars and console commands (god, noclip, warp, give_all, flag)
void RegisterClientDebugTools(GameSessionState& session, debug::DebugSystem& debugSys) noexcept;

} // namespace game
#pragma once

#include "framework/SceneDirector.h"
#include "game/assets/GameAssetPipeline.h"
#include "game/world/MetroidvaniaSystem.h"

namespace vots {

class VeilGameApp;

enum class GameStateLayerId : u8 {
    TitleScreen    = 1,
    ActiveGameplay = 2,
    ResonatorModal = 3,
    MapModal       = 4,
    PauseModal     = 5,
    EndingModal    = 6,
};

// Manages the 4 interconnected Vaelen rooms, tilemaps, interactive props, CameraController2D confiners, and StateLayerDesc stack.
class GameScenes {
public:
    [[nodiscard]] static en::StateLayerDesc MakeGameplayLayerDesc(VeilGameApp* app) noexcept;
    [[nodiscard]] static en::StateLayerDesc MakePauseModalLayerDesc(VeilGameApp* app) noexcept;
    [[nodiscard]] static en::StateLayerDesc MakeResonatorModalLayerDesc(VeilGameApp* app) noexcept;
    [[nodiscard]] static en::StateLayerDesc MakeMapModalLayerDesc(VeilGameApp* app) noexcept;
    [[nodiscard]] static en::StateLayerDesc MakeEndingModalLayerDesc(VeilGameApp* app) noexcept;

    [[nodiscard]] static en::Result<void, en::FrameworkErrc> RequestRoomTransition(
        VeilGameApp& app,
        u8 targetRoomId,
        en::SceneTransitionMode mode = en::SceneTransitionMode::RoomSwap) noexcept;

    [[nodiscard]] static en::Result<void, en::FrameworkErrc> PopulateRoom(
        VeilGameApp& app,
        en::SceneLoadContext& ctx,
        u8 roomId) noexcept;
};

} // namespace vots

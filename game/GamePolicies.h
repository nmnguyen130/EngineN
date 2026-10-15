#pragma once

#include "game/GameTypes.h"

namespace game {

// Component pool registration callback for AppHooks::OnRegisterComponents
[[nodiscard]] Result<void, u8> RegisterClientComponents(world::World& world, void* appState = nullptr) noexcept;

// Configures default Keyboard + Gamepad bindings on InputPipeline
void ConfigureDefaultInputBindings(platform::input::InputPipeline& input) noexcept;

// Builds the zero-virtual GameplayPolicyTable for GameplayPipeline
[[nodiscard]] gameplay::GameplayPolicyTable MakeGameplayPolicyTable(GameSessionState* session) noexcept;

// Custom SimCue handler for AppHooks::OnConsumeSimCue (spawns hit sparks, camera trauma, haptics)
void ConsumeClientSimCue(
    const gameplay::SimCue& cue,
    world::World& world,
    const assets::AssetBank& assetBank,
    void* userAppState) noexcept;

// Core simulation steppers invoked by GameplayState::OnSimTick
void StepPlayerController(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    audio::AudioSystem& audio,
    const assets::AssetBank& assetBank,
    time::SimStepView step,
    const platform::input::TickInput& tickInput,
    platform::input::ActionBuffer& actionBuffer) noexcept;

void StepEnemiesAndCarriers(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    audio::AudioSystem& audio,
    const assets::AssetBank& assetBank,
    time::SimStepView step) noexcept;

void StepTriggersAndVfx(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    assets::AssetSystem& assets,
    audio::AudioSystem& audio,
    time::SimStepView step,
    const platform::input::TickInput& tickInput) noexcept;

// Procedural fallback / archetype spawners (ensures persistent Player + Camera + Room populations exist)
[[nodiscard]] world::EntityId EnsurePersistentPlayerAndCamera(
    GameSessionState& session,
    world::World& world,
    const assets::AssetBank& assetBank) noexcept;

void PopulateRoomEntities(
    GameSessionState& session,
    world::World& world,
    gameplay::GameplayPipeline& gameplay,
    const assets::AssetBank& assetBank,
    assets::AssetId roomId) noexcept;

} // namespace game
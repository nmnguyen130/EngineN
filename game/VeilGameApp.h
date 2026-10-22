#pragma once

#include "framework/Application.h"
#include "game/assets/GameAssetPipeline.h"
#include "game/debug/GameDebugTools.h"
#include "game/world/MetroidvaniaSystem.h"

namespace vots {

// Top-level client game coordinator owning Veil of the Silent state and wiring AppHooks into en::Application.
// Non-copyable owner of en::Application and MetroidvaniaSystem.
// Invariant: all custom ECS pools (kTypeId 24..31) are registered before Physics/Gameplay initialization.
class VeilGameApp {
public:
    [[nodiscard]] en::Result<void, en::FrameworkError> Init(
        bool headless = true,
        en::ViewportExtent viewport = {1280, 720}) noexcept;

    void Shutdown() noexcept;

    [[nodiscard]] en::Result<en::FrameAuditDigest, en::FrameworkError> StepHeadlessTicks(
        u32 tickCount = 1U,
        u32 audioBlocksPerTick = 2U) noexcept {
        return m_app.StepHeadless(tickCount, audioBlocksPerTick);
    }

    [[nodiscard]] en::Result<void, en::FrameworkError> StepFrame(
        u64 rawDeltaNs = en::kFixedDtNs) noexcept {
        return m_app.StepFrame(rawDeltaNs);
    }

    void OnSimTick(en::SimContext& ctx, const en::SimStepView& step) noexcept;
    void OnRenderExtract(en::RenderContext& ctx) noexcept;

    [[nodiscard]] en::Application& EngineApp() noexcept { return m_app; }
    [[nodiscard]] const en::Application& EngineApp() const noexcept { return m_app; }

    [[nodiscard]] MetroidvaniaSystem& Metroidvania() noexcept { return m_metroidvania; }
    [[nodiscard]] const MetroidvaniaSystem& Metroidvania() const noexcept { return m_metroidvania; }

    [[nodiscard]] const GameAssetHandles& AssetHandles() const noexcept { return m_assets; }
    [[nodiscard]] GameDebugCVars& CVars() noexcept { return m_cvars; }
    [[nodiscard]] const GameDebugCVars& CVars() const noexcept { return m_cvars; }

    [[nodiscard]] constexpr EntityId PlayerEntity() const noexcept { return m_playerEntity; }
    constexpr void SetPlayerEntity(EntityId id) noexcept { m_playerEntity = id; }

    [[nodiscard]] constexpr EntityId CameraEntity() const noexcept { return m_cameraEntity; }
    constexpr void SetCameraEntity(EntityId id) noexcept { m_cameraEntity = id; }

    [[nodiscard]] constexpr EntityId BossEntity() const noexcept { return m_bossEntity; }
    constexpr void SetBossEntity(EntityId id) noexcept { m_bossEntity = id; }

    [[nodiscard]] constexpr u8 ActiveRoomId() const noexcept { return m_activeRoomId; }
    constexpr void SetActiveRoomId(u8 roomId) noexcept { m_activeRoomId = roomId; }

    [[nodiscard]] constexpr u8 PendingRoomId() const noexcept { return m_pendingRoomId; }
    constexpr void SetPendingRoomId(u8 roomId) noexcept { m_pendingRoomId = roomId; }

    [[nodiscard]] constexpr f32 TransitionSpawnX() const noexcept { return m_transitionSpawnX; }
    constexpr void SetTransitionSpawnX(f32 x) noexcept { m_transitionSpawnX = x; }

    constexpr void SetPortalCooldownTicks(u8 ticks) noexcept { m_portalCooldownTicks = ticks; }

    [[nodiscard]] u32& NextAttackInstanceId() noexcept { return m_nextAttackInstanceId; }

private:
    static en::Result<void, en::FrameworkErrc> OnRegisterComponentsHook(
        en::World& world,
        void* userAppState) noexcept;

    static en::Result<void, en::FrameworkErrc> OnBootHook(
        en::SceneLoadContext& ctx) noexcept;

    static void OnPreShutdownHook(
        en::SceneLoadContext& ctx) noexcept;

    static void OnConsumeSimCueHook(
        const en::SimCue& cue,
        en::SimContext& ctx,
        void* userAppState) noexcept;

    en::Application m_app{};
    MetroidvaniaSystem m_metroidvania{};
    GameAssetHandles m_assets{};
    GameDebugCVars m_cvars{};
    EntityId m_playerEntity{kNullEntity};
    EntityId m_cameraEntity{kNullEntity};
    EntityId m_bossEntity{kNullEntity};
    f32 m_transitionSpawnX{-4.0f};
    u32 m_nextAttackInstanceId{1U};
    u32 m_cuesConsumedCount{0U};
    u8 m_activeRoomId{0U};
    u8 m_pendingRoomId{0U};
    u8 m_portalCooldownTicks{0U};
};

} // namespace vots

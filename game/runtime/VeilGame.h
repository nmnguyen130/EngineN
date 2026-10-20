#pragma once

#include "game/systems/VeilCombatAndAiSystem.h"
#include "game/core/VeilAssets.h"
#include "game/systems/VeilPlayerSystem.h"

namespace en::veil {

// Client game coordinator wiring AppHooks, StateLayerDesc stack, and automated verification suite.
// Owns VeilSessionState and the procedural boot .enpak buffer.
// Invariant: RunVerificationSuite() exercises all 10 engine subsystems deterministically.
class VeilGame final {
public:
    [[nodiscard]] AppHooks BuildAppHooks() noexcept;
    [[nodiscard]] static bool RunVerificationSuite() noexcept;

private:
    static Result<void, WorldErrc> OnRegisterComponents(World& world) noexcept;
    static Result<void, FrameworkErrc> OnBoot(SceneLoadContext& ctx) noexcept;
    static void OnCourtyardActivated(SceneLoadContext& ctx) noexcept;
    static void OnSanctumActivated(SceneLoadContext& ctx) noexcept;

    static void StepPolicies(
        World& world,
        const AssetBank& assets,
        const SimStepView& step,
        const TickInput& tickInput,
        ActionBuffer& actionBuffer,
        GameplayPipeline& pipeline) noexcept;

    static void ResolveCombatHitsBridge(
        World& world,
        const SimStepView& step,
        std::span<const ResolvedHitEvent> hits,
        GameplayPipeline& pipeline) noexcept;

    static void TitleSimTick(SimContext& ctx) noexcept;
    static void TitleRenderExtract(RenderContext& ctx) noexcept;
    static void GameplaySimTick(SimContext& ctx) noexcept;
    static void GameplayRenderExtract(RenderContext& ctx) noexcept;
    static void PauseSimTick(SimContext& ctx) noexcept;
    static void PauseRenderExtract(RenderContext& ctx) noexcept;

    VeilSessionState m_session{};
    std::array<u8, 128 * 1024> m_bootPakBuffer{};
    static VeilSessionState* s_activeSession;
};

} // namespace en::veil

#pragma once

#include "framework/Application.h"
#include "game/core/Components.h"
#include "game/app/GameplayPolicies.h"
#include "game/app/VeilStateLayers.h"
#include "game/movement/TraversalSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/ai/BossSystem.h"
#include "game/world/WorldSystem.h"

#include <memory>

namespace en::veil {

// Top-level EngineN client game application driver.
// Manages Application lifecycle, AppHooks, custom components (IDs 24..35), and 120Hz deterministic stepping.
// Invariant: Strictly zero heap allocations post-Init().
class GameApp {
public:
    GameApp() noexcept = default;
    ~GameApp() noexcept;

    GameApp(const GameApp&) = delete;
    GameApp& operator=(const GameApp&) = delete;

    [[nodiscard]] Result<void, framework::FrameworkError> Initialize(bool headless = true) noexcept;
    void Shutdown() noexcept;

    // Deterministically steps a single 120Hz simulation tick with given input intent.
    // Zero heap allocations; updates ECS pools and returns combat summary.
    CombatTickSummary StepSimulationTick(
        f32 moveX = 0.0f,
        bool jumpPressed = false,
        bool evadePressed = false,
        bool primaryAttackPressed = false,
        bool weaveShiftPressed = false,
        bool wingLeftPressed = false,
        bool wingRightPressed = false,
        bool parryPressed = false,
        bool dualUnisonPressed = false,
        bool triadUnisonPressed = false) noexcept;

    [[nodiscard]] framework::Application& App() noexcept { return *m_app; }
    [[nodiscard]] const framework::Application& App() const noexcept { return *m_app; }

    [[nodiscard]] VeilAppState& State() noexcept { return m_state; }
    [[nodiscard]] const VeilAppState& State() const noexcept { return m_state; }

    [[nodiscard]] ResonanceLanternComponent* PlayerLantern() noexcept;
    [[nodiscard]] AmplitudeAscensionComponent* PlayerAmplitude() noexcept;
    [[nodiscard]] TraversalMutationComponent* PlayerTraversal() noexcept;
    [[nodiscard]] VeilCombatStatsComponent* PlayerCombatStats() noexcept;
    [[nodiscard]] VeilActionStateComponent* PlayerActionState() noexcept;
    [[nodiscard]] PhaseInversionCounterComponent* PlayerParry() noexcept;

    [[nodiscard]] VeilCombatStatsComponent* BossCombatStats() noexcept;
    [[nodiscard]] StandingWaveComponent* BossWave() noexcept;
    [[nodiscard]] BossConductorComponent* BossConductor() noexcept;
    [[nodiscard]] WorldProgressComponent* PlayerProgress() noexcept;

    // Runs full regression test suite verifying GDD mechanics on native EngineN ECS.
    [[nodiscard]] static bool RunTests() noexcept;

private:
    std::unique_ptr<framework::Application> m_app{nullptr};
    VeilAppState m_state{};
    bool m_initialized{false};
};

} // namespace en::veil

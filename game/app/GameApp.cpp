#include "game/app/GameApp.h"
#include "game/tests/VeilTests.h"
#include "framework/SceneDirector.h"

#include <algorithm>

namespace en::veil {

GameApp::~GameApp() noexcept {
    Shutdown();
}

Result<void, framework::FrameworkError> GameApp::Initialize(bool headless) noexcept {
    if (m_initialized) {
        Shutdown();
    }

    framework::ApplicationConfig cfg{
        .appName = "Veil of the Silent",
        .orgName = "Apostate Studios",
        .bootPakPath = "",
        .headless = headless
    };

    framework::AppHooks hooks{
        .userAppState = &m_state,
        .gameplayPolicies = GameplayPolicies::CreatePolicyTable(),
        .OnRegisterComponents = [](world::World& world, void*) noexcept -> Result<void, u8> {
            return RegisterGameComponents(world);
        },
        .OnBoot = [](framework::SceneLoadContext& ctx, framework::SceneDirector& director) noexcept -> Result<void, u8> {
            auto* state = static_cast<VeilAppState*>(ctx.userAppState);
            auto desc = VeilStateLayers::CreateGameplayLayerDesc(state);
            auto r = director.PushState(desc);
            if (!r) {
                return Err(u8{1});
            }
            return {};
        },
        .OnPreShutdown = nullptr,
        .OnConsumeSimCue = [](const gameplay::SimCue&, world::World&, const assets::AssetBank&, void*) noexcept {}
    };

    m_app = std::make_unique<framework::Application>();
    auto initRes = m_app->Init(cfg, hooks);
    if (!initRes && initRes.error().code == framework::FrameworkErrc::AudioInitFailed) {
        cfg.audio.headlessMode = true;
        initRes = m_app->Init(cfg, hooks);
    }
    if (!initRes) {
        return initRes;
    }

    m_app->Input().Map()
        .Bind(static_cast<u8>(GameAction::MoveLeft),       {input::Source::Key(4),  input::Source::Pad(13)})
        .Bind(static_cast<u8>(GameAction::MoveRight),      {input::Source::Key(7),  input::Source::Pad(14)})
        .Bind(static_cast<u8>(GameAction::LookUp),         {input::Source::Key(26), input::Source::Pad(11)})
        .Bind(static_cast<u8>(GameAction::CrouchDown),     {input::Source::Key(22), input::Source::Pad(12)})
        .Bind(static_cast<u8>(GameAction::Jump),           {input::Source::Key(44), input::Source::Pad(0)})
        .Bind(static_cast<u8>(GameAction::PrimaryAttack),  {input::Source::Key(13), input::Source::Pad(2)})
        .Bind(static_cast<u8>(GameAction::EvadeDash),      {input::Source::Key(14), input::Source::Pad(1)})
        .Bind(static_cast<u8>(GameAction::DefensiveParry), {input::Source::Key(15), input::Source::Pad(3)})
        .Bind(static_cast<u8>(GameAction::WeaveShift),     {input::Source::Key(12), input::Source::Pad(9)})
        .Bind(static_cast<u8>(GameAction::WingLeftAssist), {input::Source::Key(20), input::Source::Pad(4)})
        .Bind(static_cast<u8>(GameAction::WingRightAssist),{input::Source::Key(8),  input::Source::Pad(5)})
        .Bind(static_cast<u8>(GameAction::DualUnison),     {input::Source::Key(24)})
        .Bind(static_cast<u8>(GameAction::TriadUnison),    {input::Source::Key(18)})
        .BindAxis(0u, input::AxisBinding{
            .negative = input::Source::Key(4),
            .positive = input::Source::Key(7),
            .deadzone = 0.02f
        });

    if (headless) {
        auto stepBarrier = m_app->StepHeadless(1, 0);
        if (!stepBarrier) {
            return Err(framework::FrameworkError{
                .code = framework::FrameworkErrc::ClientBootFailed,
                .stage = framework::SubSystemStage::ClientApp
            });
        }
    }

    m_initialized = true;
    return {};
}

void GameApp::Shutdown() noexcept {
    if (m_initialized) {
        m_app->Shutdown();
        m_app.reset();
        m_state = {};
        m_initialized = false;
    }
}

CombatTickSummary GameApp::StepSimulationTick(
    f32 moveX,
    bool jumpPressed,
    bool evadePressed,
    bool primaryAttackPressed,
    bool weaveShiftPressed,
    bool wingLeftPressed,
    bool wingRightPressed,
    bool parryPressed,
    bool dualUnisonPressed,
    bool triadUnisonPressed) noexcept {
    if (!m_initialized || !m_app) {
        return {};
    }

    input::TickInput in{};
    in.axis[0] = static_cast<i16>(std::clamp(moveX, -1.0f, 1.0f) * 32767.0f);

    auto setAction = [&](GameAction act, bool pressed) {
        const u16 bit = static_cast<u16>(1u << static_cast<u8>(act));
        if (pressed) {
            in.pressed |= bit;
            in.held    |= bit;
        }
    };

    setAction(GameAction::Jump, jumpPressed);
    setAction(GameAction::EvadeDash, evadePressed);
    setAction(GameAction::PrimaryAttack, primaryAttackPressed);
    setAction(GameAction::WeaveShift, weaveShiftPressed);
    setAction(GameAction::WingLeftAssist, wingLeftPressed);
    setAction(GameAction::WingRightAssist, wingRightPressed);
    setAction(GameAction::DefensiveParry, parryPressed);
    setAction(GameAction::DualUnison, dualUnisonPressed);
    setAction(GameAction::TriadUnison, triadUnisonPressed);

    m_app->ActionBuffer().Feed(in, false);
    (void)m_app->StepHeadless(1, 0);

    return m_state.lastSummary;
}

ResonanceLanternComponent* GameApp::PlayerLantern() noexcept {
    return m_app ? m_app->World().TryGet<ResonanceLanternComponent>(m_state.playerEntity) : nullptr;
}

AmplitudeAscensionComponent* GameApp::PlayerAmplitude() noexcept {
    return m_app ? m_app->World().TryGet<AmplitudeAscensionComponent>(m_state.playerEntity) : nullptr;
}

TraversalMutationComponent* GameApp::PlayerTraversal() noexcept {
    return m_app ? m_app->World().TryGet<TraversalMutationComponent>(m_state.playerEntity) : nullptr;
}

VeilCombatStatsComponent* GameApp::PlayerCombatStats() noexcept {
    return m_app ? m_app->World().TryGet<VeilCombatStatsComponent>(m_state.playerEntity) : nullptr;
}

VeilActionStateComponent* GameApp::PlayerActionState() noexcept {
    return m_app ? m_app->World().TryGet<VeilActionStateComponent>(m_state.playerEntity) : nullptr;
}

PhaseInversionCounterComponent* GameApp::PlayerParry() noexcept {
    return m_app ? m_app->World().TryGet<PhaseInversionCounterComponent>(m_state.playerEntity) : nullptr;
}

VeilCombatStatsComponent* GameApp::BossCombatStats() noexcept {
    return m_app ? m_app->World().TryGet<VeilCombatStatsComponent>(m_state.bossEntity) : nullptr;
}

StandingWaveComponent* GameApp::BossWave() noexcept {
    return m_app ? m_app->World().TryGet<StandingWaveComponent>(m_state.bossEntity) : nullptr;
}

BossConductorComponent* GameApp::BossConductor() noexcept {
    return m_app ? m_app->World().TryGet<BossConductorComponent>(m_state.bossEntity) : nullptr;
}

WorldProgressComponent* GameApp::PlayerProgress() noexcept {
    return m_app ? m_app->World().TryGet<WorldProgressComponent>(m_state.playerEntity) : nullptr;
}

bool GameApp::RunTests() noexcept {
    return RunVeilTestSuite();
}

} // namespace en::veil

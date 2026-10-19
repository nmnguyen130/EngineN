#pragma once

#include "framework/FrameworkTypes.h"
#include "game/core/Components.h"
#include "game/core/Types.h"

namespace en::veil {

// State variables tracking client scene presentation, camera position, and transient visual effects.
// Allocated in GameApp heap arena during boot; zero allocation post-init.
struct VeilAppState {
    world::EntityId playerEntity{world::kNullEntity};
    world::EntityId bossEntity{world::kNullEntity};
    world::EntityId platformEntity{world::kNullEntity};
    u64 simTickCount{0};
    CombatTickSummary lastSummary{};
    f32 cameraX{0.0f};
    u8 slashVisualTicks{0};
    u8 hitSparkVisualTicks{0};
    u8 wingVisualTicks{0};
    u8 unisonVisualTicks{0};
    u8 reactionVisualTicks{0};
    u8 counterVisualTicks{0};
    u8 shiftVisualTicks{0};
    u8 parryShieldVisualTicks{0};
    u8 deflectBannerVisualTicks{0};
    WaveReactionId activeReaction{WaveReactionId::None};
    u16 playerRespawnTicks{0};
    u16 bossRespawnTicks{0};
    bool slashFacingRight{true};
    EchoId activeSlashEcho{EchoId::Ignis};
    bool isInitialized{false};
};

// Manages presentation state layer: Scene load, ECS entity spawning, input handling, and render extraction.
// Non-owning presentation handler registered with SceneDirector.
class VeilStateLayers {
public:
    static constexpr framework::StateId kGameplayLayerId = 10;

    [[nodiscard]] static framework::StateLayerDesc CreateGameplayLayerDesc(VeilAppState* state) noexcept;

    static void OnGameplaySimTick(framework::SimContext& ctx) noexcept;
    static void OnGameplayRenderExtract(framework::RenderContext& ctx) noexcept;
    static void OnGameplayEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept;
    static void OnGameplayExit(framework::SceneLoadContext& ctx, void* stateData) noexcept;
};

} // namespace en::veil

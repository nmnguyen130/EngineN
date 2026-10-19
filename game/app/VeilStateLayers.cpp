#include "game/app/VeilStateLayers.h"
#include "game/movement/TraversalSystem.h"
#include "game/combat/CombatSystem.h"
#include "game/ai/BossSystem.h"
#include "assets/AssetBank.h"
#include "assets/AssetSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "renderer/RenderList.h"
#include "world/World.h"

#include <cmath>
#include <vector>

namespace en::veil {

namespace {

[[nodiscard]] constexpr renderer::Color GetEchoColor(EchoId id, f32 alpha = 1.0f) noexcept {
    switch (id) {
        case EchoId::Ignis:    return renderer::Color::Premultiplied(1.00f, 0.35f, 0.10f, alpha); // Thermal Orange
        case EchoId::Terra:    return renderer::Color::Premultiplied(0.70f, 0.50f, 0.20f, alpha); // Seismic Amber
        case EchoId::Fulgur:   return renderer::Color::Premultiplied(1.00f, 0.90f, 0.20f, alpha); // Electric Yellow
        case EchoId::Glacies:  return renderer::Color::Premultiplied(0.20f, 0.85f, 1.00f, alpha); // Cryo Cyan
        case EchoId::Ventus:   return renderer::Color::Premultiplied(0.30f, 0.95f, 0.60f, alpha); // Aero Green
        case EchoId::Aqua:     return renderer::Color::Premultiplied(0.15f, 0.50f, 1.00f, alpha); // Hydro Blue
        case EchoId::Lux:      return renderer::Color::Premultiplied(1.00f, 1.00f, 0.90f, alpha); // Radiant White
        case EchoId::Umbra:    return renderer::Color::Premultiplied(0.50f, 0.15f, 0.80f, alpha); // Abyssal Purple
        case EchoId::Gravitas: return renderer::Color::Premultiplied(0.40f, 0.20f, 0.65f, alpha); // Singularity Violet
        case EchoId::Tempus:   return renderer::Color::Premultiplied(0.85f, 0.75f, 0.30f, alpha); // Chronos Gold
        case EchoId::Aether:   return renderer::Color::Premultiplied(0.95f, 0.20f, 0.40f, alpha); // Sympathetic Crimson
        case EchoId::Stridor:  return renderer::Color::Premultiplied(0.90f, 0.45f, 0.15f, alpha); // Mach Shock Orange
        default:               return renderer::Color::Premultiplied(0.70f, 0.70f, 0.70f, alpha);
    }
}

// Draws a framed rectangle with border and dark interior background.
void PushFramedBar(
    renderer::RenderList& rl,
    f32 x, f32 y, f32 w, f32 h,
    f32 fillRatio,
    renderer::Color fillColor,
    renderer::Color borderColor = renderer::Color::Premultiplied(0.35f, 0.45f, 0.55f, 0.9f),
    renderer::Color bgColor = renderer::Color::Premultiplied(0.08f, 0.10f, 0.14f, 0.9f)) noexcept {
    // Outer border
    rl.PushRect(renderer::FRect{x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f}, borderColor);
    // Background trough
    rl.PushRect(renderer::FRect{x, y, w, h}, bgColor);
    // Fill bar
    if (fillRatio > 0.001f) {
        const f32 clampedRatio = std::clamp(fillRatio, 0.0f, 1.0f);
        rl.PushRect(renderer::FRect{x, y, w * clampedRatio, h}, fillColor);
    }
}

} // namespace

framework::StateLayerDesc VeilStateLayers::CreateGameplayLayerDesc(VeilAppState* state) noexcept {
    return framework::StateLayerDesc{
        .onSimTick = &VeilStateLayers::OnGameplaySimTick,
        .onRenderExtract = &VeilStateLayers::OnGameplayRenderExtract,
        .onEnter = &VeilStateLayers::OnGameplayEnter,
        .onExit = &VeilStateLayers::OnGameplayExit,
        .stateData = state,
        .id = kGameplayLayerId,
        .inputActionMask = 0xFFFF,
        .flags = framework::StateLayerFlags::None
    };
}

void VeilStateLayers::OnGameplayEnter(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* appState = static_cast<VeilAppState*>(stateData);
    if (appState == nullptr) return;

    // 1. Build and bind 48m flat solid floor room collision map to GameplayPipeline (top at y = 0.0m)
    constexpr u32 kChunksX = 6;
    constexpr u32 kChunksY = 1;
    constexpr u32 kChunkBytes = sizeof(gameplay::CookedCollisionChunk8x8) * kChunksX * kChunksY;
    constexpr u32 kFineOffset = sizeof(gameplay::CookedCollisionMapHeader) + kChunkBytes;
    constexpr u32 kFineBytes = sizeof(gameplay::CookedTileCell) * 64u * kChunksX * kChunksY;
    std::vector<u8> colBuf(kFineOffset + kFineBytes, 0);

    auto* hdr = reinterpret_cast<gameplay::CookedCollisionMapHeader*>(colBuf.data());
    hdr->magic = gameplay::CookedCollisionMapHeader::kMagic;
    hdr->version = gameplay::CookedCollisionMapHeader::kVersion;
    hdr->chunkCountX = kChunksX;
    hdr->chunkCountY = kChunksY;
    hdr->tileSize_q8 = 256;
    hdr->fineTileTableOffset = kFineOffset;
    hdr->originX = -20.0f;
    hdr->originY = -1.0f;

    auto* chunks = reinterpret_cast<gameplay::CookedCollisionChunk8x8*>(colBuf.data() + sizeof(gameplay::CookedCollisionMapHeader));
    for (u32 cx = 0; cx < kChunksX; ++cx) {
        chunks[cx].solidBitmask = 0xFFULL;
        chunks[cx].pureFullBlockBitmask = 0xFFULL;
    }

    bool needsLoad = false;
    auto slotRes = ctx.assets.Bank().AcquireSlot(
        assets::AssetId::Hash("col/arena.bin"),
        assets::AssetKind::Blob,
        0,
        assets::Residency::Sim,
        needsLoad);
    if (slotRes) {
        (void)ctx.assets.Bank().CommitBlob(*slotRes, colBuf, 0, false);
        const auto colBlob = ctx.assets.Bank().ToBlobHandle(*slotRes);
        (void)ctx.gameplay.SetActiveRoomCollision(ctx.assets.Bank(), colBlob);
    }

    // 2. Spawn Arena Ground Platform Carrier (top at y = 0.0m)
    const auto platformRes = ctx.world.Spawn(world::EntitySpawnDesc{
        .localTransform = {.x = 4.0f, .y = -0.5f, .scaleX = 1.0f, .scaleY = 1.0f}
    });
    if (platformRes) {
        appState->platformEntity = *platformRes;
        (void)ctx.world.Emplace<gameplay::KinematicCarrier2D>(appState->platformEntity, gameplay::KinematicCarrier2D{
            .deltaX = 0.0f,
            .deltaY = 0.0f,
            .bounds = gameplay::CompactAabb2D::FromMeters(0.0f, 0.0f, 25.0f, 0.5f)
        });
    }

    // 3. Spawn Player Entity (resting on top of floor: y = 1.0m, halfH = 1.0m -> bottom = 0.0m)
    const auto playerRes = ctx.world.Spawn(world::EntitySpawnDesc{
        .localTransform = {.x = -3.0f, .y = 1.05f, .scaleX = 1.0f, .scaleY = 1.0f}
    });
    if (playerRes) {
        appState->playerEntity = *playerRes;
        (void)ctx.world.Emplace<gameplay::KinematicMotor2D>(appState->playerEntity, gameplay::KinematicMotor2D{
            .bounds = gameplay::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 1.0f),
            .carrierId = appState->platformEntity,
            .contacts = gameplay::ContactFlags::Ground
        });
        (void)ctx.world.Emplace<gameplay::MotionStack2D>(appState->playerEntity, gameplay::MotionStack2D{});
        (void)ctx.world.Emplace<gameplay::StateMachine2D>(appState->playerEntity, gameplay::StateMachine2D{
            .cancelMask = 0xFFFFu
        });
        (void)ctx.world.Emplace<gameplay::HitboxRig2D>(appState->playerEntity, gameplay::HitboxRig2D{
            .hurtbox = gameplay::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.4f, 1.0f),
            .hitLayerMask = 0x02u,
            .hurtLayerMask = 0x01u
        });
        (void)ctx.world.Emplace<ResonanceLanternComponent>(appState->playerEntity, ResonanceLanternComponent{
            .triadWicks = {EchoId::Ignis, EchoId::Glacies, EchoId::Fulgur}
        });
        (void)ctx.world.Emplace<AmplitudeAscensionComponent>(appState->playerEntity, AmplitudeAscensionComponent{});
        (void)ctx.world.Emplace<TraversalMutationComponent>(appState->playerEntity, TraversalMutationComponent{});
        (void)ctx.world.Emplace<VeilCombatStatsComponent>(appState->playerEntity, VeilCombatStatsComponent{
            .hp = 1000.0f,
            .maxHp = 1000.0f,
            .breakMeter = 0.0f,
            .maxBreakMeter = 300.0f,
            .baseArmorRatio = 0.15f
        });
        (void)ctx.world.Emplace<VeilActionStateComponent>(appState->playerEntity, VeilActionStateComponent{});
        (void)ctx.world.Emplace<PhaseInversionCounterComponent>(appState->playerEntity, PhaseInversionCounterComponent{});
        (void)ctx.world.Emplace<WingAssistComponent>(appState->playerEntity, WingAssistComponent{});
        (void)ctx.world.Emplace<WorldProgressComponent>(appState->playerEntity, WorldProgressComponent{});
    }

    // 4. Spawn Boss Entity (resting on top of floor: y = 1.5m, halfH = 1.5m -> bottom = 0.0m)
    const auto bossRes = ctx.world.Spawn(world::EntitySpawnDesc{
        .localTransform = {.x = 4.0f, .y = 1.55f, .scaleX = 1.0f, .scaleY = 1.0f}
    });
    if (bossRes) {
        appState->bossEntity = *bossRes;
        (void)ctx.world.Emplace<gameplay::KinematicMotor2D>(appState->bossEntity, gameplay::KinematicMotor2D{
            .bounds = gameplay::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.8f, 1.5f),
            .carrierId = appState->platformEntity,
            .contacts = gameplay::ContactFlags::Ground
        });
        (void)ctx.world.Emplace<gameplay::MotionStack2D>(appState->bossEntity, gameplay::MotionStack2D{});
        (void)ctx.world.Emplace<gameplay::HitboxRig2D>(appState->bossEntity, gameplay::HitboxRig2D{
            .hurtbox = gameplay::CompactAabb2D::FromMeters(0.0f, 0.0f, 0.8f, 1.5f),
            .hitLayerMask = 0x01u,
            .hurtLayerMask = 0x02u
        });
        (void)ctx.world.Emplace<gameplay::DamageReceiver2D>(appState->bossEntity, gameplay::DamageReceiver2D{
            .hp = 2500,
            .maxHp = 2500
        });
        (void)ctx.world.Emplace<VeilCombatStatsComponent>(appState->bossEntity, VeilCombatStatsComponent{
            .hp = 2500.0f,
            .maxHp = 2500.0f,
            .breakMeter = 0.0f,
            .maxBreakMeter = 600.0f,
            .baseArmorRatio = 0.30f
        });
        (void)ctx.world.Emplace<StandingWaveComponent>(appState->bossEntity, StandingWaveComponent{});
        (void)ctx.world.Emplace<BossConductorComponent>(appState->bossEntity, BossConductorComponent{
            .bossEchoAffinity = EchoId::Terra,
            .requiredCounterEcho = EchoId::Fulgur
        });
    }

    ctx.world.PropagateTransforms();
    appState->cameraX = -3.0f;
    appState->isInitialized = true;
}

void VeilStateLayers::OnGameplayExit(framework::SceneLoadContext& ctx, void* stateData) noexcept {
    auto* appState = static_cast<VeilAppState*>(stateData);
    if (appState == nullptr) return;

    if (appState->playerEntity.IsValid()) {
        ctx.world.Destroy(appState->playerEntity);
        appState->playerEntity = world::kNullEntity;
    }
    if (appState->bossEntity.IsValid()) {
        ctx.world.Destroy(appState->bossEntity);
        appState->bossEntity = world::kNullEntity;
    }
    if (appState->platformEntity.IsValid()) {
        ctx.world.Destroy(appState->platformEntity);
        appState->platformEntity = world::kNullEntity;
    }
    appState->isInitialized = false;
}

void VeilStateLayers::OnGameplaySimTick(framework::SimContext& ctx) noexcept {
    auto* appState = static_cast<VeilAppState*>(ctx.userState);
    if (appState == nullptr || !appState->isInitialized) return;

    ++appState->simTickCount;

    auto* lantern = ctx.world.TryGet<ResonanceLanternComponent>(appState->playerEntity);
    auto* amp = ctx.world.TryGet<AmplitudeAscensionComponent>(appState->playerEntity);
    auto* action = ctx.world.TryGet<VeilActionStateComponent>(appState->playerEntity);
    auto* traversal = ctx.world.TryGet<TraversalMutationComponent>(appState->playerEntity);
    auto* wing = ctx.world.TryGet<WingAssistComponent>(appState->playerEntity);
    auto* parry = ctx.world.TryGet<PhaseInversionCounterComponent>(appState->playerEntity);
    auto* playerStats = ctx.world.TryGet<VeilCombatStatsComponent>(appState->playerEntity);

    auto* bossStats = ctx.world.TryGet<VeilCombatStatsComponent>(appState->bossEntity);
    auto* bossWave = ctx.world.TryGet<StandingWaveComponent>(appState->bossEntity);
    auto* bossConductor = ctx.world.TryGet<BossConductorComponent>(appState->bossEntity);

    if (lantern != nullptr && amp != nullptr && action != nullptr && traversal != nullptr &&
        wing != nullptr && parry != nullptr && playerStats != nullptr &&
        bossStats != nullptr && bossWave != nullptr && bossConductor != nullptr) {

        const bool primaryAttack = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::PrimaryAttack), 6);
        const bool weaveShift    = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::WeaveShift), 6);
        const bool wingL         = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::WingLeftAssist), 6);
        const bool wingR         = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::WingRightAssist), 6);
        const bool parryPress    = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::DefensiveParry), 6);
        const bool dualUnison    = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::DualUnison), 6);
        const bool triadUnison   = ctx.actionBuffer.Consume(static_cast<u8>(GameAction::TriadUnison), 6);

        const auto& playerTf = ctx.world.GetLocalTransform(appState->playerEntity);
        const auto& bossTf = ctx.world.GetLocalTransform(appState->bossEntity);
        const f32 distX = bossTf.x - playerTf.x;
        const f32 distY = bossTf.y - playerTf.y;
        const f32 distance = std::sqrt(distX * distX + distY * distY);

        const EchoSpec& leadSpec = GetEchoSpec(lantern->LeadEcho());
        const f32 slashReachMeters = leadSpec.baseReachMeters * amp->ReachMultiplier();
        const bool facingBoss = traversal->facingRight ? (distX >= -0.3f) : (distX <= 0.3f);
        const bool inMeleeReach = (distance <= (slashReachMeters + 0.6f)) && facingBoss;
        const bool inWingReach = (distance <= 12.0f);

        // Smooth camera tracking with arena boundary clamp
        appState->cameraX += (playerTf.x - appState->cameraX) * 0.12f;
        appState->cameraX = std::clamp(appState->cameraX, -14.0f, 22.0f);

        // Visual timer decay
        if (appState->slashVisualTicks > 0) --appState->slashVisualTicks;
        if (appState->hitSparkVisualTicks > 0) --appState->hitSparkVisualTicks;
        if (appState->wingVisualTicks > 0) --appState->wingVisualTicks;
        if (appState->unisonVisualTicks > 0) --appState->unisonVisualTicks;
        if (appState->reactionVisualTicks > 0) --appState->reactionVisualTicks;
        if (appState->counterVisualTicks > 0) --appState->counterVisualTicks;
        if (appState->shiftVisualTicks > 0) --appState->shiftVisualTicks;
        if (appState->parryShieldVisualTicks > 0) --appState->parryShieldVisualTicks;
        if (appState->deflectBannerVisualTicks > 0) --appState->deflectBannerVisualTicks;

        if (primaryAttack) {
            appState->slashVisualTicks = 14;
            appState->slashFacingRight = traversal->facingRight;
            appState->activeSlashEcho = lantern->LeadEcho();
        }
        if (wingL || wingR) {
            appState->wingVisualTicks = 16;
        }
        if (dualUnison || triadUnison) {
            appState->unisonVisualTicks = 24;
        }
        if (weaveShift) {
            appState->shiftVisualTicks = 36;
        }
        if (parryPress) {
            appState->parryShieldVisualTicks = 18;
        }

        appState->lastSummary = CombatSystem::Step(
            *lantern,
            *amp,
            *action,
            *traversal,
            *wing,
            *parry,
            *playerStats,
            *bossStats,
            *bossWave,
            bossConductor,
            primaryAttack,
            weaveShift,
            wingL,
            wingR,
            parryPress,
            dualUnison,
            triadUnison,
            inMeleeReach,
            inWingReach);

        if (appState->lastSummary.damageDealt > 0.0f) {
            appState->hitSparkVisualTicks = 12;
        }
        if (appState->lastSummary.triggeredReaction != WaveReactionId::None) {
            appState->reactionVisualTicks = 80;
            appState->activeReaction = appState->lastSummary.triggeredReaction;
        }
        if (appState->lastSummary.phaseInversionSucceeded) {
            appState->counterVisualTicks = 50;
            appState->hitSparkVisualTicks = 18;
        }
        if (appState->lastSummary.parryDeflectionSucceeded) {
            appState->deflectBannerVisualTicks = 45;
            appState->hitSparkVisualTicks = 14;
        }

        // Step Boss Conductor AI: executes attacks and damages player
        BossSystem::Step(*bossConductor, *bossStats, playerStats, distance, action);

        if (ctx.world.IsAlive(appState->bossEntity)) {
            auto& mutableBossTf = ctx.world.ModifyLocalTransformByIndex(appState->bossEntity.index);
            if (mutableBossTf.y < 1.501f) {
                mutableBossTf.y = 1.501f;
            }
            // Boss combat spacing: moves toward player if far, holds ground if close
            if (bossStats->alive && bossStats->posture != BossPosture::Submissive) {
                if (bossConductor->activeTelegraph == BossTelegraphKind::None) {
                    if (distX > 3.6f) {
                        mutableBossTf.x -= 0.025f;
                    } else if (distX < -3.6f) {
                        mutableBossTf.x += 0.025f;
                    } else if (std::abs(distX) < 1.8f) {
                        mutableBossTf.x += (distX >= 0.0f ? 0.018f : -0.018f);
                    }
                }
            }
            mutableBossTf.x = std::clamp(mutableBossTf.x, -18.0f, 26.0f);
        }

        // Player auto-respawn if defeated
        if (!playerStats->alive || playerStats->hp <= 0.0f) {
            ++appState->playerRespawnTicks;
            if (appState->playerRespawnTicks >= 120) {
                appState->playerRespawnTicks = 0;
                playerStats->hp = playerStats->maxHp;
                playerStats->alive = true;
                playerStats->breakMeter = 0.0f;
                if (ctx.world.IsAlive(appState->playerEntity)) {
                    auto& pTf = ctx.world.ModifyLocalTransformByIndex(appState->playerEntity.index);
                    pTf.x = -3.0f;
                    pTf.y = 1.05f;
                }
            }
        }

        // Boss auto-revival after defeat for infinite gameplay loop
        if (!bossStats->alive || bossStats->hp <= 0.0f) {
            ++appState->bossRespawnTicks;
            if (appState->bossRespawnTicks >= 300) {
                appState->bossRespawnTicks = 0;
                bossStats->hp = bossStats->maxHp;
                bossStats->alive = true;
                bossStats->breakMeter = 0.0f;
                bossStats->posture = BossPosture::Neutral;
                bossConductor->symphonyMovementPhase = 1;
                bossConductor->activeTelegraph = BossTelegraphKind::None;
                if (ctx.world.IsAlive(appState->bossEntity)) {
                    auto& bTf = ctx.world.ModifyLocalTransformByIndex(appState->bossEntity.index);
                    bTf.x = 4.0f;
                    bTf.y = 1.55f;
                }
            }
        }
    }
}

void VeilStateLayers::OnGameplayRenderExtract(framework::RenderContext& ctx) noexcept {
    auto* appState = static_cast<VeilAppState*>(ctx.userState);
    if (appState == nullptr || !appState->isInitialized) return;

    auto* lantern = ctx.world.TryGet<ResonanceLanternComponent>(appState->playerEntity);
    auto* amp = ctx.world.TryGet<AmplitudeAscensionComponent>(appState->playerEntity);
    auto* playerStats = ctx.world.TryGet<VeilCombatStatsComponent>(appState->playerEntity);
    auto* bossStats = ctx.world.TryGet<VeilCombatStatsComponent>(appState->bossEntity);
    auto* bossWave = ctx.world.TryGet<StandingWaveComponent>(appState->bossEntity);
    auto* traversal = ctx.world.TryGet<TraversalMutationComponent>(appState->playerEntity);
    auto* bossConductor = ctx.world.TryGet<BossConductorComponent>(appState->bossEntity);

    constexpr f32 kPpm = 50.0f;
    const f32 camX = appState->cameraX;
    auto toScreenX = [&](f32 worldX) noexcept -> f32 {
        return 640.0f + (worldX - camX) * kPpm;
    };

    // =========================================================================
    // 1. Atmosphere & Parallax Background Architecture
    // =========================================================================
    // Backdrop dark gradient band
    ctx.renderList.PushRect(
        renderer::FRect{0.0f, 0.0f, 1280.0f, 720.0f},
        renderer::Color::Premultiplied(0.03f, 0.05f, 0.08f, 1.0f));

    // Parallax background gothic acoustic pillars (scrolls at 0.35x camera speed)
    for (f32 px = -30.0f; px <= 40.0f; px += 8.0f) {
        const f32 bgPillarX = 640.0f + (px - camX * 0.35f) * kPpm;
        ctx.renderList.PushRect(
            renderer::FRect{bgPillarX - 16.0f, 160.0f, 32.0f, 380.0f},
            renderer::Color::Premultiplied(0.06f, 0.09f, 0.14f, 0.7f));
        // Pillar arch accent
        ctx.renderList.PushRect(
            renderer::FRect{bgPillarX - 22.0f, 150.0f, 44.0f, 12.0f},
            renderer::Color::Premultiplied(0.09f, 0.13f, 0.19f, 0.8f));
    }

    // =========================================================================
    // 2. Arena Floor & Geometry
    // =========================================================================
    // Arena Floor Slab (48m wide from worldX = -20.0m to +28.0m)
    const f32 floorX = toScreenX(-20.0f);
    const f32 floorW = 48.0f * kPpm;

    // Dark solid slab body
    ctx.renderList.PushRect(
        renderer::FRect{floorX, 540.0f, floorW, 140.0f},
        renderer::Color::Premultiplied(0.09f, 0.12f, 0.17f, 1.0f));

    // Glowing cyan resonance top rail line
    ctx.renderList.PushRect(
        renderer::FRect{floorX, 538.0f, floorW, 3.0f},
        renderer::Color::Premultiplied(0.20f, 0.80f, 1.00f, 0.9f));

    // Distance hash notches every 2 meters, prominent markers every 4 meters
    for (f32 mx = -20.0f; mx <= 28.0f; mx += 2.0f) {
        const bool isMajor = (static_cast<int>(std::round(mx)) % 4) == 0;
        const f32 hashX = toScreenX(mx);
        const f32 hashH = isMajor ? 14.0f : 7.0f;
        const f32 hashW = isMajor ? 3.0f : 2.0f;
        const renderer::Color hashCol = isMajor
            ? renderer::Color::Premultiplied(0.35f, 0.65f, 0.90f, 0.7f)
            : renderer::Color::Premultiplied(0.20f, 0.30f, 0.45f, 0.4f);
        ctx.renderList.PushRect(renderer::FRect{hashX - hashW * 0.5f, 542.0f, hashW, hashH}, hashCol);
    }

    // =========================================================================
    // 3. Player Character (Kaelen) & Combat Visual FX
    // =========================================================================
    const auto playerTransform = ctx.world.GetLocalTransform(appState->playerEntity);
    const f32 px = toScreenX(playerTransform.x) - 16.0f;
    const f32 py = 540.0f - (playerTransform.y - 1.0f) * kPpm - 60.0f;
    const bool facingRight = traversal != nullptr ? traversal->facingRight : true;
    const EchoId leadEcho = lantern != nullptr ? lantern->LeadEcho() : EchoId::Ignis;
    const renderer::Color leadColor = GetEchoColor(leadEcho);

    // Ground Shadow
    ctx.renderList.PushRect(
        renderer::FRect{px - 4.0f, 538.0f, 40.0f, 4.0f},
        renderer::Color::Premultiplied(0.0f, 0.0f, 0.0f, 0.6f));

    // Dash motion trail ghost
    if (traversal != nullptr && traversal->traversalActiveTicks > 0) {
        const f32 ghostOffset = facingRight ? -18.0f : 18.0f;
        ctx.renderList.PushRect(
            renderer::FRect{px + ghostOffset, py, 32.0f, 60.0f},
            renderer::Color::Premultiplied(leadColor.r, leadColor.g, leadColor.b, 0.35f));
    }

    // Player Silhouette Body (torso + mantle)
    ctx.renderList.PushRect(
        renderer::FRect{px + 2.0f, py + 16.0f, 28.0f, 44.0f},
        renderer::Color::Premultiplied(0.12f, 0.22f, 0.35f, 1.0f));
    // Player Head & Hood
    ctx.renderList.PushRect(
        renderer::FRect{px + 6.0f, py, 20.0f, 18.0f},
        renderer::Color::Premultiplied(0.18f, 0.32f, 0.48f, 1.0f));
    // Glowing Visor / Eye
    const f32 visorX = facingRight ? (px + 18.0f) : (px + 6.0f);
    ctx.renderList.PushRect(
        renderer::FRect{visorX, py + 7.0f, 8.0f, 5.0f},
        renderer::Color::Premultiplied(0.40f, 0.95f, 1.00f, 1.0f));

    // Resonance Lantern hovering near hip
    const f32 lanternX = facingRight ? (px - 6.0f) : (px + 30.0f);
    const f32 lanternY = py + 30.0f;
    // Outer glow
    ctx.renderList.PushRect(
        renderer::FRect{lanternX - 3.0f, lanternY - 3.0f, 14.0f, 18.0f},
        renderer::Color::Premultiplied(leadColor.r, leadColor.g, leadColor.b, 0.45f));
    // Core wick
    ctx.renderList.PushRect(
        renderer::FRect{lanternX, lanternY, 8.0f, 12.0f},
        leadColor);

    // Primary Attack Slash Arc FX - accurately scaled to baseReachMeters * ppm
    if (appState->slashVisualTicks > 0) {
        const EchoSpec& curSpec = GetEchoSpec(appState->activeSlashEcho);
        const f32 reachMult = (amp != nullptr) ? amp->ReachMultiplier() : 1.0f;
        const f32 slashW = curSpec.baseReachMeters * reachMult * kPpm;
        const renderer::Color slashCol = GetEchoColor(appState->activeSlashEcho, 0.90f);
        const f32 slashX = appState->slashFacingRight ? (px + 24.0f) : (px - slashW + 8.0f);
        // Outer crescent glow
        ctx.renderList.PushRect(
            renderer::FRect{slashX, py + 4.0f, slashW, 52.0f},
            slashCol);
        // Cutting edge bright white flash
        const f32 bladeEdgeX = appState->slashFacingRight ? (slashX + slashW - 14.0f) : slashX;
        ctx.renderList.PushRect(
            renderer::FRect{bladeEdgeX, py + 10.0f, 14.0f, 40.0f},
            renderer::Color::Premultiplied(1.0f, 1.0f, 1.0f, 0.95f));
    }

    // Defensive Parry Shield Barrier FX (Key [L])
    if (appState->parryShieldVisualTicks > 0) {
        const f32 shieldX = facingRight ? (px + 28.0f) : (px - 22.0f);
        // Outer glowing force field arc
        ctx.renderList.PushRect(
            renderer::FRect{shieldX - 4.0f, py - 6.0f, 20.0f, 72.0f},
            renderer::Color::Premultiplied(0.20f, 0.85f, 1.00f, 0.45f));
        // Crystalline guard barrier core
        ctx.renderList.PushRect(
            renderer::FRect{shieldX, py, 12.0f, 60.0f},
            renderer::Color::Premultiplied(0.40f, 0.95f, 1.00f, 0.90f));
        // Prismatic central deflection crest
        ctx.renderList.PushRect(
            renderer::FRect{shieldX + 3.0f, py + 16.0f, 6.0f, 28.0f},
            renderer::Color::Premultiplied(1.00f, 1.00f, 1.00f, 0.98f));
    }

    // =========================================================================
    // 4. Boss Entity (Resonance Conductor)
    // =========================================================================
    const auto bossTransform = ctx.world.GetLocalTransform(appState->bossEntity);
    const f32 bx = toScreenX(bossTransform.x) - 25.0f;
    const f32 by = 540.0f - (bossTransform.y - 1.5f) * kPpm - 90.0f;

    // Polyphonic Unison Resonance Beam FX (Key [U] and [O])
    if (appState->unisonVisualTicks > 0) {
        const f32 minX = std::min(px + 16.0f, bx + 25.0f);
        const f32 beamW = std::max(60.0f, std::abs((bx + 25.0f) - (px + 16.0f)));
        ctx.renderList.PushRect(
            renderer::FRect{minX, py + 14.0f, beamW, 32.0f},
            renderer::Color::Premultiplied(0.95f, 0.25f, 0.90f, 0.75f));
        ctx.renderList.PushRect(
            renderer::FRect{minX, py + 22.0f, beamW, 16.0f},
            renderer::Color::Premultiplied(1.00f, 1.00f, 1.00f, 0.95f));
    }

    // Boss Ground Shadow
    ctx.renderList.PushRect(
        renderer::FRect{bx - 6.0f, 538.0f, 62.0f, 5.0f},
        renderer::Color::Premultiplied(0.0f, 0.0f, 0.0f, 0.6f));

    // Boss Gold Harmonic Telegraph Halo (Parry Opportunity!)
    if (bossConductor != nullptr && bossConductor->activeTelegraph == BossTelegraphKind::GoldHarmonic) {
        // Brilliant pulsing golden telegraph aura around boss
        ctx.renderList.PushRect(
            renderer::FRect{bx - 10.0f, by - 10.0f, 70.0f, 110.0f},
            renderer::Color::Premultiplied(1.00f, 0.85f, 0.15f, 0.45f));
        // Top and bottom parry bracket indicators
        ctx.renderList.PushRect(
            renderer::FRect{bx - 14.0f, by - 14.0f, 78.0f, 5.0f},
            renderer::Color::Premultiplied(1.00f, 0.95f, 0.30f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{bx - 14.0f, by + 99.0f, 78.0f, 5.0f},
            renderer::Color::Premultiplied(1.00f, 0.95f, 0.30f, 0.95f));

        // Center-screen Golden Parry Banner Prompt
        ctx.renderList.PushRect(
            renderer::FRect{490.0f, 95.0f, 300.0f, 30.0f},
            renderer::Color::Premultiplied(1.00f, 0.85f, 0.10f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{492.0f, 97.0f, 296.0f, 26.0f},
            renderer::Color::Premultiplied(0.12f, 0.08f, 0.02f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{590.0f, 102.0f, 100.0f, 16.0f},
            renderer::Color::Premultiplied(1.00f, 0.90f, 0.20f, 1.0f));
    }

    // Boss Body - Obsidian Heavy Armor
    renderer::Color bossBodyColor = renderer::Color::Premultiplied(0.20f, 0.08f, 0.12f, 1.0f);
    if (bossStats != nullptr && bossStats->posture == BossPosture::Submissive) {
        // Staggered / Submissive posture tint (shattered defense)
        bossBodyColor = renderer::Color::Premultiplied(0.15f, 0.40f, 0.50f, 0.9f);
    }
    ctx.renderList.PushRect(renderer::FRect{bx, by, 50.0f, 90.0f}, bossBodyColor);

    // Boss Chestplate Resonance Sigil
    const EchoId bossAffinity = bossConductor != nullptr ? bossConductor->bossEchoAffinity : EchoId::Terra;
    const renderer::Color bossSigilColor = GetEchoColor(bossAffinity);
    ctx.renderList.PushRect(
        renderer::FRect{bx + 18.0f, by + 24.0f, 14.0f, 20.0f},
        bossSigilColor);

    // Boss Attack Shockwave Strike FX
    if (bossConductor != nullptr && bossConductor->attackStrikeTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{bx - 45.0f, by + 10.0f, 140.0f, 75.0f},
            renderer::Color::Premultiplied(1.0f, 0.2f, 0.15f, 0.85f));
        ctx.renderList.PushRect(
            renderer::FRect{bx - 30.0f, by + 20.0f, 110.0f, 55.0f},
            renderer::Color::Premultiplied(1.0f, 0.9f, 0.25f, 0.95f));
    }

    // Hit Spark Impact Flash
    if (appState->hitSparkVisualTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{bx - 10.0f, by - 10.0f, 70.0f, 110.0f},
            renderer::Color::Premultiplied(1.0f, 1.0f, 1.0f, 0.85f));
    }

    // Wing Assist Flying Projectile FX
    if (appState->wingVisualTicks > 0) {
        const f32 t = 1.0f - (static_cast<f32>(appState->wingVisualTicks) / 16.0f);
        const f32 wx = px + (bx - px) * t;
        const EchoId wingEcho = lantern != nullptr ? lantern->WingLeftEcho() : EchoId::Glacies;
        const renderer::Color wingCol = GetEchoColor(wingEcho, 0.9f);
        ctx.renderList.PushRect(
            renderer::FRect{wx, py + 16.0f, 32.0f, 20.0f},
            wingCol);
        // Projectile core
        ctx.renderList.PushRect(
            renderer::FRect{wx + 8.0f, py + 20.0f, 16.0f, 12.0f},
            renderer::Color::Premultiplied(1.0f, 1.0f, 1.0f, 1.0f));
    }

    // Standing-Wave Reaction Banner over Boss
    if (appState->reactionVisualTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{bx - 50.0f, by - 48.0f, 150.0f, 26.0f},
            renderer::Color::Premultiplied(0.95f, 0.50f, 0.10f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{bx - 48.0f, by - 46.0f, 146.0f, 22.0f},
            renderer::Color::Premultiplied(0.08f, 0.08f, 0.14f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{bx - 40.0f, by - 40.0f, 130.0f, 10.0f},
            renderer::Color::Premultiplied(0.20f, 0.90f, 1.00f, 0.90f));
    }

    // Phase-Inversion Counter Success Banner
    if (appState->counterVisualTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{440.0f, 135.0f, 400.0f, 34.0f},
            renderer::Color::Premultiplied(1.00f, 0.90f, 0.20f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{442.0f, 137.0f, 396.0f, 30.0f},
            renderer::Color::Premultiplied(0.12f, 0.12f, 0.05f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{520.0f, 143.0f, 240.0f, 18.0f},
            renderer::Color::Premultiplied(1.00f, 0.85f, 0.10f, 1.0f));
    }

    // Guard Deflection Success Banner
    if (appState->deflectBannerVisualTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{440.0f, 135.0f, 400.0f, 34.0f},
            renderer::Color::Premultiplied(0.20f, 0.85f, 1.00f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{442.0f, 137.0f, 396.0f, 30.0f},
            renderer::Color::Premultiplied(0.04f, 0.12f, 0.20f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{500.0f, 143.0f, 280.0f, 18.0f},
            renderer::Color::Premultiplied(0.40f, 0.95f, 1.00f, 1.0f));
    }

    // Weave Shift Flash Indicator over Player
    if (appState->shiftVisualTicks > 0) {
        ctx.renderList.PushRect(
            renderer::FRect{px - 20.0f, py - 24.0f, 72.0f, 16.0f},
            renderer::Color::Premultiplied(0.1f, 0.15f, 0.2f, 0.85f));
        ctx.renderList.PushRect(
            renderer::FRect{px - 18.0f, py - 22.0f, 68.0f, 12.0f},
            leadColor);
    }

    // Defeat / Victory Banners
    if (playerStats != nullptr && (!playerStats->alive || playerStats->hp <= 0.0f)) {
        ctx.renderList.PushRect(
            renderer::FRect{460.0f, 320.0f, 360.0f, 44.0f},
            renderer::Color::Premultiplied(0.70f, 0.15f, 0.15f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{462.0f, 322.0f, 356.0f, 40.0f},
            renderer::Color::Premultiplied(0.12f, 0.03f, 0.03f, 0.95f));
    }
    if (bossStats != nullptr && (!bossStats->alive || bossStats->hp <= 0.0f)) {
        ctx.renderList.PushRect(
            renderer::FRect{460.0f, 320.0f, 360.0f, 44.0f},
            renderer::Color::Premultiplied(0.20f, 0.85f, 0.40f, 0.95f));
        ctx.renderList.PushRect(
            renderer::FRect{462.0f, 322.0f, 356.0f, 40.0f},
            renderer::Color::Premultiplied(0.03f, 0.12f, 0.05f, 0.95f));
    }

    // =========================================================================
    // 5. Professional Framed HUD (Clear & Intuitive)
    // =========================================================================
    // --- Player HUD (Top-Left) ---
    // Background card panel
    ctx.renderList.PushRect(
        renderer::FRect{16.0f, 16.0f, 280.0f, 92.0f},
        renderer::Color::Premultiplied(0.04f, 0.06f, 0.09f, 0.85f));
    ctx.renderList.PushRect(
        renderer::FRect{15.0f, 15.0f, 282.0f, 1.0f},
        renderer::Color::Premultiplied(0.20f, 0.40f, 0.60f, 0.7f));

    if (playerStats != nullptr) {
        // Player HP Bar (Green)
        const f32 hpRatio = playerStats->maxHp > 0.0f ? (playerStats->hp / playerStats->maxHp) : 0.0f;
        PushFramedBar(
            ctx.renderList,
            24.0f, 24.0f, 200.0f, 14.0f,
            hpRatio,
            renderer::Color::Premultiplied(0.15f, 0.88f, 0.35f, 1.0f),
            renderer::Color::Premultiplied(0.25f, 0.50f, 0.30f, 0.9f));
    }

    if (lantern != nullptr && amp != nullptr) {
        // Harmony Gauge Bar (Cyan)
        const f32 harmonyRatio = lantern->harmonyGauge * 0.01f;
        PushFramedBar(
            ctx.renderList,
            24.0f, 42.0f, 200.0f, 7.0f,
            harmonyRatio,
            renderer::Color::Premultiplied(0.20f, 0.75f, 1.00f, 1.0f));

        // Wave Energy Bar (Violet)
        const f32 waveRatio = amp->waveEnergy * 0.01f;
        PushFramedBar(
            ctx.renderList,
            24.0f, 52.0f, 200.0f, 7.0f,
            waveRatio,
            renderer::Color::Premultiplied(0.85f, 0.25f, 0.95f, 1.0f));

        // Amplitude Tier Badge
        renderer::Color tierBorderColor = renderer::Color::Premultiplied(0.40f, 0.45f, 0.50f, 1.0f);
        renderer::Color tierFillColor   = renderer::Color::Premultiplied(0.12f, 0.15f, 0.18f, 1.0f);
        if (amp->tier == AmplitudeTier::Harmonic) {
            tierBorderColor = renderer::Color::Premultiplied(0.25f, 0.85f, 1.00f, 1.0f);
            tierFillColor   = renderer::Color::Premultiplied(0.08f, 0.30f, 0.45f, 1.0f);
        } else if (amp->tier == AmplitudeTier::Crescendo) {
            tierBorderColor = renderer::Color::Premultiplied(1.00f, 0.85f, 0.20f, 1.0f);
            tierFillColor   = renderer::Color::Premultiplied(0.50f, 0.35f, 0.05f, 1.0f);
        }
        ctx.renderList.PushRect(renderer::FRect{232.0f, 24.0f, 36.0f, 36.0f}, tierBorderColor);
        ctx.renderList.PushRect(renderer::FRect{234.0f, 26.0f, 32.0f, 32.0f}, tierFillColor);

        // 3 Triad Wicks (Lead + Wing Left + Wing Right)
        const auto& wicks = lantern->triadWicks;
        // Lead Wick (Slot 0 - Active Weapon)
        const renderer::Color leadWickCol = GetEchoColor(wicks[0]);
        ctx.renderList.PushRect(renderer::FRect{23.0f, 65.0f, 30.0f, 30.0f}, renderer::Color::Premultiplied(1.0f, 1.0f, 1.0f, 0.9f));
        ctx.renderList.PushRect(renderer::FRect{25.0f, 67.0f, 26.0f, 26.0f}, leadWickCol);

        // Wing Left Assist (Slot 1 - Key [Q])
        const renderer::Color wingLCol = GetEchoColor(wicks[1]);
        ctx.renderList.PushRect(renderer::FRect{59.0f, 69.0f, 24.0f, 24.0f}, renderer::Color::Premultiplied(0.4f, 0.5f, 0.6f, 0.8f));
        ctx.renderList.PushRect(renderer::FRect{61.0f, 71.0f, 20.0f, 20.0f}, wingLCol);

        // Wing Right Assist (Slot 2 - Key [E])
        const renderer::Color wingRCol = GetEchoColor(wicks[2]);
        ctx.renderList.PushRect(renderer::FRect{89.0f, 69.0f, 24.0f, 24.0f}, renderer::Color::Premultiplied(0.4f, 0.5f, 0.6f, 0.8f));
        ctx.renderList.PushRect(renderer::FRect{91.0f, 71.0f, 20.0f, 20.0f}, wingRCol);
    }

    // --- Boss HUD (Top-Center / Top-Right) ---
    if (bossStats != nullptr) {
        // Boss HP Bar (Crimson)
        const f32 bossHpRatio = bossStats->maxHp > 0.0f ? (bossStats->hp / bossStats->maxHp) : 0.0f;
        PushFramedBar(
            ctx.renderList,
            420.0f, 24.0f, 480.0f, 18.0f,
            bossHpRatio,
            renderer::Color::Premultiplied(0.92f, 0.15f, 0.20f, 1.0f),
            renderer::Color::Premultiplied(0.60f, 0.20f, 0.25f, 0.9f));

        // Boss Break / Stagger Meter (Amber)
        const f32 bossBreakRatio = bossStats->maxBreakMeter > 0.0f ? (bossStats->breakMeter / bossStats->maxBreakMeter) : 0.0f;
        PushFramedBar(
            ctx.renderList,
            420.0f, 45.0f, 480.0f, 7.0f,
            bossBreakRatio,
            renderer::Color::Premultiplied(1.00f, 0.65f, 0.15f, 1.0f),
            renderer::Color::Premultiplied(0.40f, 0.30f, 0.15f, 0.8f));

        // Boss Posture Icon
        renderer::Color postureColor = renderer::Color::Premultiplied(0.5f, 0.5f, 0.5f, 1.0f); // Neutral
        if (bossStats->posture == BossPosture::Dominant) {
            postureColor = renderer::Color::Premultiplied(1.00f, 0.15f, 0.15f, 1.0f); // Dominant
        } else if (bossStats->posture == BossPosture::Submissive) {
            postureColor = renderer::Color::Premultiplied(0.20f, 0.95f, 1.00f, 1.0f); // Submissive (Staggered)
        }
        ctx.renderList.PushRect(renderer::FRect{908.0f, 24.0f, 28.0f, 28.0f}, postureColor);

        // Active Standing-Wave Elemental Mark on Boss
        if (bossWave != nullptr && bossWave->hasActiveWaveMark) {
            const renderer::Color markCol = GetEchoColor(bossWave->markedEcho);
            ctx.renderList.PushRect(renderer::FRect{420.0f, 56.0f, 120.0f, 14.0f}, renderer::Color::Premultiplied(0.1f, 0.1f, 0.15f, 0.9f));
            ctx.renderList.PushRect(renderer::FRect{422.0f, 58.0f, 10.0f, 10.0f}, markCol);
            ctx.renderList.PushRect(renderer::FRect{436.0f, 60.0f, 100.0f, 6.0f}, markCol);
        }
    }

    // --- Controls Legend Bar (Bottom-Screen) ---
    ctx.renderList.PushRect(
        renderer::FRect{200.0f, 686.0f, 880.0f, 26.0f},
        renderer::Color::Premultiplied(0.04f, 0.06f, 0.09f, 0.88f));
    ctx.renderList.PushRect(
        renderer::FRect{200.0f, 685.0f, 880.0f, 1.0f},
        renderer::Color::Premultiplied(0.25f, 0.45f, 0.65f, 0.7f));

    struct KeyBadge {
        f32 x;
        f32 w;
        renderer::Color badgeColor;
        renderer::Color coreColor;
    };
    static constexpr std::array<KeyBadge, 8> kBadges{{
        {215.0f, 80.0f, renderer::Color::Premultiplied(0.20f, 0.40f, 0.60f, 0.8f), renderer::Color::Premultiplied(0.40f, 0.70f, 1.00f, 0.9f)}, // [A/D] Move
        {310.0f, 85.0f, renderer::Color::Premultiplied(0.20f, 0.50f, 0.35f, 0.8f), renderer::Color::Premultiplied(0.35f, 0.90f, 0.55f, 0.9f)}, // [Space] Jump
        {410.0f, 75.0f, renderer::Color::Premultiplied(0.35f, 0.30f, 0.60f, 0.8f), renderer::Color::Premultiplied(0.65f, 0.50f, 1.00f, 0.9f)}, // [K] Dash
        {500.0f, 80.0f, renderer::Color::Premultiplied(0.65f, 0.25f, 0.20f, 0.8f), renderer::Color::Premultiplied(1.00f, 0.45f, 0.35f, 0.9f)}, // [J] Attack
        {595.0f, 80.0f, renderer::Color::Premultiplied(0.15f, 0.55f, 0.65f, 0.8f), renderer::Color::Premultiplied(0.25f, 0.95f, 1.00f, 0.95f)}, // [L] Parry
        {690.0f, 75.0f, renderer::Color::Premultiplied(0.55f, 0.45f, 0.15f, 0.8f), renderer::Color::Premultiplied(0.95f, 0.80f, 0.25f, 0.9f)}, // [I] Shift
        {780.0f, 85.0f, renderer::Color::Premultiplied(0.20f, 0.50f, 0.55f, 0.8f), renderer::Color::Premultiplied(0.30f, 0.90f, 0.95f, 0.9f)}, // [Q/E] Wings
        {880.0f, 95.0f, renderer::Color::Premultiplied(0.55f, 0.20f, 0.50f, 0.8f), renderer::Color::Premultiplied(0.95f, 0.35f, 0.85f, 0.9f)}, // [U/O] Unisons
    }};

    for (const auto& b : kBadges) {
        ctx.renderList.PushRect(renderer::FRect{b.x, 690.0f, b.w, 18.0f}, b.badgeColor);
        ctx.renderList.PushRect(renderer::FRect{b.x + 2.0f, 692.0f, b.w - 4.0f, 14.0f}, renderer::Color::Premultiplied(0.06f, 0.08f, 0.12f, 0.9f));
        ctx.renderList.PushRect(renderer::FRect{b.x + 6.0f, 695.0f, b.w - 12.0f, 8.0f}, b.coreColor);
    }
}

} // namespace en::veil

#pragma once

#include "world/WorldTypes.h"
#include "world/World.h"
#include "game/core/Types.h"

#include <algorithm>
#include <array>

namespace en::veil {

// Component ID 24: Kaelen's ancient Resonance Lantern and active Triad wicks.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: triadWicks contains exactly 3 valid EchoId values.
struct alignas(16) ResonanceLanternComponent {
    static constexpr u8 kTypeId = 24;

    std::array<EchoId, kTriadDeckSize> triadWicks{EchoId::Ignis, EchoId::Glacies, EchoId::Fulgur};
    u16 unlockedWicksMask{0x0FFFu};
    f32 harmonyGauge{0.0f};
    bool lanternRestSiteOpen{false};
    u8 reserved0{0};
    u16 reserved1{0};

    [[nodiscard]] constexpr EchoId LeadEcho() const noexcept { return triadWicks[0]; }
    [[nodiscard]] constexpr EchoId WingLeftEcho() const noexcept { return triadWicks[1]; }
    [[nodiscard]] constexpr EchoId WingRightEcho() const noexcept { return triadWicks[2]; }

    constexpr void RotateTriadForward() noexcept {
        const EchoId prevLead = triadWicks[0];
        triadWicks[0] = triadWicks[1];
        triadWicks[1] = triadWicks[2];
        triadWicks[2] = prevLead;
    }
};
static_assert(sizeof(ResonanceLanternComponent) == 16 && alignof(ResonanceLanternComponent) == 16);
static_assert(world::Component<ResonanceLanternComponent>);

// Component ID 25: Amplitude Ascension weapon power tier, wave energy, and decay.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: waveEnergy is clamped to [0.0f, 100.0f].
struct alignas(16) AmplitudeAscensionComponent {
    static constexpr u8 kTypeId = 25;

    AmplitudeTier tier{AmplitudeTier::Fundamental};
    u8 reserved0{0};
    u16 tierDecayTicksRemaining{0};
    f32 waveEnergy{0.0f};
    bool crescendoBurstPending{false};
    u8 reserved1{0};
    u16 reserved2{0};
    u32 reserved3{0};

    [[nodiscard]] constexpr f32 ReachMultiplier() const noexcept {
        switch (tier) {
            case AmplitudeTier::Harmonic:  return 1.20f;
            case AmplitudeTier::Crescendo: return 1.40f;
            default:                       return 1.00f;
        }
    }

    [[nodiscard]] constexpr f32 DamageMultiplier() const noexcept {
        switch (tier) {
            case AmplitudeTier::Harmonic:  return 1.15f;
            case AmplitudeTier::Crescendo: return 1.35f;
            default:                       return 1.00f;
        }
    }

    constexpr bool PromoteOnShift(f32 threshold = 30.0f, u16 decayTicks = 480) noexcept {
        if (waveEnergy >= threshold) {
            waveEnergy -= threshold;
            if (tier == AmplitudeTier::Fundamental) {
                tier = AmplitudeTier::Harmonic;
            } else if (tier == AmplitudeTier::Harmonic) {
                tier = AmplitudeTier::Crescendo;
            }
            tierDecayTicksRemaining = decayTicks;
            return true;
        }
        return false;
    }
};
static_assert(sizeof(AmplitudeAscensionComponent) == 16 && alignof(AmplitudeAscensionComponent) == 16);
static_assert(world::Component<AmplitudeAscensionComponent>);

// Component ID 26: Active Standing-Wave elemental/phenomenon mark, armor shred, and status.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: markTicksRemaining == 0 implies !hasActiveWaveMark.
struct alignas(32) StandingWaveComponent {
    static constexpr u8 kTypeId = 26;

    f32 armorShredRatio{0.0f};
    f32 delayedDamagePool{0.0f};
    u32 totalInterferencesTriggered{0};
    u16 markTicksRemaining{0};
    u16 armorShredTicksRemaining{0};
    u16 stunTicksRemaining{0};
    u16 delayedBurstTicksRemaining{0};
    EchoId markedEcho{EchoId::Ignis};
    WavePolarity markedPolarity{WavePolarity::Oscillation};
    WaveReactionId lastTriggeredReaction{WaveReactionId::None};
    bool hasActiveWaveMark{false};
    bool sympatheticLinked{false};
    u8 reserved0{0};
    u16 reserved1{0};
};
static_assert(sizeof(StandingWaveComponent) == 32 && alignof(StandingWaveComponent) == 32);
static_assert(world::Component<StandingWaveComponent>);

// Component ID 27: Phase-Inversion Counter parry timing, counter echo match, and audit metrics.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: Tracks single-frame and multi-frame 180-degree counter windows.
struct alignas(16) PhaseInversionCounterComponent {
    static constexpr u8 kTypeId = 27;

    u32 totalCountersLanded{0};
    u16 parryActiveTicks{0};
    u16 counterWindowTicks{0};
    EchoId lastCounterEcho{EchoId::Glacies};
    bool phaseCounterSucceeded{false};
    u8 reserved0{0};
    u8 reserved1{0};
    u32 reserved2{0};
};
static_assert(sizeof(PhaseInversionCounterComponent) == 16 && alignof(PhaseInversionCounterComponent) == 16);
static_assert(world::Component<PhaseInversionCounterComponent>);

// Component ID 28: Platformer Kinematics and Lantern Traversal Mutation physics state.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: Coordinates with KinematicMotor2D without duplicating velocity integration.
struct alignas(32) TraversalMutationComponent {
    static constexpr u8 kTypeId = 28;

    f32 temporalAnchorX{0.0f};
    f32 temporalAnchorY{0.0f};
    u16 traversalActiveTicks{0};
    u16 traversalCooldownTicks{0};
    u16 invulnerableTicks{0};
    u16 coyoteTicksRemaining{0};
    u16 jumpBufferTicksRemaining{0};
    u8 airJumpsRemaining{1};
    u8 airDashesRemaining{1};
    TraversalType activeTraversal{TraversalType::BlazeJetDash};
    bool hasTemporalAnchor{false};
    bool facingRight{true};
    u8 reserved0{0};
    u32 reserved1{0};
};
static_assert(sizeof(TraversalMutationComponent) == 32 && alignof(TraversalMutationComponent) == 32);
static_assert(world::Component<TraversalMutationComponent>);

// Component ID 29: Health, Break meter, Armor ratio, and Boss posture.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: hp is clamped to [0.0f, maxHp].
struct alignas(32) VeilCombatStatsComponent {
    static constexpr u8 kTypeId = 29;

    f32 hp{1000.0f};
    f32 maxHp{1000.0f};
    f32 breakMeter{0.0f};
    f32 maxBreakMeter{500.0f};
    f32 baseArmorRatio{0.25f};
    u16 submissiveTicksRemaining{0};
    BossPosture posture{BossPosture::Neutral};
    bool alive{true};
    u32 reserved{0};
};
static_assert(sizeof(VeilCombatStatsComponent) == 32 && alignof(VeilCombatStatsComponent) == 32);
static_assert(world::Component<VeilCombatStatsComponent>);

// Component ID 30: Combat FSM action frame ticks, combo branching, and Stridor hit tracking.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: comboStep advances deterministically on hit confirmation.
struct alignas(16) VeilActionStateComponent {
    static constexpr u8 kTypeId = 30;

    u16 actionTick{0};
    ActionId currentAction{ActionId::Idle};
    u8 comboStep{0};
    u8 stridorHitCounter{0};
    bool hitConfirmedThisAction{false};
    u16 reserved0{0};
    u64 reserved1{0};
};
static_assert(sizeof(VeilActionStateComponent) == 16 && alignof(VeilActionStateComponent) == 16);
static_assert(world::Component<VeilActionStateComponent>);

// Component ID 31: Boss Conductor Glint-and-Tone telegraphs and Symphony Phase Shifts.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: telegraphTicksRemaining == 0 implies activeTelegraph == None.
struct alignas(16) BossConductorComponent {
    static constexpr u8 kTypeId = 31;

    u32 phaseInversionsSuffered{0};
    u16 telegraphTicksRemaining{0};
    EchoId bossEchoAffinity{EchoId::Terra};
    EchoId requiredCounterEcho{EchoId::Fulgur};
    BossTelegraphKind activeTelegraph{BossTelegraphKind::None};
    u8 symphonyMovementPhase{1};
    u16 actionTimerTicks{0};
    u8 attackStrikeTicks{0};
    u8 reserved0{0};
    u16 reserved1{0};
};
static_assert(sizeof(BossConductorComponent) == 16 && alignof(BossConductorComponent) == 16);
static_assert(world::Component<BossConductorComponent>);

// Component ID 32: Metroidvania progression, Tuning Shards, and Lost Wick Husk.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: maxHpLockRatio in [0.0f, 0.25f].
struct alignas(32) WorldProgressComponent {
    static constexpr u8 kTypeId = 32;

    f32 lostWickX{0.0f};
    f32 lostWickY{0.0f};
    f32 maxHpLockRatio{0.0f};
    u32 tuningShards{250};
    u32 bankedShards{0};
    u32 shardsInLostWick{0};
    u32 unlockedBiomesMask{0x0001u};
    u32 solvedFrequencyGatesMask{0u};
    u8 collectedTuningForks{0};
    bool hasLostWickHusk{false};
    u16 reserved0{0};
};
static_assert(sizeof(WorldProgressComponent) == 64 && alignof(WorldProgressComponent) == 32);
static_assert(world::Component<WorldProgressComponent>);

// Component ID 33: Off-hand Wing Echo assist cooldowns and zero-recovery assist state.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: Cooldowns decrement at 120 Hz down to 0.
struct alignas(16) WingAssistComponent {
    static constexpr u8 kTypeId = 33;

    u16 wingLeftCooldownTicks{0};
    u16 wingRightCooldownTicks{0};
    EchoId activeWingAssist{EchoId::Glacies};
    u8 assistActiveTicks{0};
    u16 reserved0{0};
    u64 reserved1{0};
};
static_assert(sizeof(WingAssistComponent) == 16 && alignof(WingAssistComponent) == 16);
static_assert(world::Component<WingAssistComponent>);

// Component ID 34: Bio-Sympathetic Pulse tether linking enemy heartbeats (Aether).
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: damageShareRatio is 0.45f when active.
struct alignas(16) SympatheticTetherComponent {
    static constexpr u8 kTypeId = 34;

    f32 damageShareRatio{0.45f};
    u16 linkedEntityIndex{0};
    u16 linkedEntityGeneration{0};
    u16 tetherTicksRemaining{0};
    bool active{false};
    u8 reserved0{0};
    u32 reserved1{0};
};
static_assert(sizeof(SympatheticTetherComponent) == 16 && alignof(SympatheticTetherComponent) == 16);
static_assert(world::Component<SympatheticTetherComponent>);

// Component ID 35: Polyphonic Unison and Sacred Chord ultimate execution.
// Trivially copyable ECS value type; lifetime managed by World ComponentPool.
// Invariant: chordCastTicksRemaining counts down during active channel.
struct alignas(16) SacredChordComponent {
    static constexpr u8 kTypeId = 35;

    SacredChordId activeChord{SacredChordId::None};
    u16 chordCastTicksRemaining{0};
    f32 unisonDamageMultiplier{1.0f};
    f32 unisonBreakMultiplier{1.0f};
    u32 reserved{0};
};
static_assert(sizeof(SacredChordComponent) == 16 && alignof(SacredChordComponent) == 16);
static_assert(world::Component<SacredChordComponent>);

// Registers all custom client game components (IDs 24..35) into World ComponentPools.
// Contiguous arena slots allocated during AppHooks boot; zero allocation post-init.
[[nodiscard]] inline Result<void, u8> RegisterGameComponents(world::World& world) noexcept {
    if (!world.RegisterComponentPool<ResonanceLanternComponent>(16) ||
        !world.RegisterComponentPool<AmplitudeAscensionComponent>(16) ||
        !world.RegisterComponentPool<StandingWaveComponent>(64) ||
        !world.RegisterComponentPool<PhaseInversionCounterComponent>(16) ||
        !world.RegisterComponentPool<TraversalMutationComponent>(16) ||
        !world.RegisterComponentPool<VeilCombatStatsComponent>(64) ||
        !world.RegisterComponentPool<VeilActionStateComponent>(16) ||
        !world.RegisterComponentPool<BossConductorComponent>(16) ||
        !world.RegisterComponentPool<WorldProgressComponent>(16) ||
        !world.RegisterComponentPool<WingAssistComponent>(16) ||
        !world.RegisterComponentPool<SympatheticTetherComponent>(64) ||
        !world.RegisterComponentPool<SacredChordComponent>(16)) [[unlikely]] {
        return Err(u8{1});
    }
    return {};
}

} // namespace en::veil

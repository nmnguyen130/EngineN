#pragma once

#include "gameplay/GameplayTypes.h"
#include "gameplay/KinematicSolver2D.h"
#include "gameplay/SpatialIndex2D.h"
#include "gameplay/StateAndCombatSolver.h"

#include <array>
#include <span>

namespace en::gameplay {

enum class SimCueKind : u8 {
    AudioOneShot = 0,
    SpawnPrefabEffect,
    CameraTraumaPulse,
    GamepadRumblePulse,
    CustomGameEvent
};

// Compact 24-byte outgoing presentation cue emitted during 120 Hz simulation ticks.
struct alignas(8) SimCue {
    assets::AssetId assetId{};
    f32 worldX{0.0f};
    f32 worldY{0.0f};
    u16 magnitude_q16{0};
    u16 durationTicks{0};
    SimCueKind kind{SimCueKind::AudioOneShot};
    u8 channel{0};
    u16 userTag{0};
};
static_assert(sizeof(SimCue) == 24);

struct GameplayPipelineConfig {
    usize arenaBytes{512u * 1024u};
    u16 maxMotors{512};
    u16 maxCarriers{64};
    u16 maxHitboxRigs{512};
    u16 maxStateMachines{256};
    u16 maxDamageReceivers{512};
    u16 maxBreadcrumbs{16};
    u16 maxCameras{4};
    KinematicStepConfig kinematicConfig{};
};

// Top-level 120 Hz gameplay mechanism orchestrator, camera director, and persistent world-flag container.
// Move-only RAII owner of a single 512 KB pre-allocated arena slab; zero dynamic allocation during Tick().
// Invariant: Tick() mutates structural entity state strictly via World::Defer*() and never allocates memory.
class GameplayPipeline {
public:
    static constexpr u32 kMaxWorldFlags = 4096;
    static constexpr u32 kMaxCuesPerFrame = 256;
    static constexpr u32 kMaxHitsPerTick = 256;

    GameplayPipeline() = default;
    ~GameplayPipeline() noexcept;

    GameplayPipeline(const GameplayPipeline&) = delete;
    GameplayPipeline& operator=(const GameplayPipeline&) = delete;
    GameplayPipeline(GameplayPipeline&& other) noexcept;
    GameplayPipeline& operator=(GameplayPipeline&& other) noexcept;

    [[nodiscard]] Result<void, GameplayErrc> Init(
        world::World& world,
        const GameplayPolicyTable& policies = {},
        const GameplayPipelineConfig& cfg = {}) noexcept;

    void Shutdown() noexcept;

    void SetPolicyTable(const GameplayPolicyTable& policies) noexcept {
        m_policies = policies;
    }

    [[nodiscard]] Result<void, GameplayErrc> SetActiveRoomCollision(
        const assets::AssetBank& bank,
        assets::BlobHandle collisionMapBlob) noexcept;

    void ClearActiveRoomCollision() noexcept {
        m_spatial.ClearCollisionMap();
    }

    void Tick(
        world::World& world,
        const assets::AssetBank& assetBank,
        time::SimStepView step,
        const platform::input::TickInput& tickInput,
        platform::input::ActionBuffer& actionBuffer) noexcept;

    void EmitCue(const SimCue& cue) noexcept {
        if (m_cueBuffer != nullptr && m_cueCount < kMaxCuesPerFrame) {
            m_cueBuffer[m_cueCount++] = cue;
        }
    }

    [[nodiscard]] constexpr std::span<const SimCue> DrainCues() noexcept {
        const std::span<const SimCue> ready{m_cueBuffer, m_cueCount};
        m_cueCount = 0;
        return ready;
    }

    constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept {
        if (flagId < kMaxWorldFlags) {
            const u32 word = static_cast<u32>(flagId) >> 6u;
            const u64 bit  = 1ULL << (flagId & 63u);
            if (value) m_worldFlags[word] |= bit;
            else       m_worldFlags[word] &= ~bit;
        }
    }

    [[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept {
        if (flagId >= kMaxWorldFlags) return false;
        return (m_worldFlags[static_cast<u32>(flagId) >> 6u] & (1ULL << (flagId & 63u))) != 0ULL;
    }

    constexpr void ClearWorldFlags() noexcept {
        m_worldFlags.fill(0ULL);
    }

    [[nodiscard]] constexpr const SpatialIndex2D& Spatial() const noexcept { return m_spatial; }
    [[nodiscard]] u64 ComputeAuditHash(const world::World& world) const noexcept;

private:
    static void StepCameraControllers(
        world::World& world,
        time::SimStepView step) noexcept;

    u8* m_arena{nullptr};
    usize m_arenaBytes{0};
    GameplayPolicyTable m_policies{};
    KinematicStepConfig m_kinematicCfg{};
    SpatialIndex2D m_spatial{};
    ResolvedHitEvent* m_hitScratch{nullptr};
    SimCue* m_cueBuffer{nullptr};
    u32 m_cueCount{0};
    std::array<u64, 64> m_worldFlags{};
};

} // namespace en::gameplay

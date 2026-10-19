#pragma once

#include "gameplay/GameplayTypes.h"
#include "gameplay/KinematicSolver2D.h"
#include "gameplay/SpatialIndex2D.h"
#include "gameplay/StateAndCombatSolver.h"

#include <array>
#include <bit>
#include <span>

namespace en::gameplay {

struct GameplayPipelineConfig {
    usize arenaBytes{512u * 1024u};
    u16 maxMotors{512};
    u16 maxCarriers{64};
    u16 maxHitboxRigs{512};
    u16 maxStateMachines{256};
    u16 maxDamageReceivers{512};
    u16 maxBreadcrumbs{16};
    u16 maxCameras{4};
    f32 pixelsPerMeter{64.0f};
    KinematicStepConfig kinematicConfig{};
};

// Top-level 120 Hz gameplay mechanism orchestrator, camera director, and persistent world-flag container.
// Move-only RAII owner of a single 64B-aligned pre-allocated arena slab; zero dynamic allocation during Tick().
// Cache Line 0 (offset 0..63): Hot policy pointers, kinematic config, scale, and scratch buffer pointers.
// Cache Line 1 (offset 64..127): 64B SpatialIndex2D accelerator.
// Invariant: Never calls AudioSystem, Renderer, or DebugSystem directly; communicates strictly via DrainCues() and World ECS.
class alignas(64) GameplayPipeline final {
public:
    static constexpr u32 kMaxWorldFlags   = 4096;
    static constexpr u32 kMaxCuesPerFrame = 256;
    static constexpr u32 kMaxHitsPerTick  = 256;

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

    constexpr void SetPolicyTable(const GameplayPolicyTable& policies) noexcept {
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
        const input::TickInput& tickInput,
        input::ActionBuffer& actionBuffer) noexcept;

    constexpr void EmitCue(const SimCue& cue) noexcept {
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
            if (value) {
                m_worldFlags[word]    |= bit;
                m_activeFlagWordsMask |= (1ULL << word);
            } else {
                m_worldFlags[word] &= ~bit;
                if (m_worldFlags[word] == 0ULL) {
                    m_activeFlagWordsMask &= ~(1ULL << word);
                }
            }
        }
    }

    [[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept {
        if (flagId >= kMaxWorldFlags) return false;
        return (m_worldFlags[static_cast<u32>(flagId) >> 6u] & (1ULL << (flagId & 63u))) != 0ULL;
    }

    constexpr void ClearWorldFlags() noexcept {
        for (u64 m = m_activeFlagWordsMask; m != 0ULL; m &= (m - 1ULL)) {
            m_worldFlags[std::countr_zero(m)] = 0ULL;
        }
        m_activeFlagWordsMask = 0ULL;
    }

    [[nodiscard]] constexpr const SpatialIndex2D& Spatial() const noexcept { return m_spatial; }
    [[nodiscard]] u64 ComputeAuditHash(const world::World& world) const noexcept;

private:
    void StepCameraControllers(
        world::World& world,
        time::SimStepView step) noexcept;

    // Cache Line 0 (64B exact): Hot 120 Hz dispatch state & scratch pointers
    GameplayPolicyTable m_policies{};
    KinematicStepConfig m_kinematicCfg{};
    f32 m_invPixelsPerMeter{1.0f / 64.0f};
    ResolvedHitEvent* m_hitScratch{nullptr};
    SimCue* m_cueBuffer{nullptr};

    // Cache Line 1 (64B exact): Spatial accelerator
    SpatialIndex2D m_spatial{};

    // Cache Line 2 (64B): Arena ownership, cue count, and sparse world-flag summary bitmask
    u8* m_arena{nullptr};
    usize m_arenaBytes{0};
    u64 m_activeFlagWordsMask{0};
    u32 m_cueCount{0};

    // Cache Lines 3..10 (512B): 4096-bit world flag storage
    alignas(64) std::array<u64, 64> m_worldFlags{};
};
static_assert(sizeof(GameplayPipeline) == 704);

} // namespace en::gameplay

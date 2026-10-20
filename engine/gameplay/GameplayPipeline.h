#pragma once

#include "gameplay/AiAndNavigation.h"
#include "gameplay/CombatAndStateSolver.h"
#include "gameplay/ParticleAndCamera.h"
#include "physics/KinematicSolver2D.h"

namespace en {

struct GameplayPipelineConfig {
    usize arenaBytes{2 * 1024 * 1024};
    u16 maxMapWidth{256};
    u16 maxMapHeight{128};
    u32 maxParticles{2048};
    KinematicStepConfig kinematics{};
};

// Authoritative 120 Hz gameplay, physics, combat, particle, camera, and world-flag coordinator.
class alignas(64) GameplayPipeline {
public:
    static constexpr usize kMaxCues = 128;
    static constexpr usize kMaxHitScratch = 128;

    GameplayPipeline() noexcept = default;
    ~GameplayPipeline() noexcept { Shutdown(); }

    GameplayPipeline(const GameplayPipeline&) = delete;
    GameplayPipeline& operator=(const GameplayPipeline&) = delete;

    [[nodiscard]] Result<void, WorldErrc> Init(
        World& world,
        const GameplayPolicyTable& policies = {},
        const GameplayPipelineConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;

    constexpr void SetPolicyTable(const GameplayPolicyTable& policies) noexcept { m_policies = policies; }
    [[nodiscard]] Result<void, WorldErrc> SetActiveRoomCollision(
        const AssetBank& bank, BlobHandle collisionMapBlob) noexcept;

    void Tick(
        World& world,
        const AssetBank& assetBank,
        const SimStepView& step,
        const TickInput& tickInput,
        ActionBuffer& actionBuffer) noexcept;

    constexpr void EmitCue(const SimCue& cue) noexcept {
        if (m_cueCount < kMaxCues) m_cueBuffer[m_cueCount++] = cue;
    }
    [[nodiscard]] std::span<const SimCue> DrainCues() noexcept {
        const u32 count = m_cueCount;
        m_cueCount = 0;
        return m_cueBuffer.subspan(0, count);
    }

    constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept {
        if (flagId >= 4096U) return;
        const u16 w = flagId >> 6U;
        const u64 bit = (1ULL << (flagId & 63U));
        if (value) {
            m_worldFlags[w] |= bit;
            m_activeFlagWordsMask |= (1ULL << w);
        } else {
            m_worldFlags[w] &= ~bit;
            if (m_worldFlags[w] == 0ULL) m_activeFlagWordsMask &= ~(1ULL << w);
        }
    }

    [[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept {
        if (flagId >= 4096U) return false;
        return (m_worldFlags[flagId >> 6U] & (1ULL << (flagId & 63U))) != 0ULL;
    }

    constexpr void ClearWorldFlags() noexcept {
        u64 m = m_activeFlagWordsMask;
        while (m != 0ULL) {
            const u8 w = static_cast<u8>(std::countr_zero(m));
            m &= (m - 1ULL);
            m_worldFlags[w] = 0ULL;
        }
        m_activeFlagWordsMask = 0ULL;
    }

    [[nodiscard]] constexpr SpatialIndex2D& Spatial() noexcept { return m_spatial; }
    [[nodiscard]] constexpr const SpatialIndex2D& Spatial() const noexcept { return m_spatial; }
    [[nodiscard]] constexpr ParticlePool2D& Particles() noexcept { return m_particles; }
    [[nodiscard]] constexpr const ParticlePool2D& Particles() const noexcept { return m_particles; }
    [[nodiscard]] constexpr KinematicStepConfig& KinematicConfig() noexcept { return m_kinematicCfg; }
    [[nodiscard]] constexpr std::span<const u64, 64> RawWorldFlags() const noexcept { return m_worldFlags; }
    void LoadRawWorldFlags(std::span<const u64, 64> words) noexcept;

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    GameplayPolicyTable m_policies{};
    KinematicStepConfig m_kinematicCfg{};
    std::span<ResolvedHitEvent> m_hitScratch{};
    std::span<SimCue> m_cueBuffer{};
    u64 m_activeFlagWordsMask{0};
    u32 m_cueCount{0};

    SpatialIndex2D m_spatial{};
    ParticlePool2D m_particles{};
    AlignedArena m_arena{};
    alignas(64) std::array<u64, 64> m_worldFlags{};
};

} // namespace en

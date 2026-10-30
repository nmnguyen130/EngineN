#pragma once

#include "core/Memory.h"
#include "gameplay/CameraDirector2D.h"
#include "gameplay/MotionSolver2D.h"
#include "gameplay/StateAndCombatSolver2D.h"

namespace en {

class alignas(64) GameplayPipeline {
public:
    static constexpr u32 kWorldFlagWords = 64U;
    static constexpr u32 kMaxWorldFlags = kWorldFlagWords * 64U;

    GameplayPipeline() noexcept = default;
    ~GameplayPipeline() noexcept { Shutdown(); }

    GameplayPipeline(const GameplayPipeline&) = delete;
    GameplayPipeline& operator=(const GameplayPipeline&) = delete;
    GameplayPipeline(GameplayPipeline&&) = delete;
    GameplayPipeline& operator=(GameplayPipeline&&) = delete;

    [[nodiscard]] Result<void, GameplayErrc> Init(
        World& world,
        const GameplayPolicyTable& policies = {},
        const GameplayConfig& cfg = {}) noexcept;

    void Shutdown() noexcept;
    void Clear() noexcept;

    void Tick(World& world, PhysicsSystem& physics, const AssetBank& assetBank,
              const SimStepView& step, const TickInput& input, ActionBuffer& actions) noexcept;

    constexpr void EmitCue(const SimCue& cue) noexcept {
        if (m_cues && m_cueCount < m_cfg.maxCuesPerTick) {
            m_cues[m_cueCount++] = cue;
        }
    }

    [[nodiscard]] constexpr std::span<const SimCue> DrainCues() noexcept {
        const std::span<const SimCue> out{m_cues, m_cueCount};
        m_cueCount = 0U;
        return out;
    }

    [[nodiscard]] constexpr std::span<const SimCue> PeekCues() const noexcept {
        return {m_cues, m_cueCount};
    }

    [[nodiscard]] constexpr std::span<const CombatHitEvent> LatestCombatHits() const noexcept {
        return {m_hitEvents, m_hitEventCount};
    }

    constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept {
        if (flagId >= kMaxWorldFlags) return;
        const u32 wordIdx = static_cast<u32>(flagId >> 6U);
        const u64 bit = (1ULL << (flagId & 63U));
        if (value) {
            m_worldFlags[wordIdx] |= bit;
            m_activeFlagWordsMask |= (1ULL << wordIdx);
        } else {
            m_worldFlags[wordIdx] &= ~bit;
            if (m_worldFlags[wordIdx] == 0ULL) {
                m_activeFlagWordsMask &= ~(1ULL << wordIdx);
            }
        }
    }

    [[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept {
        if (flagId >= kMaxWorldFlags) return false;
        return (m_worldFlags[flagId >> 6U] & (1ULL << (flagId & 63U))) != 0ULL;
    }

    constexpr void ClearWorldFlags() noexcept {
        u64 mask = m_activeFlagWordsMask;
        while (mask != 0ULL) {
            const u32 w = static_cast<u32>(std::countr_zero(mask));
            mask &= (mask - 1ULL);
            m_worldFlags[w] = 0ULL;
        }
        m_activeFlagWordsMask = 0ULL;
    }

    [[nodiscard]] constexpr const GameplayPolicyTable& PolicyTable() const noexcept { return m_policies; }
    constexpr void SetPolicyTable(const GameplayPolicyTable& policies) noexcept { m_policies = policies; }

    [[nodiscard]] constexpr const GameplayConfig& Config() const noexcept { return m_cfg; }
    constexpr void SetConfig(const GameplayConfig& cfg) noexcept { m_cfg = cfg; }

    [[nodiscard]] constexpr u64 TickCount() const noexcept { return m_tickCount; }
    [[nodiscard]] constexpr u64 TotalCombatHits() const noexcept { return m_totalCombatHits; }
    [[nodiscard]] constexpr u64 ActiveFlagWordsMask() const noexcept { return m_activeFlagWordsMask; }

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    GameplayConfig m_cfg{};
    GameplayPolicyTable m_policies{};
    SimCue* m_cues{nullptr};
    CombatHitEvent* m_hitEvents{nullptr};
    AlignedArena m_arena{};
    u64 m_activeFlagWordsMask{0ULL};
    u64 m_tickCount{0ULL};
    u32 m_totalCombatHits{0U};
    u16 m_cueCount{0U};
    u16 m_hitEventCount{0U};
    alignas(64) std::array<u64, kWorldFlagWords> m_worldFlags{};
};
static_assert(sizeof(GameplayPipeline) == 640);

} // namespace en

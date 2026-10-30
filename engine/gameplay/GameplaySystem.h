#pragma once

#include "core/Memory.h"
#include "gameplay/GameplayTypes.h"
#include "physics/PhysicsSystem.h"

namespace en {

// Deterministic 2D Metroidvania gameplay pipeline orchestrating character locomotion, FSMs, combat, and cameras.
// Move-disabled owner of an Arena; exposes a 4096-bit progression flag table and cue/combat event buffers.
// Invariant: sizeof(GameplaySystem) == 616, and zero heap allocations occur after Init.
class GameplaySystem {
public:
    static constexpr u32 kFlagWords = 64U;
    static constexpr u32 kMaxFlags  = kFlagWords * 64U;

    GameplaySystem() noexcept = default;
    ~GameplaySystem() noexcept { Shutdown(); }

    GameplaySystem(const GameplaySystem&) = delete;
    GameplaySystem& operator=(const GameplaySystem&) = delete;

    [[nodiscard]] Result<void, GameplayErrc> Init(
        World& world,
        const GameplayConfig& cfg = {},
        const GameplayCallbacks& callbacks = {}) noexcept;
    void Shutdown() noexcept;
    void Clear() noexcept;

    void Step(
        World& world,
        PhysicsSystem& physics,
        const TickStep& step,
        const TickInput& input,
        ActionBuffer& actions) noexcept;

    constexpr void EmitCue(const GameplayCue& cue) noexcept {
        if (m_cues && m_cueCount < m_cfg.maxCuesPerTick) {
            m_cues[m_cueCount++] = cue;
        }
    }

    [[nodiscard]] constexpr std::span<const GameplayCue> DrainCues() noexcept {
        const std::span<const GameplayCue> out{m_cues, m_cueCount};
        m_cueCount = 0U;
        return out;
    }

    [[nodiscard]] constexpr std::span<const GameplayCue> Cues() const noexcept {
        return {m_cues, m_cueCount};
    }

    [[nodiscard]] constexpr std::span<const CombatHitEvent> CombatHits() const noexcept {
        return {m_hits, m_hitCount};
    }

    constexpr void SetWorldFlag(u16 flagId, bool value = true) noexcept {
        if (flagId >= kMaxFlags) return;
        const u32 w   = static_cast<u32>(flagId >> 6U);
        const u64 bit = 1ULL << (flagId & 63U);
        if (value) m_worldFlags[w] |= bit;
        else m_worldFlags[w] &= ~bit;
    }

    [[nodiscard]] constexpr bool TestWorldFlag(u16 flagId) const noexcept {
        return flagId < kMaxFlags &&
               (m_worldFlags[flagId >> 6U] & (1ULL << (flagId & 63U))) != 0ULL;
    }

    constexpr void ClearWorldFlags() noexcept {
        m_worldFlags.fill(0ULL);
    }

    [[nodiscard]] constexpr const GameplayConfig& Config() const noexcept { return m_cfg; }
    constexpr void SetConfig(const GameplayConfig& cfg) noexcept { m_cfg = cfg; }

    [[nodiscard]] constexpr const GameplayCallbacks& Callbacks() const noexcept { return m_callbacks; }
    constexpr void SetCallbacks(const GameplayCallbacks& cb) noexcept { m_callbacks = cb; }

    [[nodiscard]] constexpr u64 TickCount() const noexcept { return m_tickCount; }
    [[nodiscard]] constexpr u32 TotalCombatHits() const noexcept { return m_totalHits; }
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_cues != nullptr; }

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    void StepStatesAndHealth(
        World& world,
        const TickStep& step,
        const TickInput& input,
        ActionBuffer& actions) noexcept;
    void StepCharactersPrePhysics(World& world, const TickStep& step) noexcept;
    void SyncCharactersPostPhysics(
        World& world,
        const PhysicsSystem& physics,
        const TickStep& step) noexcept;
    void ResolveCombat(World& world, const TickStep& step) noexcept;
    void StepCameras(World& world, const TickStep& step) noexcept;

    GameplayConfig               m_cfg{};
    u32                          m_pad0{0};
    GameplayCallbacks            m_callbacks{};
    Arena                        m_arena{};
    GameplayCue*                 m_cues{nullptr};
    CombatHitEvent*              m_hits{nullptr};
    u64                          m_tickCount{0};
    u32                          m_totalHits{0};
    u16                          m_cueCount{0};
    u16                          m_hitCount{0};
    std::array<u64, kFlagWords>  m_worldFlags{};
};
static_assert(sizeof(GameplaySystem) == 616);

} // namespace en

#pragma once

#include "core/Containers.h"
#include "core/Memory.h"
#include "gameplay/GameplayTypes.h"
#include "physics/PhysicsSystem.h"

namespace en {

// Deterministic 2D Metroidvania gameplay coordinator managing locomotion, FSMs, combat, mutations, and cameras.
// Move-disabled owner of an Arena; exposes a 4096-bit progression flag table, sparse GUID mutation journal, and cue/combat event buffers.
// Invariant: sizeof(GameplaySystem) == 648, and zero heap allocations occur after Init.
class GameplaySystem {
public:
    static constexpr u32 kFlagWords     = 64U;
    static constexpr u32 kMaxFlags      = kFlagWords * 64U;
    static constexpr u32 kMaxMutations  = 512U;
    static constexpr u32 kSaveBlobMagic = 0x5653564DU;

    struct SaveHeader {
        u32 magic{kSaveBlobMagic};
        u16 version{1};
        u16 mutationCount{0};
        u64 tickCount{0};
        u32 payloadCrc32{0};
        u32 pad{0};
    };
    static_assert(sizeof(SaveHeader) == 24);

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

    // Standalone full-tick helper executing PrePhysics -> Physics -> PostPhysics in sequence.
    void Step(
        World& world,
        PhysicsSystem& physics,
        const TickStep& step,
        const TickInput& input,
        ActionBuffer& actions) noexcept;

    // Canonical pipeline stage entry points invoked by EnginePipeline.
    void StepPrePhysics(
        World& world,
        const TickStep& step,
        const TickInput& input,
        ActionBuffer& actions) noexcept;
    void StepPostPhysics(
        World& world,
        const PhysicsSystem& physics,
        const TickStep& step) noexcept;

    // Granular subsystem steps (implemented across CharacterController.cpp, CombatResolver.cpp, CameraAndPersistence.cpp).
    void BeginTick(const TickInput& input, ActionBuffer& actions, bool playerFrozen) noexcept;
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

    // Applies persistent world flags and spatial GUID mutations to newly spawned room entities.
    u32 ApplyRoomMutations(World& world) noexcept;

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

    constexpr void ClearWorldFlags() noexcept { m_worldFlags.fill(0ULL); }

    bool RecordMutation(u64 guid, u32 value = 1U) noexcept {
        return guid != 0ULL && m_mutations.Insert(guid, value);
    }

    [[nodiscard]] u32 QueryMutation(u64 guid) const noexcept {
        const u32* p = m_mutations.Find(guid);
        return p ? *p : 0U;
    }

    [[nodiscard]] bool HasMutation(u64 guid) const noexcept {
        return m_mutations.Contains(guid);
    }

    void ClearMutations() noexcept { m_mutations.Clear(); }
    [[nodiscard]] constexpr u32 MutationCount() const noexcept { return m_mutations.Size(); }

    [[nodiscard]] usize RequiredSaveBytes() const noexcept {
        return sizeof(SaveHeader) + sizeof(m_worldFlags) +
               static_cast<usize>(m_mutations.Size()) * (sizeof(u64) + sizeof(u32));
    }

    [[nodiscard]] Result<usize, GameplayErrc> SerializePersistence(std::span<u8> outBuf) const noexcept;
    [[nodiscard]] Result<void, GameplayErrc> DeserializePersistence(std::span<const u8> inBuf) noexcept;

    [[nodiscard]] constexpr const GameplayConfig& Config() const noexcept { return m_cfg; }
    constexpr void SetConfig(const GameplayConfig& cfg) noexcept { m_cfg = cfg; }

    [[nodiscard]] constexpr const GameplayCallbacks& Callbacks() const noexcept { return m_callbacks; }
    constexpr void SetCallbacks(const GameplayCallbacks& cb) noexcept { m_callbacks = cb; }

    [[nodiscard]] constexpr u64 TickCount() const noexcept { return m_tickCount; }
    [[nodiscard]] constexpr u32 TotalCombatHits() const noexcept { return m_totalHits; }
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_cues != nullptr; }

    [[nodiscard]] u64 ComputeAuditHash(const World& world) const noexcept;

private:
    GameplayConfig              m_cfg{};
    GameplayCallbacks           m_callbacks{};
    Arena                       m_arena{};
    GameplayCue*                m_cues{nullptr};
    CombatHitEvent*             m_hits{nullptr};
    HashMap<u32>                m_mutations{};
    u64                         m_tickCount{0};
    u32                         m_totalHits{0};
    u16                         m_cueCount{0};
    u16                         m_hitCount{0};
    std::array<u64, kFlagWords> m_worldFlags{};
};
static_assert(sizeof(GameplaySystem) == 648);

} // namespace en

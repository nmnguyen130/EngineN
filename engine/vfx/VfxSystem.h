#pragma once

#include "assets/AssetBank.h"
#include "core/Jobs.h"
#include "core/Memory.h"
#include "gameplay/GameplayTypes.h"
#include "renderer/RenderList.h"
#include "vfx/VfxTypes.h"

namespace en {

// Standalone, zero-allocation 2D VFX subsystem managing Swap-Back dense particles and dash after-image ghosts.
// Decoupled from World; steps active particles in parallel via JobSystem::ParallelFor and extracts directly to RenderList.
// Invariant: sizeof(VfxSystem) == 56, and active elements are strictly packed in [0 .. m_particleCount) and [0 .. m_ghostCount).
class VfxSystem {
public:
    VfxSystem() noexcept = default;
    ~VfxSystem() noexcept { Shutdown(); }

    VfxSystem(const VfxSystem&) = delete;
    VfxSystem& operator=(const VfxSystem&) = delete;

    [[nodiscard]] Result<void, VfxErrc> Init(const VfxConfig& cfg = {}) noexcept;
    void Shutdown() noexcept;
    void Clear() noexcept;

    void EmitBurst(const ParticleBurstDesc& desc) noexcept;
    void EmitGhost(const DashGhost2D& ghost) noexcept;

    // Consumes a GameplayCue (CueKind::Effect or CueKind::DashGhost) and spawns the corresponding VFX.
    void ProcessCue(const GameplayCue& cue, const World& world) noexcept;

    // Steps active particles (via JobSystem::ParallelFor) and ghosts, then compacts expired slots via Swap-Back.
    void Step(const TickStep& step, JobSystem* jobs = nullptr) noexcept;

    // Extracts all active [0..m_ghostCount) and [0..m_particleCount) quads with zero dead-slot branching.
    void ExtractRender(const AssetBank& assets, RenderList& outList) const noexcept;

    [[nodiscard]] constexpr u32 ActiveParticleCount() const noexcept { return m_particleCount; }
    [[nodiscard]] constexpr u32 ActiveGhostCount() const noexcept { return m_ghostCount; }
    [[nodiscard]] constexpr std::span<const Particle2D> Particles() const noexcept {
        return {m_particles, m_particleCount};
    }
    [[nodiscard]] constexpr std::span<const DashGhost2D> Ghosts() const noexcept {
        return {m_ghosts, m_ghostCount};
    }
    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_particles != nullptr; }

private:
    Arena        m_arena{};
    Particle2D*  m_particles{nullptr};
    DashGhost2D* m_ghosts{nullptr};
    VfxConfig    m_cfg{};
    u16          m_particleCount{0};
    u16          m_ghostCount{0};
    u32          m_rngState{0xA341316CU};
    u32          m_tickCount{0};
};
static_assert(sizeof(VfxSystem) == 56);

} // namespace en

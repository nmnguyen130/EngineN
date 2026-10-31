#pragma once

#include "audio/AudioTypes.h"
#include "framework/FrameworkTypes.h"

namespace en {

class DebugSystem;
class Platform;
class Renderer;
class TimeSystem;
class VfxSystem;

// Non-owning runtime services bundle passed to EnginePipeline for built-in stage orchestration.
// Value type; bound once during Application::Init.
// Invariant: sizeof(PipelineServices) == 72.
struct PipelineServices {
    TimeSystem*     time{nullptr};
    Platform*       platform{nullptr};
    Renderer*       renderer{nullptr};
    DebugSystem*    debug{nullptr};
    VfxSystem*      vfx{nullptr};
    JobSystem*      jobs{nullptr};
    const AppHooks* hooks{nullptr};
    Viewport*       viewport{nullptr};
    f32             pixelsPerMeter{48.0f};
    u8              headless{1};
    u8              enableDebug{1};
    u16             pad0{0};
};
static_assert(sizeof(PipelineServices) == 72);

// Deterministic 5-stage Metroidvania execution pipeline (PrePhysics, Physics, PostPhysics, Extract, Overlay).
// Value type owning inline storage for up to kMaxSystems (32) sorted stage nodes; zero heap allocations.
// Invariant: systems within each stage are sorted by (order, registration index) when m_dirty == false.
class EnginePipeline {
public:
    static constexpr usize kPhaseCount = static_cast<usize>(PipelinePhase::Count);

    constexpr EnginePipeline() noexcept = default;

    void Clear() noexcept;

    // Binds runtime services and registers the 5 canonical built-in engine stages.
    void InitBuiltinStages(const PipelineServices& services) noexcept;

    [[nodiscard]] Result<void, FwErrc> Register(const SystemDesc& desc) noexcept;
    bool Unregister(StringId id) noexcept;
    bool SetEnabled(StringId id, bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled(StringId id) const noexcept;

    void Compile() noexcept;

    // Executes one 120 Hz simulation tick across PrePhysics -> Physics -> PostPhysics.
    void ExecuteSimTick(SimCtx& ctx, const TickStep& step) noexcept;

    // Executes one variable-rate presentation frame across Extract -> Overlay -> Submit.
    void ExecuteRenderFrame(DrawCtx& ctx) noexcept;

    void RunSimPhase(PipelinePhase phase, SimCtx& ctx, const TickStep& step) noexcept;
    void RunDrawPhase(PipelinePhase phase, DrawCtx& ctx) noexcept;

    [[nodiscard]] std::span<const SystemDesc> PhaseSystems(PipelinePhase phase) noexcept;

    [[nodiscard]] constexpr u16 SystemCount() const noexcept { return m_count; }
    [[nodiscard]] constexpr u64 PhaseRunCount(PipelinePhase phase) const noexcept {
        const usize p = static_cast<usize>(phase);
        return p < kPhaseCount ? m_phaseRuns[p] : 0ULL;
    }

    [[nodiscard]] u64 ComputeAuditHash() const noexcept;

private:
    static void StagePrePhysics(SimCtx& ctx, const TickStep& step, void* ud) noexcept;
    static void StagePhysics(SimCtx& ctx, const TickStep& step, void* ud) noexcept;
    static void StagePostPhysics(SimCtx& ctx, const TickStep& step, void* ud) noexcept;
    static void StageExtract(DrawCtx& ctx, void* ud) noexcept;
    static void StageOverlay(DrawCtx& ctx, void* ud) noexcept;

    void CheckRoomPortals(SimCtx& ctx) noexcept;
    void RouteGameplayCues(SimCtx& ctx, const TickStep& step) noexcept;

    PipelineServices                    m_srv{};
    std::array<SystemDesc, kMaxSystems> m_nodes{};
    std::array<AudioTrigger, 64>        m_audioTriggers{};
    std::array<u64, kPhaseCount>        m_phaseRuns{};
    std::array<u8, kPhaseCount>         m_phaseStart{};
    std::array<u8, kPhaseCount>         m_phaseCount{};
    u16                                 m_count{0};
    bool                                m_dirty{false};
};

} // namespace en

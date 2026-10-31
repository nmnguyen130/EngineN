#include "framework/EnginePipeline.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "debug/DebugSystem.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplaySystem.h"
#include "physics/PhysicsSystem.h"
#include "platform/Platform.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"
#include "time/Clock.h"
#include "time/TimeSystem.h"
#include "vfx/VfxSystem.h"
#include "world/World.h"
#include "world/WorldSystems.h"

namespace en {

using namespace en::literals;

void EnginePipeline::Clear() noexcept {
    m_srv        = {};
    m_nodes      = {};
    m_phaseStart = {};
    m_phaseCount = {};
    m_phaseRuns  = {};
    m_count      = 0U;
    m_dirty      = false;
}

void EnginePipeline::InitBuiltinStages(const PipelineServices& services) noexcept {
    Clear();
    m_srv = services;

    (void)Register(SystemDesc{
        .id       = "engine/pre_physics"_sid,
        .fn       = {.tickFn = &EnginePipeline::StagePrePhysics},
        .userData = this,
        .order    = 0,
        .phase    = PipelinePhase::PrePhysics,
    });
    (void)Register(SystemDesc{
        .id       = "engine/physics"_sid,
        .fn       = {.tickFn = &EnginePipeline::StagePhysics},
        .userData = this,
        .order    = 0,
        .phase    = PipelinePhase::Physics,
    });
    (void)Register(SystemDesc{
        .id       = "engine/post_physics"_sid,
        .fn       = {.tickFn = &EnginePipeline::StagePostPhysics},
        .userData = this,
        .order    = 0,
        .phase    = PipelinePhase::PostPhysics,
    });
    (void)Register(SystemDesc{
        .id       = "engine/extract"_sid,
        .fn       = {.drawFn = &EnginePipeline::StageExtract},
        .userData = this,
        .order    = 0,
        .phase    = PipelinePhase::Extract,
    });
    (void)Register(SystemDesc{
        .id       = "engine/overlay"_sid,
        .fn       = {.drawFn = &EnginePipeline::StageOverlay},
        .userData = this,
        .order    = 0,
        .phase    = PipelinePhase::Overlay,
    });

    Compile();
}

Result<void, FwErrc> EnginePipeline::Register(const SystemDesc& desc) noexcept {
    if (!desc.id.IsValid() || (!desc.fn.tickFn && !desc.fn.drawFn) ||
        static_cast<usize>(desc.phase) >= kPhaseCount) {
        return Err(FwErrc::InvalidConfig);
    }
    for (u16 i = 0U; i < m_count; ++i) {
        if (m_nodes[i].id == desc.id) {
            m_nodes[i] = desc;
            m_dirty    = true;
            return {};
        }
    }
    if (m_count >= kMaxSystems) return Err(FwErrc::PipelineFull);
    m_nodes[m_count++] = desc;
    m_dirty            = true;
    return {};
}

bool EnginePipeline::Unregister(StringId id) noexcept {
    if (!id.IsValid()) return false;
    for (u16 i = 0U; i < m_count; ++i) {
        if (m_nodes[i].id == id) {
            for (u16 j = i; j + 1U < m_count; ++j) {
                m_nodes[j] = m_nodes[j + 1U];
            }
            m_nodes[--m_count] = {};
            m_dirty = true;
            return true;
        }
    }
    return false;
}

bool EnginePipeline::SetEnabled(StringId id, bool enabled) noexcept {
    if (!id.IsValid()) return false;
    for (u16 i = 0U; i < m_count; ++i) {
        if (m_nodes[i].id == id) {
            m_nodes[i].enabled = enabled ? 1U : 0U;
            return true;
        }
    }
    return false;
}

bool EnginePipeline::IsEnabled(StringId id) const noexcept {
    for (u16 i = 0U; i < m_count; ++i) {
        if (m_nodes[i].id == id) return m_nodes[i].enabled != 0U;
    }
    return false;
}

void EnginePipeline::Compile() noexcept {
    if (!m_dirty) return;
    m_dirty = false;

    for (u16 i = 1U; i < m_count; ++i) {
        const SystemDesc key = m_nodes[i];
        u16 j = i;
        while (j > 0U) {
            const SystemDesc& prev = m_nodes[j - 1U];
            const bool after = (static_cast<u8>(prev.phase) > static_cast<u8>(key.phase)) ||
                               (prev.phase == key.phase && prev.order > key.order);
            if (!after) break;
            m_nodes[j] = prev;
            --j;
        }
        m_nodes[j] = key;
    }

    m_phaseStart.fill(0U);
    m_phaseCount.fill(0U);
    for (u16 i = 0U; i < m_count; ++i) {
        const usize p = static_cast<usize>(m_nodes[i].phase);
        if (m_phaseCount[p] == 0U) m_phaseStart[p] = static_cast<u8>(i);
        ++m_phaseCount[p];
    }
}

void EnginePipeline::RunSimPhase(PipelinePhase phase, SimCtx& ctx, const TickStep& step) noexcept {
    if (m_dirty) Compile();
    const usize p = static_cast<usize>(phase);
    if (p >= static_cast<usize>(PipelinePhase::Extract)) return;
    ++m_phaseRuns[p];

    const u8 start = m_phaseStart[p];
    const u8 count = m_phaseCount[p];
    for (u8 i = 0U; i < count; ++i) {
        const SystemDesc& sys = m_nodes[start + i];
        if (sys.enabled != 0U && sys.fn.tickFn) {
            sys.fn.tickFn(ctx, step, sys.userData);
        }
    }
}

void EnginePipeline::RunDrawPhase(PipelinePhase phase, DrawCtx& ctx) noexcept {
    if (m_dirty) Compile();
    const usize p = static_cast<usize>(phase);
    if (p < static_cast<usize>(PipelinePhase::Extract) || p >= kPhaseCount) return;
    ++m_phaseRuns[p];

    const u8 start = m_phaseStart[p];
    const u8 count = m_phaseCount[p];
    for (u8 i = 0U; i < count; ++i) {
        const SystemDesc& sys = m_nodes[start + i];
        if (sys.enabled != 0U && sys.fn.drawFn) {
            sys.fn.drawFn(ctx, sys.userData);
        }
    }
}

void EnginePipeline::ExecuteSimTick(SimCtx& ctx, const TickStep& step) noexcept {
    RunSimPhase(PipelinePhase::PrePhysics, ctx, step);
    RunSimPhase(PipelinePhase::Physics, ctx, step);
    RunSimPhase(PipelinePhase::PostPhysics, ctx, step);
}

void EnginePipeline::ExecuteRenderFrame(DrawCtx& ctx) noexcept {
    RunDrawPhase(PipelinePhase::Extract, ctx);
    RunDrawPhase(PipelinePhase::Overlay, ctx);

    if (m_srv.renderer) {
        if (m_srv.debug) {
            ScopeTimer<Clock> submitTimer(m_srv.debug->Profiler(), ProfileScope::Submit);
            m_srv.renderer->Submit(*ctx.list);
        } else {
            m_srv.renderer->Submit(*ctx.list);
        }
    }
    if (m_srv.debug && m_srv.enableDebug != 0U) {
        m_srv.debug->Profiler().RecordTelemetry(Telemetry{
            .entities = static_cast<u16>(ctx.world->AliveCount()),
            .quads    = static_cast<u16>(ctx.list->Stats().submitted),
            .batches  = static_cast<u16>(ctx.list->Stats().batches),
            .contacts = static_cast<u16>(ctx.physics->ContactEvents().size()),
            .voices   = 0U,
        });
    }
}

std::span<const SystemDesc> EnginePipeline::PhaseSystems(PipelinePhase phase) noexcept {
    if (m_dirty) Compile();
    const usize p = static_cast<usize>(phase);
    if (p >= kPhaseCount) return {};
    return {m_nodes.data() + m_phaseStart[p], m_phaseCount[p]};
}

void EnginePipeline::StagePrePhysics(SimCtx& ctx, const TickStep& step, void*) noexcept {
    ctx.world->BeginTick(step);
    ctx.scenes->TickStates(ctx, step);
    WorldSystems::StepVelocities(*ctx.world, step);
    ctx.gameplay->StepPrePhysics(*ctx.world, step, *ctx.input, *ctx.actions);
}

void EnginePipeline::StagePhysics(SimCtx& ctx, const TickStep& step, void* ud) noexcept {
    auto* self = static_cast<EnginePipeline*>(ud);
    if (self->m_srv.debug) {
        ScopeTimer<Clock> t(self->m_srv.debug->Profiler(), ProfileScope::Physics);
        ctx.physics->Step(*ctx.world, step);
    } else {
        ctx.physics->Step(*ctx.world, step);
    }
}

void EnginePipeline::StagePostPhysics(SimCtx& ctx, const TickStep& step, void* ud) noexcept {
    auto*     self = static_cast<EnginePipeline*>(ud);
    EventBus* bus  = self->m_srv.events;
    if (self->m_srv.debug) {
        ScopeTimer<Clock> t(self->m_srv.debug->Profiler(), ProfileScope::Gameplay);
        ctx.gameplay->StepPostPhysics(*ctx.world, *ctx.physics, step, bus);
    } else {
        ctx.gameplay->StepPostPhysics(*ctx.world, *ctx.physics, step, bus);
    }
    WorldSystems::StepVisualsAndLifetimes(*ctx.world, step);

    if (bus) {
        if (self->m_srv.vfx) self->m_srv.vfx->ConsumeEvents(*bus, *ctx.world);
        if (self->m_srv.time) self->m_srv.time->ConsumeEvents(*bus);
        if (self->m_srv.platform) self->m_srv.platform->ConsumeEvents(*bus);
        if (ctx.scenes) ctx.scenes->ConsumeEvents(*bus);
    }
    if (self->m_srv.vfx) {
        self->m_srv.vfx->Step(step, self->m_srv.jobs);
    }
    self->StepAudio(ctx, step);
    ctx.world->EndTick(step);
}

void EnginePipeline::StageExtract(DrawCtx& ctx, void* ud) noexcept {
    auto* self = static_cast<EnginePipeline*>(ud);
    const Viewport vp = self->m_srv.viewport ? *self->m_srv.viewport : Viewport{1280, 720};
    ctx.list->Reset(vp);
    if (self->m_srv.debug) {
        ScopeTimer<Clock> t(self->m_srv.debug->Profiler(), ProfileScope::Extract);
        WorldSystems::ExtractRender(
            *ctx.world, *ctx.assets, *ctx.renderStep, *ctx.list, self->m_srv.pixelsPerMeter);
        if (self->m_srv.vfx) self->m_srv.vfx->ExtractRender(*ctx.assets, *ctx.list);
    } else {
        WorldSystems::ExtractRender(
            *ctx.world, *ctx.assets, *ctx.renderStep, *ctx.list, self->m_srv.pixelsPerMeter);
        if (self->m_srv.vfx) self->m_srv.vfx->ExtractRender(*ctx.assets, *ctx.list);
    }
    ctx.scenes->DrawStates(ctx);
}

void EnginePipeline::StageOverlay(DrawCtx& ctx, void* ud) noexcept {
    auto* self = static_cast<EnginePipeline*>(ud);
    const Viewport vp = self->m_srv.viewport ? *self->m_srv.viewport : Viewport{1280, 720};
    ctx.list->EmitPostProcess();
    ctx.scenes->DrawWipe(*ctx.list, vp);
    if (self->m_srv.debug && self->m_srv.enableDebug != 0U) {
        ScopeTimer<Clock> t(self->m_srv.debug->Profiler(), ProfileScope::Debug);
        const u32 tick = self->m_srv.time
            ? static_cast<u32>(self->m_srv.time->Stats().totalTicks)
            : 0U;
        FrameCtx fc{
            .world    = ctx.world,
            .physics  = ctx.physics,
            .viewport = vp,
            .tick     = tick,
        };
        self->m_srv.debug->Emit(fc, *ctx.list);
    }
}

void EnginePipeline::StepAudio(SimCtx& ctx, const TickStep& step) noexcept {
    if (!ctx.audio) return;

    Vec2 listenerPos{};
    if ((ctx.world->RegisteredPoolsMask() & MakeComponentMask<CameraFollow2D>()) != 0ULL) {
        const auto& pool = ctx.world->GetPool<CameraFollow2D>();
        if (pool.Size() > 0U) listenerPos = pool.ComponentAtDense(0U).anchor;
    }

    std::array<f32, kGameplayChannels> scales{};
    for (u8 i = 0U; i < kGameplayChannels; ++i) {
        scales[i] = step.Scale(static_cast<TimeChannel>(i));
    }

    const std::span<const AudioTrigger> triggers =
        m_srv.events ? m_srv.events->audio.Drain() : std::span<const AudioTrigger>{};

    AudioUpdateDesc ad{
        .triggers      = triggers,
        .assets        = ctx.assets,
        .occlusionFn   = [](Vec2 l, Vec2 e, void* p) noexcept -> bool {
            const auto* phys = static_cast<const PhysicsSystem*>(p);
            if (!phys) return false;
            const Vec2 d   = e - l;
            const f32  len = d.Length();
            return len > 1e-4f && static_cast<bool>(phys->Raycast(l, d, len, 0x0001U));
        },
        .occlusionCtx  = ctx.physics,
        .channelScales = scales.data(),
        .listenerPos   = listenerPos,
        .tick          = step.tick,
        .frozenMask    = static_cast<u8>(step.frozenMask | (ctx.scenes->IsFrozen() ? kGameplayMask : 0U)),
    };
    ctx.audio->Update(ad);
}

u64 EnginePipeline::ComputeAuditHash() const noexcept {
    u64 h = HashCombine(0xCBF29CE484222325ULL, m_count);
    for (u16 i = 0U; i < m_count; ++i) {
        if (m_nodes[i].enabled != 0U) {
            h = HashCombine(h, m_nodes[i].id.value ^ static_cast<u64>(m_nodes[i].phase));
        }
    }
    return h;
}

} // namespace en

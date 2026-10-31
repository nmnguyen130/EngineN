#include "vfx/VfxSystem.h"

#include "world/World.h"

namespace en {
namespace {

[[nodiscard]] constexpr f32 NextSignedUnit(u32& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<f32>(static_cast<i32>(state & 0xFFFFU) - 32768) * (1.0f / 32768.0f);
}

void IntegrateParticleSlice(std::span<Particle2D> slice, f32 dt) noexcept {
    for (Particle2D& p : slice) {
        if (p.lifeTicks > 0U) --p.lifeTicks;
        p.vel.y += p.gravity * dt;
        if (p.dampingQ6 > 0U) {
            const f32 damp = static_cast<f32>(p.dampingQ6) * (1.0f / 64.0f);
            p.vel *= 1.0f / (1.0f + damp * dt);
        }
        p.pos += p.vel * dt;
    }
}

} // namespace

Result<void, VfxErrc> VfxSystem::Init(const VfxConfig& cfg) noexcept {
    Shutdown();
    if (cfg.maxParticles == 0U || cfg.maxGhosts == 0U || cfg.arenaBytes < 4096U) {
        return Err(VfxErrc::InvalidConfig);
    }
    if (!m_arena.Init(cfg.arenaBytes)) {
        return Err(VfxErrc::OutOfMemory);
    }

    auto pSpan = m_arena.Alloc<Particle2D>(cfg.maxParticles);
    auto gSpan = m_arena.Alloc<DashGhost2D>(cfg.maxGhosts);
    if (pSpan.empty() || gSpan.empty()) {
        Shutdown();
        return Err(VfxErrc::OutOfMemory);
    }

    m_particles = pSpan.data();
    m_ghosts    = gSpan.data();
    m_cfg       = cfg;
    return {};
}

void VfxSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_particles     = nullptr;
    m_ghosts        = nullptr;
    m_cfg           = {};
    m_particleCount = 0U;
    m_ghostCount    = 0U;
    m_rngState      = 0xA341316CU;
    m_tickCount     = 0U;
}

void VfxSystem::Clear() noexcept {
    m_particleCount = 0U;
    m_ghostCount    = 0U;
    m_rngState      = 0xA341316CU;
}

void VfxSystem::EmitBurst(const ParticleBurstDesc& desc) noexcept {
    if (!m_particles || desc.count == 0U || desc.lifeTicks == 0U) return;

    u32 rng = m_rngState ^ (m_tickCount * 0x9E3779B9U + m_particleCount);
    if (rng == 0U) rng = 0xA341316CU;

    for (u8 i = 0U; i < desc.count; ++i) {
        if (m_particleCount >= m_cfg.maxParticles) {
            // Overwrite oldest slot if at capacity.
            m_particles[i % m_cfg.maxParticles] = Particle2D{
                .pos          = desc.origin,
                .vel          = {desc.baseVel.x + NextSignedUnit(rng) * desc.velSpread.x,
                                 desc.baseVel.y + NextSignedUnit(rng) * desc.velSpread.y},
                .color        = desc.color,
                .size         = desc.size,
                .gravity      = desc.gravity,
                .lifeTicks    = desc.lifeTicks,
                .maxLifeTicks = desc.lifeTicks,
                .dampingQ6    = 8U,
                .layer        = desc.layer,
            };
            continue;
        }

        const f32 rx = NextSignedUnit(rng);
        const f32 ry = NextSignedUnit(rng);
        m_particles[m_particleCount++] = Particle2D{
            .pos          = desc.origin,
            .vel          = {desc.baseVel.x + rx * desc.velSpread.x, desc.baseVel.y + ry * desc.velSpread.y},
            .color        = desc.color,
            .size         = desc.size,
            .gravity      = desc.gravity,
            .lifeTicks    = desc.lifeTicks,
            .maxLifeTicks = desc.lifeTicks,
            .dampingQ6    = 8U,
            .layer        = desc.layer,
        };
    }
    m_rngState = rng;
}

void VfxSystem::EmitGhost(const DashGhost2D& ghost) noexcept {
    if (!m_ghosts || ghost.remainingTicks == 0U) return;
    if (m_ghostCount < m_cfg.maxGhosts) {
        m_ghosts[m_ghostCount++] = ghost;
    } else {
        m_ghosts[m_tickCount % m_cfg.maxGhosts] = ghost;
    }
}

void VfxSystem::ProcessCue(const GameplayCue& cue, const World& world) noexcept {
    if (!m_particles) return;

    if (cue.kind == CueKind::DashGhost) {
        DashGhost2D ghost{
            .pos            = cue.position,
            .size           = {1.0f, 1.0f},
            .tint           = {110, 215, 255, 180},
            .remainingTicks = static_cast<u8>(std::clamp<u16>(cue.durationTicks, 1U, 255U)),
            .initialTicks   = static_cast<u8>(std::clamp<u16>(cue.durationTicks, 1U, 255U)),
            .layer          = 9U,
            .flags          = static_cast<u8>(cue.userTag & 0x03U),
        };
        if (cue.emitter.IsValid() && world.IsAlive(cue.emitter)) {
            if (const auto* sp = world.TryGet<Sprite2D>(cue.emitter)) {
                ghost.size   = {sp->width, sp->height};
                ghost.sprite = sp->sprite;
                ghost.layer  = (sp->layer > 0U) ? static_cast<u8>(sp->layer - 1U) : 0U;
            }
        }
        EmitGhost(ghost);
        return;
    }

    if (cue.kind == CueKind::Effect) {
        const u8 count = static_cast<u8>(std::clamp<u16>(cue.userTag > 0U ? cue.userTag : 6U, 1U, 64U));
        const u8 life  = static_cast<u8>(std::clamp<u16>(cue.durationTicks > 0U ? cue.durationTicks : 14U, 1U, 255U));
        EmitBurst(ParticleBurstDesc{
            .origin    = cue.position,
            .baseVel   = {0.0f, 1.8f * cue.magnitude},
            .velSpread = {2.8f * cue.magnitude, 2.2f * cue.magnitude},
            .color     = {255, 230, 175, 235},
            .size      = 0.11f,
            .gravity   = -10.0f,
            .lifeTicks = life,
            .count     = count,
        });
    }
}

void VfxSystem::Step(const TickStep& step, JobSystem* jobs) noexcept {
    if (!m_particles || !step || step.IsFrozen(TimeChannel::World)) return;
    ++m_tickCount;

    const f32 dt = step.Dt(TimeChannel::World);
    if (dt <= 0.0f) return;

    // 1. Parallel integration of active particles [0 .. m_particleCount) across worker threads.
    if (m_particleCount > 0U) {
        std::span<Particle2D> activeSpan{m_particles, m_particleCount};
        const u32 batchSize = std::max<u32>(16U, m_cfg.parallelBatchSize);
        if (jobs && jobs->WorkerCount() > 0U && m_particleCount >= batchSize) {
            jobs->ParallelFor(m_particleCount, batchSize, [&](u32 begin, u32 end, u32) noexcept {
                IntegrateParticleSlice(activeSpan.subspan(begin, end - begin), dt);
            });
        } else {
            IntegrateParticleSlice(activeSpan, dt);
        }

        // Swap-Back compaction: O(1) removal of expired particles so [0 .. m_particleCount) remains 100% dense.
        u16 i = 0U;
        while (i < m_particleCount) {
            if (m_particles[i].lifeTicks == 0U) {
                m_particles[i] = m_particles[--m_particleCount];
            } else {
                ++i;
            }
        }
    }

    // 2. Step and Swap-Back compact active dash after-image ghosts [0 .. m_ghostCount).
    u16 g = 0U;
    while (g < m_ghostCount) {
        if (m_ghosts[g].remainingTicks > 0U) --m_ghosts[g].remainingTicks;
        if (m_ghosts[g].remainingTicks == 0U) {
            m_ghosts[g] = m_ghosts[--m_ghostCount];
        } else {
            ++g;
        }
    }
}

void VfxSystem::ExtractRender(const AssetBank& assets, RenderList& outList) const noexcept {
    if (!m_particles) return;

    // Dense iteration over [0 .. m_ghostCount): every element is guaranteed alive.
    for (u16 i = 0U; i < m_ghostCount; ++i) {
        const DashGhost2D& g  = m_ghosts[i];
        const f32          f  = static_cast<f32>(g.remainingTicks) / static_cast<f32>(std::max<u8>(1U, g.initialTicks));
        const SpriteFrame& fr = assets.GetSprite(g.sprite);
        Color col             = g.tint.ToColor();
        col.a                *= f;
        outList.Push({
            .center  = g.pos,
            .size    = g.size,
            .pivot   = fr.pivot,
            .uv      = fr.uv,
            .color   = col,
            .texture = fr.gpuTexture,
            .depth   = g.depth,
            .layer   = g.layer,
            .blend   = BlendMode::Additive,
            .flipX   = (g.flags & 0x01U) != 0U,
            .flipY   = (g.flags & 0x02U) != 0U,
        });
    }

    // Dense iteration over [0 .. m_particleCount): every element is guaranteed alive (zero dead-slot branching).
    for (u16 i = 0U; i < m_particleCount; ++i) {
        const Particle2D& p = m_particles[i];
        const f32         f = static_cast<f32>(p.lifeTicks) / static_cast<f32>(std::max<u8>(1U, p.maxLifeTicks));
        Color col           = p.color.ToColor();
        col.a              *= f;
        const f32 s         = p.size * (0.5f + 0.5f * f);
        outList.Push({
            .center  = p.pos,
            .size    = {s, s},
            .color   = col,
            .texture = kWhiteTexture,
            .depth   = 32800U,
            .layer   = p.layer,
            .blend   = BlendMode::Additive,
        });
    }
}

} // namespace en

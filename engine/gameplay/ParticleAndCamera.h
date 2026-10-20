#pragma once

#include "core/Memory.h"
#include "gameplay/GameplayTypes.h"
#include "world/World.h"

namespace en {

class SpatialIndex2D;

struct ParticleBurstDesc {
    Vec2 origin{};
    Vec2 baseVelocity{0.0f, 3.0f};
    f32 speedSpread{2.5f};
    f32 gravityScale{0.5f};
    f32 drag{1.5f};
    f32 startSize{0.16f};
    f32 endSize{0.03f};
    f32 lifetimeSec{0.45f};
    Color startColor{1.0f, 0.8f, 0.3f, 1.0f};
    Color endColor{1.0f, 0.2f, 0.0f, 0.0f};
    u16 count{16};
};

// Structure-of-Arrays (SoA) high-throughput particle simulation pool.
class alignas(64) ParticlePool2D {
public:
    [[nodiscard]] bool Init(AlignedArena& arena, u32 capacity = 2048) noexcept;
    void Clear() noexcept { m_activeCount = 0; }

    void EmitBurst(const ParticleBurstDesc& desc) noexcept;
    void Step(f32 dt, f32 worldGravity = 32.0f) noexcept;

    [[nodiscard]] constexpr u32 ActiveCount() const noexcept { return m_activeCount; }
    [[nodiscard]] constexpr std::span<const f32> PosX() const noexcept { return m_posX.subspan(0, m_activeCount); }
    [[nodiscard]] constexpr std::span<const f32> PosY() const noexcept { return m_posY.subspan(0, m_activeCount); }
    [[nodiscard]] constexpr std::span<const f32> Size() const noexcept { return m_size.subspan(0, m_activeCount); }
    [[nodiscard]] constexpr std::span<const Color> Colors() const noexcept { return m_color.subspan(0, m_activeCount); }

private:
    std::span<f32> m_posX{};
    std::span<f32> m_posY{};
    std::span<f32> m_velX{};
    std::span<f32> m_velY{};
    std::span<f32> m_drag{};
    std::span<f32> m_gravScale{};
    std::span<f32> m_age{};
    std::span<f32> m_invLife{};
    std::span<f32> m_startSize{};
    std::span<f32> m_endSize{};
    std::span<f32> m_size{};
    std::span<Color> m_startColor{};
    std::span<Color> m_endColor{};
    std::span<Color> m_color{};
    u32 m_capacity{0};
    u32 m_activeCount{0};
    Rng64 m_rng{0x9E3779B97F4A7C15ULL};
};

// 2nd-order critically damped camera director and safe-ground breadcrumb verifier.
class CameraAndBreadcrumbSolver final {
public:
    static void StepCameraControllers(World& world, const SimStepView& step) noexcept;
    static void StepSafeBreadcrumbs(World& world, const SpatialIndex2D& spatial, const SimStepView& step) noexcept;
};

} // namespace en

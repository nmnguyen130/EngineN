#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "core/Jobs.h"
#include "debug/DebugSystem.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplayPipeline.h"
#include "input/InputPipeline.h"
#include "physics/PhysicsSystem.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"
#include "world/World.h"

namespace en {

struct alignas(32) ApplicationConfig {
    AssetBankConfig assets{};
    AudioSystemConfig audio{};
    DebugSystemConfig debug{};
    ViewportExtent viewport{1280, 720};
    WorldConfig world{};
    f32 defaultPixelsPerMeter{48.0f};
    u16 maxManifolds{1024U};
    u16 maxCuesPerTick{64U};
    u8 velocityIterations{8U};
    u8 sleepTicksThreshold{60U};
    u8 headless{1U};
    u8 enableDebugTools{1U};
};
static_assert(sizeof(ApplicationConfig) == 96);

// Top-level EngineN runtime coordinator owning all 12 subsystems inside a single AlignedArena slab.
// Move-only resource owner; destroys subsystems in reverse dependency order on Shutdown().
// Invariant: m_rt != nullptr if and only if m_initialized != 0U.
class alignas(64) Application {
public:
    Application() noexcept = default;
    ~Application() noexcept { Shutdown(); }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    Application(Application&& other) noexcept;
    Application& operator=(Application&& other) noexcept;

    [[nodiscard]] Result<void, FrameworkError> Init(
        const ApplicationConfig& cfg = {},
        const AppHooks& hooks = {}) noexcept;

    void Shutdown() noexcept;

    [[nodiscard]] constexpr bool IsInitialized() const noexcept { return m_initialized != 0U; }
    [[nodiscard]] constexpr bool IsRunning() const noexcept { return m_running != 0U; }
    [[nodiscard]] constexpr bool IsHeadless() const noexcept { return m_headless != 0U; }
    constexpr void RequestQuit() noexcept { m_running = 0U; }

    void BindReplayStream(InputReplayStream* stream, ReplayMode mode) noexcept;
    [[nodiscard]] constexpr ReplayMode GetReplayMode() const noexcept { return m_replayMode; }

    void UploadCommittedTexturesToGpu(u16 customFontTexSlot = 0xFFFFU) noexcept;

    [[nodiscard]] Result<void, FrameworkError> Run(u32 targetFps = 120U) noexcept;
    [[nodiscard]] Result<void, FrameworkError> StepFrame(u64 rawDeltaNs = kFixedDtNs) noexcept;

    [[nodiscard]] Result<FrameAuditDigest, FrameworkError> StepHeadless(
        u32 tickCount = 1U,
        u32 audioBlocksPerTick = 2U) noexcept;

    [[nodiscard]] Result<usize, WorldErrc> SaveWorldSnapshot(std::span<u8> outBuffer) const noexcept;
    [[nodiscard]] Result<void, WorldErrc> LoadWorldSnapshot(std::span<const u8> inBuffer) noexcept;

    [[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept;
    [[nodiscard]] u64 LastCombinedAuditHash() const noexcept {
        return (m_lastCombinedAuditHash != 0ULL) ? m_lastCombinedAuditHash : ComputeFrameAuditDigest().combinedHash;
    }
    [[nodiscard]] constexpr u64 FrameIndex() const noexcept { return m_frameIndex; }
    [[nodiscard]] constexpr u64 TotalSimTicks() const noexcept { return m_totalSimTicks; }

    [[nodiscard]] Platform& GetPlatform() noexcept;
    [[nodiscard]] const Platform& GetPlatform() const noexcept;
    [[nodiscard]] Window& GetWindow() noexcept;
    [[nodiscard]] const Window& GetWindow() const noexcept;
    [[nodiscard]] Renderer& GetRenderer() noexcept;
    [[nodiscard]] const Renderer& GetRenderer() const noexcept;
    [[nodiscard]] JobSystem& Jobs() noexcept;
    [[nodiscard]] const JobSystem& Jobs() const noexcept;
    [[nodiscard]] SceneDirector& Director() noexcept;
    [[nodiscard]] const SceneDirector& Director() const noexcept;
    [[nodiscard]] World& GetWorld() noexcept;
    [[nodiscard]] const World& GetWorld() const noexcept;
    [[nodiscard]] PhysicsSystem& Physics() noexcept;
    [[nodiscard]] const PhysicsSystem& Physics() const noexcept;
    [[nodiscard]] GameplayPipeline& Gameplay() noexcept;
    [[nodiscard]] const GameplayPipeline& Gameplay() const noexcept;
    [[nodiscard]] AssetBank& Assets() noexcept;
    [[nodiscard]] const AssetBank& Assets() const noexcept;
    [[nodiscard]] AudioSystem& Audio() noexcept;
    [[nodiscard]] const AudioSystem& Audio() const noexcept;
    [[nodiscard]] DebugSystem& Debug() noexcept;
    [[nodiscard]] const DebugSystem& Debug() const noexcept;
    [[nodiscard]] TimePipeline& Time() noexcept;
    [[nodiscard]] const TimePipeline& Time() const noexcept;
    [[nodiscard]] InputPipeline& Input() noexcept;
    [[nodiscard]] const InputPipeline& Input() const noexcept;
    [[nodiscard]] ActionBuffer& Actions() noexcept;
    [[nodiscard]] const ActionBuffer& Actions() const noexcept;
    [[nodiscard]] RenderList& GetRenderList() noexcept;
    [[nodiscard]] const RenderList& GetRenderList() const noexcept;

private:
    struct AppRuntimeState;

    [[nodiscard]] SceneLoadContext MakeLoadContext() const noexcept;
    void RouteSimCuesAndSyncAudio(SimContext& simCtx, const SimStepView& step) noexcept;
    void ExtractAndCompileRenderFrame() noexcept;

    AlignedArena m_arena{};
    AppRuntimeState* m_rt{nullptr};
    void* m_userAppState{nullptr};
    mutable u64 m_lastCombinedAuditHash{0};
    u64 m_frameIndex{0};
    u64 m_totalSimTicks{0};
    u16 m_pixelsPerMeterQ4{768U};
    u8 m_initialized{0U};
    u8 m_running{0U};
    u8 m_headless{1U};
    u8 m_debugEnabled{1U};
    u8 m_statePausedClock{0U};
    ReplayMode m_replayMode{ReplayMode::None};
};
static_assert(sizeof(Application) == 64);

} // namespace en

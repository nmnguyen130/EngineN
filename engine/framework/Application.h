#pragma once

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "debug/DebugSystem.h"
#include "framework/SceneDirector.h"
#include "gameplay/GameplayPipeline.h"
#include "input/InputPipeline.h"
#include "physics/PhysicsSystem.h"
#include "renderer/RenderList.h"
#include "world/World.h"

namespace en {

// 96-byte (3 x 32B blocks exact, zero padding) consolidated configuration for Application::Init().
struct alignas(32) ApplicationConfig {
    AssetBankConfig assets{};           // 32B (offset 0..31)
    AudioSystemConfig audio{};          // 16B (offset 32..47)
    DebugSystemConfig debug{};          // 16B (offset 48..63)
    ViewportExtent viewport{1280, 720}; // 8B  (offset 64..71)
    WorldConfig world{};                // 12B (offset 72..83)
    f32 defaultPixelsPerMeter{48.0f};   // 4B  (offset 84..87)
    u16 maxManifolds{1024U};            // 2B  (offset 88..89)
    u16 maxCuesPerTick{64U};            // 2B  (offset 90..91)
    u8 velocityIterations{8U};          // 1B  (offset 92)
    u8 sleepTicksThreshold{60U};        // 1B  (offset 93)
    u8 headless{1U};                    // 1B  (offset 94)
    u8 enableDebugTools{1U};            // 1B  (offset 95)
};
static_assert(sizeof(ApplicationConfig) == 96);

// 64-byte (1 cache line exact, zero padding) top-level EngineN runtime coordinator owning all 11 subsystems inside a single AlignedArena slab.
// Prevents >1.3 MB stack overflow in main() by housing RenderList and subsystem instances in a 64B-aligned arena allocation.
// Invariant: sizeof(Application) == 64, subsystems initialize in dependency order and tear down in strict reverse order.
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
    constexpr void RequestQuit() noexcept { m_running = 0U; }

    // Binds an InputReplayStream for deterministic 120 Hz input recording or automated playback.
    void BindReplayStream(InputReplayStream* stream, ReplayMode mode) noexcept;
    [[nodiscard]] constexpr ReplayMode GetReplayMode() const noexcept { return m_replayMode; }

    // Advances the runtime by rawDeltaNs of wall-clock time (120 Hz fixed-step simulation + interpolated render extraction).
    [[nodiscard]] Result<void, FrameworkError> StepFrame(u64 rawDeltaNs = kFixedDtNs) noexcept;

    // Runs tickCount deterministic 120 Hz simulation ticks and audioBlocksPerTick headless DSP blocks, returning the 64B FrameAuditDigest.
    [[nodiscard]] Result<FrameAuditDigest, FrameworkError> StepHeadless(
        u32 tickCount = 1U,
        u32 audioBlocksPerTick = 2U) noexcept;

    [[nodiscard]] FrameAuditDigest ComputeFrameAuditDigest() const noexcept;
    [[nodiscard]] constexpr u64 LastCombinedAuditHash() const noexcept { return m_lastCombinedAuditHash; }
    [[nodiscard]] constexpr u64 FrameIndex() const noexcept { return m_frameIndex; }
    [[nodiscard]] constexpr u64 TotalSimTicks() const noexcept { return m_totalSimTicks; }

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

    // Cache Line 0 (offset 0..63, 64B exact, zero padding)
    AlignedArena m_arena{};                     // 16B (0..15)
    AppRuntimeState* m_rt{nullptr};             // 8B  (16..23)
    void* m_userAppState{nullptr};              // 8B  (24..31)
    u64 m_lastCombinedAuditHash{0};             // 8B  (32..39)
    u64 m_frameIndex{0};                        // 8B  (40..47)
    u64 m_totalSimTicks{0};                     // 8B  (48..55)
    u16 m_pixelsPerMeterQ4{768U};               // 2B  (56..57) 48.0f * 16
    u8 m_initialized{0U};                       // 1B  (58)
    u8 m_running{0U};                           // 1B  (59)
    u8 m_headless{1U};                          // 1B  (60)
    u8 m_debugEnabled{1U};                      // 1B  (61)
    u8 m_statePausedClock{0U};                  // 1B  (62)
    ReplayMode m_replayMode{ReplayMode::None};  // 1B  (63)
};
static_assert(sizeof(Application) == 64);

} // namespace en

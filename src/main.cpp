#include "core/time/TimeManager.h"
#include "core/Config.h"
#include "core/Log.h"
#include "math/Vec2.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventPump.h"
#include "platform/input/InputSystem.h"
#include "renderer/RendererTypes.h"
#include "renderer/RenderDevice.h"
#include "renderer/RenderContext.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace {

struct DemoEntity {
    EngineN::Vec2 prevPosition{400.0f, 300.0f};
    EngineN::Vec2 position{400.0f, 300.0f};
    EngineN::Vec2 velocity{0.0f, 0.0f};
    EngineN::Vec2 size{48.0f, 48.0f};
    float speed{400.0f};
    bool isAttacking = false;
    bool isDashing = false;
    double actionTimer = 0.0;

    void Init(const EngineN::Vec2& startPos) {
        prevPosition = startPos;
        position = startPos;
    }
};

void SetTargetFps(
    int fps,
    EngineN::TimeManager& time,
    EngineN::RenderDevice& device,
    int& framesThisSec,
    uint64_t& lastFpsTimeNs)
{
    if (time.Pacer().GetTargetFps() == fps) {
        return; // Already at this target, do not reconfigure VSync or reset timers
    }
    time.Pacer().SetTargetFps(fps);
    framesThisSec = 0;
    lastFpsTimeNs = SDL_GetTicksNS();
    if (fps == 0) {
        (void)device.SetVSync(EngineN::VSyncMode::Adaptive);
    } else {
        (void)device.SetVSync(EngineN::VSyncMode::Off);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    using namespace EngineN;

    AppConfig config{
        .appName = "EngineN — Sandbox Runtime",
        .windowWidth = 1280,
        .windowHeight = 720,
        .simulationHz = 120,
        .targetRenderFps = 0, // 0 = Uncapped / VSync-driven
        .maxCatchupTicks = 8
    };

    // 1. Initialize Platform Subsystem
    EngineN::Platform platform;
    auto platRes = platform.Init();
    if (!platRes) {
        ENGINE_LOG(Fatal, Platform, "Platform initialization failed: {}", platRes.Error().message);
        return -1;
    }

    // 2. Create Engine Window
    EngineN::Window window;
    EngineN::WindowConfig winConfig{
        .title = config.appName,
        .width = config.windowWidth,
        .height = config.windowHeight,
        .mode = EngineN::WindowMode::Windowed,
        .highDPI = true,
        .resizable = true
    };
    auto winRes = window.Create(winConfig);
    if (!winRes) {
        ENGINE_LOG(Fatal, Platform, "Window creation failed: {}", winRes.Error().message);
        return -1;
    }

    // 3. Create Hardware RenderDevice & RenderContext Subsystems (Milestone 03)
    EngineN::RenderDevice device;
    EngineN::RenderDeviceConfig renderConfig{
        .vsync = (config.targetRenderFps == 0) ? EngineN::VSyncMode::Adaptive : EngineN::VSyncMode::Off,
        .presentation = EngineN::PresentationMode::Letterbox,
        .virtualWidth = config.windowWidth,
        .virtualHeight = config.windowHeight,
        .clearColor = EngineN::Color::FromRGBA8(18, 18, 24, 255),
        .maxTextures = 1024
    };
    auto devRes = device.Create(window, renderConfig);
    if (!devRes) {
        ENGINE_LOG(Fatal, Render, "RenderDevice initialization failed: {}", devRes.Error().message);
        return -1;
    }

    EngineN::RenderContext ctx;
    auto ctxRes = ctx.Init(device);
    if (!ctxRes) {
        ENGINE_LOG(Fatal, Render, "RenderContext initialization failed: {}", ctxRes.Error().message);
        return -1;
    }

    // 4. Input & Event Pump Subsystems
    EngineN::InputSystem input;
    EngineN::EventPump pump;

    // 5. Unified 120Hz TimeManager
    EngineN::TimeManager time;
    time.Init({
        .simulationHz = config.simulationHz,
        .targetRenderFps = config.targetRenderFps,
        .maxCatchupTicks = config.maxCatchupTicks
    });

    DemoEntity player;
    player.Init(EngineN::Vec2{config.windowWidth * 0.5f - 24.0f, config.windowHeight * 0.5f - 24.0f});

    int framesThisSec = 0;
    uint64_t lastFpsTimeNs = SDL_GetTicksNS();
    double fpsCounter = 0.0;
    bool useInterpolation = true;
    bool bulletTime = false;
    bool running = true;
    float frameTimeHistory[60] = {};
    int historyIndex = 0;

    SetTargetFps(config.targetRenderFps, time, device, framesThisSec, lastFpsTimeNs);

    while (running) {
        // [TIME] Begin frame measurement
        time.BeginFrame();

        float currentFrameMs = static_cast<float>(time.GetLastRawDeltaMs());
        frameTimeHistory[historyIndex] = currentFrameMs;
        historyIndex = (historyIndex + 1) % 60;

        // [EVENT PUMP & INPUT] Drain SDL events once per frame
        pump.BeginFrame();
        input.BeginFrame();
        pump.PollAll(window, input);

        if (pump.WasQuitRequested() || input.IsActionJustPressed(EngineN::PlayerAction::MenuPause)) {
            running = false;
        }

        // [DIAGNOSTICS] FPS calculation
        framesThisSec++;
        uint64_t nowNs = SDL_GetTicksNS();
        uint64_t elapsedNs = (nowNs >= lastFpsTimeNs) ? (nowNs - lastFpsTimeNs) : 0ULL;
        if (elapsedNs >= 500'000'000ULL) {
            fpsCounter = static_cast<double>(framesThisSec) * 1'000'000'000.0 / static_cast<double>(elapsedNs);
            framesThisSec = 0;
            lastFpsTimeNs = nowNs;
        }

        // Toggle Fullscreen (F11)
        if (input.IsKeyJustPressed(SDL_SCANCODE_F11)) {
            auto newMode = (window.GetMode() == EngineN::WindowMode::Windowed)
                ? EngineN::WindowMode::BorderlessFullscreen
                : EngineN::WindowMode::Windowed;
            auto res = window.SetMode(newMode);
            (void)res;
        }

        // Interactive Hotkeys
        if (input.IsKeyJustPressed(SDL_SCANCODE_I)) {
            useInterpolation = !useInterpolation;
        }

        if (input.IsKeyJustPressed(SDL_SCANCODE_H)) {
            time.Hitstop().Request({.channel = EngineN::HitstopChannel::Player, .durationSec = 0.100});
        }

        if (input.IsKeyJustPressed(SDL_SCANCODE_B)) {
            bulletTime = !bulletTime;
            time.PlayerTime().SetTimeScale(bulletTime ? 0.25 : 1.0);
            time.WorldTime().SetTimeScale(bulletTime ? 0.25 : 1.0);
        }

        if (input.IsKeyJustPressed(SDL_SCANCODE_1)) SetTargetFps(60, time, device, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyJustPressed(SDL_SCANCODE_2)) SetTargetFps(120, time, device, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyJustPressed(SDL_SCANCODE_3)) SetTargetFps(144, time, device, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyJustPressed(SDL_SCANCODE_0)) SetTargetFps(0, time, device, framesThisSec, lastFpsTimeNs);

        // Diagnostic hotkeys (F-keys to avoid conflicting with gameplay: WASD, J, K, L)
        if (input.IsKeyJustPressed(SDL_SCANCODE_F1)) {
            static int s_logCounter = 1;
            if (s_logCounter % 3 == 1) {
                LOG_INFO("Live log test #{}: Player active at ({:.0f}, {:.0f})", s_logCounter++, player.position.x, player.position.y);
            } else if (s_logCounter % 3 == 2) {
                LOG_WARN("Live log test #{}: Resource usage threshold alert", s_logCounter++);
            } else {
                LOG_ERROR("Live log test #{}: Synthetic anomaly test", s_logCounter++);
            }
        }

        // Input Actions triggering combat feedback
        if (input.IsActionJustPressed(EngineN::PlayerAction::Attack)) {
            player.isAttacking = true;
            player.actionTimer = 0.15;
            time.Hitstop().Request({.channel = EngineN::HitstopChannel::Player, .durationSec = 0.050});
        }
        if (input.IsActionJustPressed(EngineN::PlayerAction::Dash)) {
            player.isDashing = true;
            player.actionTimer = 0.20;
        }

        // [SIMULATION] Deterministic Fixed Timestep 120Hz Loop
        while (time.ConsumeTick()) {
            double dt = time.PlayerTime().GetScaledDt();
            if (dt > 0.0) {
                float dtFloat = static_cast<float>(dt);
                player.prevPosition = player.position;

                // Query Virtual Axes (Gamepad Stick or WASD/Arrows)
                float moveX = input.GetAxis(EngineN::AxisId::MoveX);
                float moveY = input.GetAxis(EngineN::AxisId::MoveY);

                float currentSpeed = player.speed;
                if (player.isDashing) {
                    currentSpeed *= 2.2f;
                }

                EngineN::Vec2 moveDir{moveX, moveY};
                float lenSq = moveDir.x * moveDir.x + moveDir.y * moveDir.y;
                if (lenSq > 1.0f) {
                    float invLen = 1.0f / std::sqrt(lenSq);
                    moveDir.x *= invLen;
                    moveDir.y *= invLen;
                }

                if (lenSq > 0.0001f) {
                    player.velocity = moveDir * currentSpeed;
                } else if (!player.isDashing) {
                    player.velocity *= std::max(0.0f, 1.0f - 10.0f * dtFloat);
                }

                player.position += player.velocity * dtFloat;

                if (player.actionTimer > 0.0) {
                    player.actionTimer -= dt;
                    if (player.actionTimer <= 0.0) {
                        player.isAttacking = false;
                        player.isDashing = false;
                    }
                }

                // Bounds clamp within virtual arena (1280x720)
                float minX = 40.0f;
                float maxX = static_cast<float>(config.windowWidth) - 40.0f - player.size.x;
                float minY = 40.0f;
                float maxY = static_cast<float>(config.windowHeight) - 40.0f - player.size.y;

                if (player.position.x < minX) player.position.x = minX;
                if (player.position.x > maxX) player.position.x = maxX;
                if (player.position.y < minY) player.position.y = minY;
                if (player.position.y > maxY) player.position.y = maxY;
            }
            time.EndTick();
        }

        // [RENDER] Variable Rate Presentation with Sub-frame Alpha
        time.EndFrame();
        double alpha = time.GetAlpha();

        (void)device.BeginFrame();
        ctx.BeginFrame();

        // --------------------------------------------------------------------
        // Pass 1: World Entities & Arena Geometry (World Space)
        // --------------------------------------------------------------------
        ctx.SetPass(EngineN::RenderPass::World);
        ctx.SetLayer(0);

        // Arena background border
        const EngineN::FRect arenaRect{
            40.0f, 40.0f,
            static_cast<float>(config.windowWidth) - 80.0f,
            static_cast<float>(config.windowHeight) - 80.0f
        };
        ctx.Primitives().FillRect(arenaRect, EngineN::Color::FromRGBA8(35, 38, 52, 255));
        ctx.Primitives().StrokeRect(arenaRect, 2.0f, EngineN::Color::FromRGBA8(65, 72, 104, 255));

        // Arena grid lines
        ctx.Primitives().Grid(arenaRect, 80.0f, 1.0f, EngineN::Color{0.18f, 0.20f, 0.28f, 0.4f});

        // Visual position via sub-frame alpha interpolation
        EngineN::Vec2 visualPos = useInterpolation
            ? EngineN::Lerp(player.prevPosition, player.position, static_cast<float>(alpha))
            : player.position;

        EngineN::Color playerColor = EngineN::Color::FromRGBA8(92, 224, 153, 255); // Green: Normal
        if (time.Hitstop().IsFrozen(EngineN::HitstopChannel::Player)) {
            playerColor = EngineN::Color::FromRGBA8(255, 90, 95, 255); // Red: Hitstop Freeze
        } else if (player.isAttacking) {
            playerColor = EngineN::Color::FromRGBA8(255, 215, 0, 255); // Gold: Attack
        } else if (player.isDashing) {
            playerColor = EngineN::Color::FromRGBA8(180, 100, 255, 255); // Purple: Dash
        } else if (bulletTime) {
            playerColor = EngineN::Color::FromRGBA8(100, 220, 255, 255); // Cyan: Bullet-time
        }

        // Combat VFX: Dash ghost trail capsule
        if (player.isDashing) {
            EngineN::Vec2 trailStart = visualPos - (player.velocity * 0.04f);
            ctx.Primitives().FillCapsule(trailStart, visualPos, 20.0f, EngineN::Color{0.7f, 0.4f, 1.0f, 0.35f});
            ctx.Primitives().StrokeCapsule(trailStart, visualPos, 20.0f, 1.5f, EngineN::Color{0.8f, 0.5f, 1.0f, 0.7f});
        }

        // Player Box & Outline
        const EngineN::FRect boxRect{visualPos.x, visualPos.y, player.size.x, player.size.y};
        ctx.Primitives().FillRect(boxRect, playerColor);
        ctx.Primitives().StrokeRect(boxRect, 2.0f, EngineN::Color{1.0f, 1.0f, 1.0f, 0.8f});

        // Combat VFX: Attack slash circle
        if (player.isAttacking) {
            EngineN::Vec2 attackCenter{visualPos.x + player.size.x * 0.5f + 30.0f, visualPos.y + player.size.y * 0.5f};
            ctx.Primitives().FillCircle(attackCenter, 32.0f, EngineN::Color{1.0f, 0.85f, 0.1f, 0.45f});
            ctx.Primitives().StrokeCircle(attackCenter, 32.0f, 3.0f, EngineN::Color{1.0f, 0.95f, 0.4f, 0.9f});
        }

        // --------------------------------------------------------------------
        // Pass 2: Screen-Space HUD (Virtual UI Canvas)
        // --------------------------------------------------------------------
        ctx.SetScreenSpace();
        ctx.SetPass(EngineN::RenderPass::Ui);
        ctx.SetLayer(10);

        // 1. Top-Left Controls & Shortcuts Panel
        const EngineN::FRect controlsPanel{40.0f, 40.0f, 540.0f, 130.0f};
        ctx.Primitives().FillRect(controlsPanel, EngineN::Color{0.06f, 0.07f, 0.10f, 0.85f});
        ctx.Primitives().StrokeRect(controlsPanel, 1.5f, EngineN::Color{0.25f, 0.30f, 0.45f, 0.8f});

        ctx.Primitives().Text(Vec2{55.0f, 52.0f}, "--- ENGINEN METROIDVANIA RUNTIME ---", Color{0.4f, 0.8f, 1.0f, 1.0f});
        ctx.Primitives().Text(Vec2{55.0f, 70.0f}, "WASD / Stick: Move  |  J / Space: Attack  |  K / Shift: Dash  |  L: Parry", Colors::White);
        ctx.Primitives().Text(Vec2{55.0f, 88.0f}, "1/2/3/0: 60/120/144/Uncapped  |  H: Hitstop  |  B: Bullet-Time", Color{0.8f, 0.8f, 0.8f, 1.0f});
        ctx.Primitives().Text(Vec2{55.0f, 106.0f}, "I: Interpolation  |  F1: Test Log  |  F11: Fullscreen  |  Esc: Quit", Color{0.9f, 0.75f, 0.3f, 1.0f});

        // 2. Top-Right CPU Performance & Frame Spike Monitor
        const FRect perfPanel{960.0f, 40.0f, 280.0f, 160.0f};
        ctx.Primitives().FillRect(perfPanel, Color{0.06f, 0.07f, 0.10f, 0.88f});
        ctx.Primitives().StrokeRect(perfPanel, 1.5f, Color{0.20f, 0.50f, 0.35f, 0.8f});

        ctx.Primitives().Text(Vec2{975.0f, 50.0f}, "CPU & FRAME SPIKE MONITOR", Color{0.3f, 1.0f, 0.5f, 1.0f});

        int targetFps = time.Pacer().GetTargetFps();
        float targetBudgetMs = (targetFps > 0) ? (1000.0f / static_cast<float>(targetFps)) : 8.333f;
        float maxGraphScaleMs = std::max(33.333f, targetBudgetMs * 2.2f);

        char fpsText[64];
        std::snprintf(fpsText, sizeof(fpsText), "FPS: %.1f (Target: %d)", fpsCounter, targetFps);
        ctx.Primitives().Text(Vec2{975.0f, 68.0f}, fpsText, Colors::White);

        char frameText[64];
        std::snprintf(frameText, sizeof(frameText), "Frame: %.2f ms (Sim: %d ticks)", currentFrameMs, time.GetTicksThisFrame());
        const Color frameCol = (currentFrameMs > targetBudgetMs * 1.40f) ? Colors::Red
                             : (currentFrameMs > targetBudgetMs * 1.15f) ? Colors::Yellow
                                                                         : Color{0.3f, 1.0f, 0.5f, 1.0f};
        ctx.Primitives().Text(Vec2{975.0f, 86.0f}, frameText, frameCol);

        const auto& stats = ctx.Queue().GetStats();
        char statsText[64];
        std::snprintf(statsText, sizeof(statsText), "Batches: %u runs | %u verts", stats.runCount, stats.vertexCount);
        ctx.Primitives().Text(Vec2{975.0f, 104.0f}, statsText, Color{0.75f, 0.75f, 0.85f, 1.0f});

        // Real-time Sparkline Graph (60-frame history, dynamic budget line)
        const FRect graphArea{975.0f, 124.0f, 250.0f, 65.0f};
        ctx.Primitives().FillRect(graphArea, Color{0.02f, 0.02f, 0.04f, 0.95f});
        ctx.Primitives().StrokeRect(graphArea, 1.0f, Color{0.25f, 0.25f, 0.30f, 0.8f});

        // Target budget reference line (e.g. 16.6ms for 60fps, 8.33ms for 120fps)
        const f32 budgetY = 124.0f + 65.0f - std::clamp((targetBudgetMs / maxGraphScaleMs) * 65.0f, 4.0f, 62.0f);
        ctx.Primitives().Line(Vec2{975.0f, budgetY}, Vec2{1225.0f, budgetY}, 1.0f, Color{0.2f, 0.8f, 0.3f, 0.5f});

        // Draw 60 history bars
        for (int i = 0; i < 60; ++i) {
            int histIdx = (historyIndex + i) % 60;
            f32 ms = frameTimeHistory[histIdx];
            if (ms <= 0.01f) continue;

            f32 barHeight = std::clamp((ms / maxGraphScaleMs) * 65.0f, 2.0f, 63.0f);
            f32 barX = 976.0f + static_cast<f32>(i) * 4.12f;
            f32 barY = 124.0f + 65.0f - barHeight;

            Color barColor = (ms > targetBudgetMs * 1.40f) ? Color{1.0f, 0.2f, 0.2f, 1.0f} // Red: True Spike (>40% over target)
                           : (ms > targetBudgetMs * 1.15f) ? Color{1.0f, 0.85f, 0.2f, 1.0f} // Yellow: Slight Jitter (>15% over target)
                                                           : Color{0.2f, 0.9f, 0.4f, 0.9f}; // Green: Healthy

            ctx.Primitives().FillRect(FRect{barX, barY, 3.0f, barHeight}, barColor);
        }

        // 3. Bottom-Left Live Engine Log Feed Panel
        const FRect logPanel{40.0f, 525.0f, 620.0f, 155.0f};
        ctx.Primitives().FillRect(logPanel, Color{0.05f, 0.06f, 0.09f, 0.92f});
        ctx.Primitives().StrokeRect(logPanel, 1.5f, Color{0.30f, 0.35f, 0.50f, 0.8f});

        ctx.Primitives().Text(Vec2{55.0f, 537.0f}, "--- LIVE LOG FEED (Press 'F1' for test log) ---", Color{0.6f, 0.75f, 0.95f, 1.0f});

        LogRecord recentLogs[5];
        usize logCount = Logger::GetRecentLogs(recentLogs, 5);

        for (usize i = 0; i < logCount; ++i) {
            const auto& rec = recentLogs[i];
            Color lvlCol = Color{0.3f, 0.9f, 0.4f, 1.0f}; // Info: Green
            const char* lvlTag = "[INFO]";
            if (rec.level == LogLevel::Warn) {
                lvlCol = Color{1.0f, 0.85f, 0.2f, 1.0f}; // Warn: Yellow
                lvlTag = "[WARN]";
            } else if (rec.level >= LogLevel::Error) {
                lvlCol = Color{1.0f, 0.3f, 0.3f, 1.0f}; // Error/Fatal: Red
                lvlTag = "[FAIL]";
            } else if (rec.level == LogLevel::Debug) {
                lvlCol = Color{0.3f, 0.8f, 1.0f, 1.0f}; // Debug: Cyan
                lvlTag = "[DBUG]";
            }

            char logLine[256];
            std::snprintf(logLine, sizeof(logLine), "%s %.66s", lvlTag, rec.message);
            ctx.Primitives().Text(Vec2{55.0f, 558.0f + static_cast<f32>(i) * 20.0f}, logLine, lvlCol);
        }

        // Finalize batch and submit to RenderDevice
        (void)ctx.EndFrame();

        // Hardware swapchain present
        (void)device.Present();

        // [TIME] Frame Pacing delay
        time.Pacer().WaitForTarget();
    }

    device.Destroy();
    window.Destroy();
    platform.Shutdown();

    return 0;
}

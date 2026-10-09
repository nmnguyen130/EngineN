#include "core/time/Time.h"
#include "core/Config.h"
#include "core/Log.h"
#include "math/Vec2.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventPump.h"
#include "platform/input/InputSystem.h"
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

void SetTargetFps(int fps, EngineN::TimeLoop& time, SDL_Renderer* renderer, int& framesThisSec, uint64_t& lastFpsTimeNs) {
    time.Pacer().SetTargetFps(fps);
    framesThisSec = 0;
    lastFpsTimeNs = SDL_GetTicksNS();
    if (renderer) {
        if (fps == 0) {
            if (!SDL_SetRenderVSync(renderer, -1)) {
                SDL_SetRenderVSync(renderer, 1);
            }
        } else {
            SDL_SetRenderVSync(renderer, 0);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    EngineN::AppConfig config{
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

    // 3. Create Hardware Accelerated SDL Renderer for debug graphics
    SDL_Renderer* renderer = SDL_CreateRenderer(window.GetRawHandle(), nullptr);
    if (!renderer) {
        ENGINE_LOG(Fatal, Platform, "SDL_CreateRenderer failed: {}", SDL_GetError());
        return -1;
    }

    // 4. Input & Event Pump Subsystems
    EngineN::InputSystem input;
    EngineN::EventPump pump;

    // 5. Unified 120Hz TimeLoop
    EngineN::TimeLoop time;
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

    SetTargetFps(config.targetRenderFps, time, renderer, framesThisSec, lastFpsTimeNs);

    while (running) {
        // [TIME] Begin frame measurement
        time.BeginFrame();

        // [EVENT PUMP & INPUT] Drain SDL events once per frame
        pump.BeginFrame();
        input.BeginFrame();
        pump.PollAll(window, input);

        if (pump.WasQuitRequested() || input.IsActionJustPressed(EngineN::PlayerAction::MenuPause)) {
            running = false;
        }

        // Handle window refocus clock stabilization
        if (window.HasFocus() == false) {
            // Can pause or throttle if needed
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
        if (input.IsKeyDown(SDL_SCANCODE_F11)) {
            static bool s_f11Down = false;
            if (!s_f11Down) {
                s_f11Down = true;
                auto newMode = (window.GetMode() == EngineN::WindowMode::Windowed)
                    ? EngineN::WindowMode::BorderlessFullscreen
                    : EngineN::WindowMode::Windowed;
                auto res = window.SetMode(newMode);
                (void)res;
            }
        } else {
            static bool s_f11Down = false;
            s_f11Down = false;
        }

        // Interactive Hotkeys
        if (input.IsKeyDown(SDL_SCANCODE_I)) {
            static bool s_iDown = false;
            if (!s_iDown) {
                s_iDown = true;
                useInterpolation = !useInterpolation;
            }
        } else {
            static bool s_iDown = false;
            s_iDown = false;
        }

        if (input.IsKeyDown(SDL_SCANCODE_H)) {
            static bool s_hDown = false;
            if (!s_hDown) {
                s_hDown = true;
                time.Hitstop().Request({.channel = EngineN::HitstopChannel::Player, .freezeDurationSec = 0.100});
            }
        } else {
            static bool s_hDown = false;
            s_hDown = false;
        }

        if (input.IsKeyDown(SDL_SCANCODE_B)) {
            static bool s_bDown = false;
            if (!s_bDown) {
                s_bDown = true;
                bulletTime = !bulletTime;
                time.PlayerTime().SetTimeScale(bulletTime ? 0.25 : 1.0);
                time.WorldTime().SetTimeScale(bulletTime ? 0.25 : 1.0);
            }
        } else {
            static bool s_bDown = false;
            s_bDown = false;
        }

        if (input.IsKeyDown(SDL_SCANCODE_1)) SetTargetFps(60, time, renderer, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyDown(SDL_SCANCODE_2)) SetTargetFps(120, time, renderer, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyDown(SDL_SCANCODE_3)) SetTargetFps(144, time, renderer, framesThisSec, lastFpsTimeNs);
        if (input.IsKeyDown(SDL_SCANCODE_0)) SetTargetFps(0, time, renderer, framesThisSec, lastFpsTimeNs);

        // Input Actions triggering combat feedback
        if (input.IsActionJustPressed(EngineN::PlayerAction::Attack)) {
            player.isAttacking = true;
            player.actionTimer = 0.15;
            time.Hitstop().Request({.channel = EngineN::HitstopChannel::Player, .freezeDurationSec = 0.050});
        }
        if (input.IsActionJustPressed(EngineN::PlayerAction::Dash)) {
            player.isDashing = true;
            player.actionTimer = 0.20;
        }

        // [SIMULATION] Deterministic Fixed Timestep 120Hz Loop
        while (time.ConsumeTick()) {
            double dt = time.PlayerTime().scaledDt;
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

                // If input active, move player; else bounce gently
                if (std::abs(moveX) > 0.01f || std::abs(moveY) > 0.01f) {
                    player.velocity = EngineN::Vec2{moveX, moveY} * currentSpeed;
                } else if (!player.isDashing) {
                    // Friction damping towards zero
                    player.velocity *= std::max(0.0f, 1.0f - 10.0f * dtFloat);
                }

                player.position += player.velocity * dtFloat;

                // Action timer decrement
                if (player.actionTimer > 0.0) {
                    player.actionTimer -= dt;
                    if (player.actionTimer <= 0.0) {
                        player.isAttacking = false;
                        player.isDashing = false;
                    }
                }

                // Bounds clamp within window
                float arenaW = static_cast<float>(window.GetWidth());
                float arenaH = static_cast<float>(window.GetHeight());
                float minX = 40.0f;
                float maxX = arenaW - 40.0f - player.size.x;
                float minY = 40.0f;
                float maxY = arenaH - 40.0f - player.size.y;

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

        SDL_SetRenderDrawColor(renderer, 18, 18, 24, 255);
        SDL_RenderClear(renderer);

        // Arena border
        SDL_FRect arenaRect{
            40.0f, 40.0f,
            static_cast<float>(window.GetWidth()) - 80.0f,
            static_cast<float>(window.GetHeight()) - 80.0f
        };
        SDL_SetRenderDrawColor(renderer, 35, 38, 52, 255);
        SDL_RenderFillRect(renderer, &arenaRect);
        SDL_SetRenderDrawColor(renderer, 65, 72, 104, 255);
        SDL_RenderRect(renderer, &arenaRect);

        // Visual position via alpha interpolation
        EngineN::Vec2 visualPos = useInterpolation
            ? EngineN::Lerp(player.prevPosition, player.position, static_cast<float>(alpha))
            : player.position;

        SDL_FRect boxRect{visualPos.x, visualPos.y, player.size.x, player.size.y};
        if (time.Hitstop().IsFrozen(EngineN::HitstopChannel::Player)) {
            SDL_SetRenderDrawColor(renderer, 255, 90, 95, 255); // Red: Hitstop Freeze
        } else if (player.isAttacking) {
            SDL_SetRenderDrawColor(renderer, 255, 215, 0, 255); // Gold: Attack
        } else if (player.isDashing) {
            SDL_SetRenderDrawColor(renderer, 180, 100, 255, 255); // Purple: Dash
        } else if (bulletTime) {
            SDL_SetRenderDrawColor(renderer, 100, 220, 255, 255); // Cyan: Bullet-time
        } else {
            SDL_SetRenderDrawColor(renderer, 92, 224, 153, 255); // Green: Normal
        }
        SDL_RenderFillRect(renderer, &boxRect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
        SDL_RenderRect(renderer, &boxRect);

        // HUD overlay
        char fpsTargetBuf[32];
        if (time.Pacer().GetTargetFps() == 0) {
            snprintf(fpsTargetBuf, sizeof(fpsTargetBuf), "VSync (Auto)");
        } else {
            snprintf(fpsTargetBuf, sizeof(fpsTargetBuf), "%d FPS", time.Pacer().GetTargetFps());
        }

        const char* padStatus = input.HasGamepad() ? input.GetGamepad().GetName() : "None";

        char hudBuffer[1024];
        snprintf(hudBuffer, sizeof(hudBuffer),
            "%s - 120Hz Sim & Platform Subsystem\n"
            "Window: %dx%d (Pixel: %dx%d, DPI: %.1fx) | Mode: %s [F11: Toggle Fullscreen]\n"
            "FPS: %.1f | Raw Delta: %.2f ms | Ticks This Frame: %d | Total Ticks: %llu\n"
            "Gamepad: %s | Mouse: (%d, %d)\n"
            "Actions: Jump=%s Attack=%s Dash=%s Parry=%s [Ts Actions Queued: %u]\n"
            "Interpolation: %s [Press I to toggle]\n"
            "Hitstop: %s [Press H or Attack]\n"
            "Bullet-Time: %s [Press B to toggle]\n"
            "Target FPS: %s [Press 1=60, 2=120, 3=144, 0=VSync]\n"
            "Controls: WASD / Stick / Arrows to Move | Space/A: Jump | J/LeftClick/X: Attack | Shift/RB: Dash",
            config.appName,
            window.GetWidth(), window.GetHeight(), window.GetPixelWidth(), window.GetPixelHeight(),
            window.GetDPIScale(),
            window.GetMode() == EngineN::WindowMode::Windowed ? "Windowed" : "Borderless",
            fpsCounter,
            time.GetLastRawDeltaMs(),
            time.GetTicksThisFrame(),
            static_cast<unsigned long long>(time.GetSimTickCount()),
            padStatus, input.GetMouseX(), input.GetMouseY(),
            input.IsActionHeld(EngineN::PlayerAction::Jump) ? "HELD" : "IDLE",
            input.IsActionHeld(EngineN::PlayerAction::Attack) ? "HELD" : "IDLE",
            input.IsActionHeld(EngineN::PlayerAction::Dash) ? "HELD" : "IDLE",
            input.IsActionHeld(EngineN::PlayerAction::Parry) ? "HELD" : "IDLE",
            input.GetTimestampedActionCount(),
            useInterpolation ? "ON (Lerp)" : "OFF (Snap)",
            time.Hitstop().IsFrozen(EngineN::HitstopChannel::Player) ? "FROZEN" : "ACTIVE",
            bulletTime ? "ON (0.25x)" : "OFF (1.0x)",
            fpsTargetBuf
        );

        SDL_SetRenderDrawColor(renderer, 240, 240, 245, 255);
        SDL_RenderDebugText(renderer, 50.0f, 50.0f, hudBuffer);

        SDL_RenderPresent(renderer);

        // [TIME] Frame Pacing delay
        time.Pacer().WaitForTarget();
    }

    SDL_DestroyRenderer(renderer);
    window.Destroy();
    platform.Shutdown();

    return 0;
}

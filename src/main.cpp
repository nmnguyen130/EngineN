#include "core/Config.h"
#include "core/Log.h"
#include "core/time/TimeManager.h"
#include "math/Vec2.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventRouting.h"
#include "platform/input/InputSystem.h"
#include "renderer/RendererTypes.h"
#include "renderer/QuadInstance.h"
#include "renderer/SortKey.h"
#include "renderer/FramePacket.h"
#include "renderer/FrameArena.h"
#include "renderer/Batcher.h"
#include "renderer/RenderDevice.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

struct PlayerEntity {
    EngineN::Vec2 prevPosition{640.0f, 360.0f};
    EngineN::Vec2 position{640.0f, 360.0f};
    EngineN::Vec2 velocity{0.0f, 0.0f};
    EngineN::Vec2 size{48.0f, 48.0f};
    float speed{420.0f};
    bool isAttacking{false};
    bool isDashing{false};
    double actionTimer{0.0};
};

enum class GameAction : EngineN::ActionId {
    Jump = 0,
    Attack = 1,
    Dash = 2,
    Sprint = 3,
    MenuPause = 4,
    Confirm = 5
};

void SetTargetFps(int fps, EngineN::TimeManager& time, EngineN::RenderDevice& device) {
    if (time.Pacer().GetTargetFps() == fps) {
        return;
    }
    time.Pacer().SetTargetFps(fps);
    (void)device.SetVSync(fps == 0);
    LOG_INFO("Target FPS set to {} (VSync: {})", fps, (fps == 0 ? "ON" : "OFF"));
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
    Platform platform;
    auto platRes = platform.Init();
    if (!platRes) {
        LOG_FATAL("Platform initialization failed: {}", platRes.Error().message);
        return -1;
    }

    // 2. Create Engine Window
    WindowConfig winConfig{
        .title = config.appName,
        .size = {config.windowWidth, config.windowHeight},
        .mode = WindowMode::Windowed,
        .highDPI = true,
        .resizable = true
    };
    auto winRes = Window::Create(winConfig, platform.Displays());
    if (!winRes) {
        LOG_FATAL("Window creation failed: {}", winRes.Error().message);
        return -1;
    }
    Window window = std::move(*winRes);

    // 3. Initialize Hardware RenderDevice
    RenderDevice device;
    auto devRes = device.Init(window, {.vsync = (config.targetRenderFps == 0)});
    if (!devRes) {
        LOG_FATAL("RenderDevice initialization failed: {}", devRes.Error().message);
        return -1;
    }

    // 4. Input Subsystem & Raw Scancode Watcher
    InputSystem input;
    std::array<bool, 512> keysJustPressed{};

    struct KeyWatcher {
        std::array<bool, 512>* keys{nullptr};
        SinkVerdict OnEvent(const SDL_Event& e) noexcept {
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode < 512) {
                (*keys)[e.key.scancode] = true;
            }
            return SinkVerdict::Continue;
        }
    } keyWatcher{&keysJustPressed};

    (void)platform.Router().Add(
        SinkStage::App,
        EventSinkRef::Bind<&KeyWatcher::OnEvent>(keyWatcher)
    );

    // 5. Deterministic 120Hz Time Manager
    TimeManager time;
    time.Init({
        .simulationHz = config.simulationHz,
        .targetRenderFps = config.targetRenderFps,
        .maxCatchupTicks = config.maxCatchupTicks
    });

    // 6. Double-Buffered Zero-Allocation Frame Arena & Batcher
    FrameArena arena;
    Batcher batcher;
    FramePacket packet;

    // Entity State
    PlayerEntity player;
    player.position = {config.windowWidth * 0.5f - 24.0f, config.windowHeight * 0.5f - 24.0f};
    player.prevPosition = player.position;

    bool running = true;
    bool useInterpolation = true;
    bool bulletTime = false;

    int framesThisSec = 0;
    std::uint64_t lastTelemetryNs = SDL_GetTicksNS();

    LOG_INFO("EngineN initialized successfully at 120Hz fixed simulation.");

    while (running) {
        // [TIME] Begin frame measurement
        time.BeginFrame();

        // [PLATFORM & INPUT] Single-source SDL event pumping
        keysJustPressed.fill(false);
        (void)platform.Pump(window, input);
        const auto& frameInput = input.PublishFrameInput();

        if (platform.WasQuitRequested() || frameInput.Pressed(GameAction::MenuPause)) {
            running = false;
        }

        // Toggle Fullscreen (F11)
        if (keysJustPressed[SDL_SCANCODE_F11]) {
            const auto newMode = (window.GetMode() == WindowMode::Windowed)
                ? WindowMode::BorderlessFullscreen
                : WindowMode::Windowed;
            (void)window.SetMode(newMode);
        }

        // Hotkeys for simulation & performance diagnostics
        if (keysJustPressed[SDL_SCANCODE_I]) {
            useInterpolation = !useInterpolation;
            LOG_INFO("Sub-frame visual interpolation: {}", useInterpolation ? "ON" : "OFF");
        }
        if (keysJustPressed[SDL_SCANCODE_H]) {
            time.Hitstop().Request({.channel = HitstopChannel::Player, .durationSec = 0.080});
        }
        if (keysJustPressed[SDL_SCANCODE_B]) {
            bulletTime = !bulletTime;
            const double scale = bulletTime ? 0.25 : 1.0;
            time.PlayerTime().SetTimeScale(scale);
            time.WorldTime().SetTimeScale(scale);
            LOG_INFO("Bullet-time {}: time scale = {:.2f}x", bulletTime ? "enabled" : "disabled", scale);
        }
        if (keysJustPressed[SDL_SCANCODE_1]) SetTargetFps(60, time, device);
        if (keysJustPressed[SDL_SCANCODE_2]) SetTargetFps(120, time, device);
        if (keysJustPressed[SDL_SCANCODE_3]) SetTargetFps(144, time, device);
        if (keysJustPressed[SDL_SCANCODE_0]) SetTargetFps(0, time, device);
        if (keysJustPressed[SDL_SCANCODE_F1]) {
            LOG_INFO("Live diagnostic pulse: Player at ({:.1f}, {:.1f})", player.position.x, player.position.y);
        }

        // Combat trigger actions
        if (frameInput.Pressed(GameAction::Attack)) {
            player.isAttacking = true;
            player.actionTimer = 0.15;
            time.Hitstop().Request({.channel = HitstopChannel::Player, .durationSec = 0.040});
        }
        if (frameInput.Pressed(GameAction::Dash) || frameInput.Pressed(GameAction::Sprint)) {
            player.isDashing = true;
            player.actionTimer = 0.20;
        }

        // [SIMULATION] Deterministic Fixed Timestep 120Hz Loop
        while (time.ConsumeTick()) {
            TickInput tickInput = input.CutTick();
            const double dt = time.PlayerTime().GetScaledDt();
            if (dt > 0.0) {
                const float dtFloat = static_cast<float>(dt);
                player.prevPosition = player.position;

                // Virtual Axes: 0 = MoveX (A/D), 1 = MoveY (W/S)
                Vec2 moveDir{tickInput.Axis(0), tickInput.Axis(1)};
                const float lenSq = moveDir.LengthSquared();
                if (lenSq > 1.0f) {
                    moveDir = moveDir.Normalized();
                }

                const float currentSpeed = player.speed * (player.isDashing ? 2.2f : 1.0f);
                if (lenSq > 0.0001f) {
                    player.velocity = moveDir * currentSpeed;
                } else if (!player.isDashing) {
                    player.velocity = player.velocity * std::max(0.0f, 1.0f - 12.0f * dtFloat);
                }

                player.position += player.velocity * dtFloat;

                if (player.actionTimer > 0.0) {
                    player.actionTimer -= dt;
                    if (player.actionTimer <= 0.0) {
                        player.isAttacking = false;
                        player.isDashing = false;
                    }
                }

                // Arena boundary constraints (40px margin)
                constexpr float kMargin = 40.0f;
                const float minX = kMargin;
                const float maxX = static_cast<float>(config.windowWidth) - kMargin - player.size.x;
                const float minY = kMargin;
                const float maxY = static_cast<float>(config.windowHeight) - kMargin - player.size.y;
                player.position.x = std::clamp(player.position.x, minX, maxX);
                player.position.y = std::clamp(player.position.y, minY, maxY);
            }
            time.EndTick();
        }

        // [RENDER] Variable Rate Presentation with Sub-frame Interpolation Alpha
        time.EndFrame();
        const double alpha = time.GetAlpha();

        auto& slot = arena.CurrentSlot();
        slot.Reset();

        // Configure Primary RenderPass and ViewSlot
        (void)slot.PushPass(RenderPassDesc{
            .pass = RenderPass::Scene,
            .enabled = true,
            .clearColor = Color::FromRGBA8(18, 18, 24, 255)
        });
        (void)slot.PushViewSlot(ViewSlotDesc{
            .viewport = {0.0f, 0.0f, static_cast<f32>(config.windowWidth), static_cast<f32>(config.windowHeight)}
        });

        // Compute visually interpolated position
        const Vec2 visualPos = useInterpolation
            ? Lerp(player.prevPosition, player.position, static_cast<float>(alpha))
            : player.position;

        // 1. Arena Background
        QuadInstance bgQuad{};
        bgQuad.pos[0] = static_cast<f32>(config.windowWidth) * 0.5f;
        bgQuad.pos[1] = static_cast<f32>(config.windowHeight) * 0.5f;
        bgQuad.size[0] = static_cast<f32>(config.windowWidth) - 80.0f;
        bgQuad.size[1] = static_cast<f32>(config.windowHeight) - 80.0f;
        bgQuad.SetColor(Color::FromRGBA8(30, 33, 44, 255));
        (void)slot.PushQuad(bgQuad, SortKey{SortKey::Pack(0, 0, 10, static_cast<u8>(BlendMode::Opaque), 0, 0)}, 0);

        // 2. Dash Motion Ghost (Additive VFX)
        if (player.isDashing) {
            QuadInstance trailQuad{};
            const Vec2 trailPos = visualPos - (player.velocity * 0.035f);
            trailQuad.pos[0] = trailPos.x + player.size.x * 0.5f;
            trailQuad.pos[1] = trailPos.y + player.size.y * 0.5f;
            trailQuad.size[0] = player.size.x * 1.15f;
            trailQuad.size[1] = player.size.y * 1.15f;
            trailQuad.SetColor(Color{0.7f, 0.4f, 1.0f, 0.45f});
            (void)slot.PushQuad(trailQuad, SortKey{SortKey::Pack(0, 0, 40, static_cast<u8>(BlendMode::Additive), 0, 0)}, 0);
        }

        // 3. Player Sprite Quad (State-reactive coloring)
        Color playerColor = Color::FromRGBA8(92, 224, 153, 255); // Green: Normal
        if (time.Hitstop().IsFrozen(HitstopChannel::Player)) {
            playerColor = Color::FromRGBA8(255, 90, 95, 255);    // Red: Hitstop Freeze
        } else if (player.isAttacking) {
            playerColor = Color::FromRGBA8(255, 215, 0, 255);   // Gold: Attack
        } else if (player.isDashing) {
            playerColor = Color::FromRGBA8(180, 100, 255, 255); // Purple: Dash
        } else if (bulletTime) {
            playerColor = Color::FromRGBA8(100, 220, 255, 255); // Cyan: Bullet Time
        }

        QuadInstance playerQuad{};
        playerQuad.pos[0] = visualPos.x + player.size.x * 0.5f;
        playerQuad.pos[1] = visualPos.y + player.size.y * 0.5f;
        playerQuad.size[0] = player.size.x;
        playerQuad.size[1] = player.size.y;
        playerQuad.SetColor(playerColor);
        (void)slot.PushQuad(playerQuad, SortKey{SortKey::Pack(0, 0, 50, static_cast<u8>(BlendMode::Premultiplied), 0, 0)}, 0);

        // 4. Attack Slash Effect
        if (player.isAttacking) {
            QuadInstance attackQuad{};
            attackQuad.pos[0] = visualPos.x + player.size.x * 0.5f + 36.0f;
            attackQuad.pos[1] = visualPos.y + player.size.y * 0.5f;
            attackQuad.size[0] = 32.0f;
            attackQuad.size[1] = 48.0f;
            attackQuad.SetColor(Color{1.0f, 0.85f, 0.1f, 0.75f});
            (void)slot.PushQuad(attackQuad, SortKey{SortKey::Pack(0, 0, 60, static_cast<u8>(BlendMode::Additive), 0, 0)}, 0);
        }

        // Finalize batches via Radix Sort and State Coalescence
        batcher.Finalize(slot, packet);

        // Hardware submission of FramePacket & swapchain present
        (void)device.Submit(packet);

        arena.AdvanceSlot();

        // 1-second interval diagnostics telemetry
        framesThisSec++;
        const std::uint64_t nowNs = SDL_GetTicksNS();
        if (nowNs - lastTelemetryNs >= 1'000'000'000ULL) {
            const double fps = static_cast<double>(framesThisSec) * 1'000'000'000.0 /
                               static_cast<double>(nowNs - lastTelemetryNs);
            LOG_INFO("FPS: {:.1f} | Frame: {:.2f} ms | Sim Ticks: {} | Batches: {} | Quads: {}",
                fps,
                time.GetLastRawDeltaMs(),
                time.GetSimTickCount(),
                packet.batches.size(),
                packet.instances.size()
            );
            framesThisSec = 0;
            lastTelemetryNs = nowNs;
        }

        // Frame Pacer wait (for targeted framerates)
        time.Pacer().WaitForTarget();
    }

    // Clean shutdown
    device.Shutdown();
    window.Destroy();
    platform.Shutdown();

    return 0;
}

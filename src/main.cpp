#include "core/Config.h"
#include "core/Log.h"
#include "core/EngineContext.h"
#include "core/GameLoop.h"
#include "core/time/TimeManager.h"
#include "core/time/TickContext.h"
#include "core/time/SimEvents.h"
#include "core/interpolation/Interpolated.h"
#include "math/Vec2.h"
#include "platform/Platform.h"
#include "platform/Window.h"
#include "platform/EventRouting.h"
#include "platform/input/InputSystem.h"
#include "asset/AssetManager.h"
#include "asset/AssetSet.h"
#include "renderer/RendererTypes.h"
#include "renderer/FrameArena.h"
#include "renderer/RenderQueue.h"
#include "renderer/Batcher.h"
#include "renderer/RenderDevice.h"
#include "renderer/Presenter.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

void EnsureDemoTexturesExist() {
    std::filesystem::create_directories("assets/textures");
    const std::string sheetPath = "assets/textures/hero_sheet.tga";
    if (std::filesystem::exists(sheetPath)) {
        return;
    }

    constexpr int kFrameWidth = 32;
    constexpr int kFrameHeight = 32;
    constexpr int kFrameCount = 4;
    constexpr int kTotalWidth = kFrameWidth * kFrameCount;
    constexpr int kTotalHeight = kFrameHeight;

    std::vector<EngineN::u8> pixels(kTotalWidth * kTotalHeight * 4, 0);

    for (int frame = 0; frame < kFrameCount; ++frame) {
        const int xOffset = frame * kFrameWidth;
        const int yBob = (frame == 1 || frame == 3) ? -1 : 0;

        for (int y = 0; y < kFrameHeight; ++y) {
            for (int x = 0; x < kFrameWidth; ++x) {
                const int globalX = xOffset + x;
                const int globalY = y;
                const int idx = (globalY * kTotalWidth + globalX) * 4;
                const int localY = y - yBob;

                // 1px Border
                if (x == 0 || x == 31 || y == 0 || y == 31) {
                    pixels[idx + 0] = 20;  pixels[idx + 1] = 24;  pixels[idx + 2] = 34;  pixels[idx + 3] = 255;
                }
                // Gold helmet
                else if (localY >= 4 && localY <= 12 && x >= 8 && x <= 23) {
                    if (localY >= 8 && localY <= 10 && x >= 11 && x <= 20) {
                        const EngineN::u8 visorR = (frame == 2) ? 255 : 80;
                        const EngineN::u8 visorG = (frame == 2) ? 120 : 240;
                        pixels[idx + 0] = visorR; pixels[idx + 1] = visorG; pixels[idx + 2] = 255; pixels[idx + 3] = 255;
                    } else {
                        pixels[idx + 0] = 255; pixels[idx + 1] = 200; pixels[idx + 2] = 40;  pixels[idx + 3] = 255;
                    }
                }
                // Cobalt blue chestplate
                else if (localY >= 13 && localY <= 22 && x >= 6 && x <= 25) {
                    const bool isTrim = (x == 6 || x == 25 || localY == 13 || localY == 22 || x == 15 || x == 16);
                    if (isTrim) {
                        pixels[idx + 0] = 220; pixels[idx + 1] = 230; pixels[idx + 2] = 255; pixels[idx + 3] = 255;
                    } else {
                        pixels[idx + 0] = 40;  pixels[idx + 1] = 110; pixels[idx + 2] = 240; pixels[idx + 3] = 255;
                    }
                }
                // Silver greaves
                else if (localY >= 23 && localY <= 29) {
                    bool isLeg = false;
                    if (frame == 0)      isLeg = (x >= 9 && x <= 13) || (x >= 18 && x <= 22);
                    else if (frame == 1) isLeg = (x >= 7 && x <= 12) || (x >= 18 && x <= 24);
                    else if (frame == 2) isLeg = (x >= 9 && x <= 14) || (x >= 17 && x <= 22);
                    else                 isLeg = (x >= 8 && x <= 14) || (x >= 20 && x <= 25);

                    if (isLeg) {
                        pixels[idx + 0] = 160; pixels[idx + 1] = 170; pixels[idx + 2] = 190; pixels[idx + 3] = 255;
                    }
                }
            }
        }
    }

    std::FILE* fp = std::fopen(sheetPath.c_str(), "wb");
    if (fp) {
        const EngineN::u8 header[18] = {
            0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            static_cast<EngineN::u8>(kTotalWidth & 0xFF), static_cast<EngineN::u8>((kTotalWidth >> 8) & 0xFF),
            static_cast<EngineN::u8>(kTotalHeight & 0xFF), static_cast<EngineN::u8>((kTotalHeight >> 8) & 0xFF),
            32, 8 | 0x20
        };
        std::fwrite(header, 1, sizeof(header), fp);
        std::vector<EngineN::u8> bgra(pixels.size());
        for (EngineN::usize i = 0; i < pixels.size(); i += 4) {
            bgra[i + 0] = pixels[i + 2];
            bgra[i + 1] = pixels[i + 1];
            bgra[i + 2] = pixels[i + 0];
            bgra[i + 3] = pixels[i + 3];
        }
        std::fwrite(bgra.data(), 1, bgra.size(), fp);
        std::fclose(fp);
    }
}

enum class GameAction : EngineN::ActionId {
    Jump = 0,
    Attack = 1,
    Dash = 2,
    Sprint = 3,
    MenuPause = 4,
    Confirm = 5
};

enum class GameChannel : EngineN::u8 {
    World = 0,
    Player = 1,
    Enemy = 2
};

enum class PlayerState : EngineN::u8 {
    Idle = 0,
    Run,
    Attack,
    Dash
};

struct PlayerSim {
    EngineN::Interpolated<EngineN::Vec2> pos{EngineN::Vec2{640.0f, 360.0f}};
    EngineN::Vec2 vel{};
    EngineN::Vec2 size{48.0f, 48.0f};
    PlayerState   state = PlayerState::Idle;
    EngineN::u32  stateTicks = 0; // Entity-owned clock
    float         speed = 420.0f;
    bool          facingLeft = false;

    void BeginTick() noexcept {
        pos.BeginTick(); // Unconditional snapshot guarantees stillness during hitstop
    }

    void Tick(const EngineN::TickInput& in, EngineN::TickContext& ctx) noexcept {
        using namespace EngineN;
        const f32 dt = ctx.Dt(GameChannel::Player);
        if (dt <= 0.0f) {
            return; // Frozen: pos.prev == pos.cur already, zero drift
        }
        ++stateTicks;

        // Combat triggers from TICK input stream (deterministic and replayable)
        if (state != PlayerState::Attack && in.Pressed(GameAction::Attack)) {
            state = PlayerState::Attack;
            stateTicks = 0;
            ctx.RequestHitstop(GameChannel::Player, 3_f60); // 6 ticks exact freeze
            ctx.Emit({SimEventKind::AttackStarted, static_cast<u32>(ctx.TickIndex()), pos.Cur(), 1.0f});
        } else if (state != PlayerState::Dash && (in.Pressed(GameAction::Dash) || in.Pressed(GameAction::Sprint))) {
            state = PlayerState::Dash;
            stateTicks = 0;
            ctx.Emit({SimEventKind::DashStarted, static_cast<u32>(ctx.TickIndex()), pos.Cur(), 1.0f});
        }

        // Virtual axis movement
        Vec2 moveDir{in.Axis(0), in.Axis(1)};
        const float lenSq = moveDir.LengthSquared();
        if (lenSq > 1.0f) {
            moveDir = moveDir.Normalized();
        }

        if (moveDir.x < -0.1f) facingLeft = true;
        else if (moveDir.x > 0.1f) facingLeft = false;

        const float currentSpeed = speed * (state == PlayerState::Dash ? 2.2f : 1.0f);
        if (lenSq > 0.0001f) {
            vel = moveDir * currentSpeed;
            if (state == PlayerState::Idle) state = PlayerState::Run;
        } else if (state != PlayerState::Dash) {
            vel = vel * std::max(0.0f, 1.0f - 12.0f * dt);
            if (state == PlayerState::Run) state = PlayerState::Idle;
        }

        pos.Cur() += vel * dt;

        constexpr u32 kAttackTicks = 18_f60;
        constexpr u32 kDashTicks   = 12_f60;
        if ((state == PlayerState::Attack && stateTicks >= kAttackTicks) ||
            (state == PlayerState::Dash   && stateTicks >= kDashTicks)) {
            state = (lenSq > 0.0001f) ? PlayerState::Run : PlayerState::Idle;
        }

        // World boundaries in virtual screen space (1280x720)
        constexpr float kMargin = 40.0f;
        pos.Cur().x = std::clamp(pos.Cur().x, kMargin, 1280.0f - kMargin - size.x);
        pos.Cur().y = std::clamp(pos.Cur().y, kMargin, 720.0f - kMargin - size.y);
    }
};

struct World {
    PlayerSim player;

    void BeginTick() noexcept {
        player.BeginTick();
    }

    void Tick(const EngineN::TickInput& in, EngineN::TickContext& ctx) noexcept {
        player.Tick(in, ctx);
    }
};

class DemoGame {
public:
    explicit DemoGame(EngineN::EngineContext& ctx) noexcept : m_ctx(ctx) {}

    EngineN::Result<void, EngineN::EngineError> Init() noexcept {
        using namespace EngineN;
        EnsureDemoTexturesExist();

        m_levelAssets = std::make_unique<AssetSet>(*m_ctx.assets);
        auto sheet = m_levelAssets->LoadTexture("assets/textures/hero_sheet.tga");
        if (!sheet) {
            return Err(sheet.Error());
        }

        AtlasGridDesc desc{
            .texture = *sheet,
            .frameWidth = 32,
            .frameHeight = 32,
            .frameCount = 4,
            .pivotX = 16,
            .pivotY = 16
        };
        auto atlas = m_levelAssets->LoadAtlasGrid("atlases/hero", desc);
        if (!atlas) {
            return Err(atlas.Error());
        }
        m_heroAtlas = *atlas;

        return {};
    }

    void Shutdown() noexcept {
        if (m_levelAssets) {
            m_levelAssets->Release();
            m_levelAssets.reset();
        }
    }

    void OnFrameInput(const EngineN::FrameInput& fi) noexcept {
        (void)fi;
    }

    void BeginTick() noexcept {
        m_world.BeginTick();
    }

    void Tick(const EngineN::TickInput& in, EngineN::TickContext& ctx) noexcept {
        m_world.Tick(in, ctx);
    }

    void Update(double realDt, float alpha, const EngineN::SimEvents& events) noexcept {
        (void)realDt;
        (void)alpha;
        (void)events;
    }

    void Extract(EngineN::RenderQueue& q, float alpha, const EngineN::AssetReader& assets) noexcept {
        using namespace EngineN;
        q.Begin(ViewTransform2D{}, Color::FromRGBA8(18, 18, 24, 255));

        const Vec2 p = m_world.player.pos.Get(alpha);

        // 1. Dash trail ghost (additive VFX, shares premultiplied batch)
        if (m_world.player.state == PlayerState::Dash) {
            q.SetLayer(Layers::Fx);
            (void)q.Sprite({
                .sprite = assets.Sprite({m_heroAtlas, 1}),
                .pos = p + m_world.player.vel * -0.035f + m_world.player.size * 0.5f,
                .scale = {1.6f, 1.6f},
                .tint = Color{0.7f, 0.4f, 1.0f, 0.45f},
                .blend = Blend::Additive
            });
        }

        // 2. Player animated sprite
        q.SetLayer(Layers::Actors);
        u16 animFrame = 0;
        if (m_world.player.state == PlayerState::Attack) animFrame = 2;
        else if (m_world.player.state == PlayerState::Dash) animFrame = 1;
        else if (m_world.player.state == PlayerState::Run) animFrame = static_cast<u16>((m_world.player.stateTicks / 15) % 4);

        Color playerTint = Colors::White;
        if (m_ctx.time->Hitstop().IsFrozen(GameChannel::Player)) {
            playerTint = Color::FromRGBA8(255, 90, 95, 255);
        } else if (m_world.player.state == PlayerState::Attack) {
            playerTint = Color::FromRGBA8(255, 215, 0, 255);
        } else if (m_world.player.state == PlayerState::Dash) {
            playerTint = Color::FromRGBA8(180, 100, 255, 255);
        }

        (void)q.Sprite({
            .sprite = assets.Sprite({m_heroAtlas, animFrame}),
            .pos = p + m_world.player.size * 0.5f,
            .scale = {1.5f, 1.5f},
            .tint = playerTint,
            .flipX = m_world.player.facingLeft,
            .sortY = p.y
        });
    }

private:
    EngineN::EngineContext&            m_ctx;
    World                              m_world;
    std::unique_ptr<EngineN::AssetSet> m_levelAssets;
    EngineN::AssetHandle<EngineN::Atlas> m_heroAtlas{};
};

static_assert(EngineN::GameLoop<DemoGame>);

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    using namespace EngineN;

    // 1. Platform Subsystem
    Platform platform;
    auto platRes = platform.Init();
    if (!platRes) {
        LOG_FATAL("Platform initialization failed: {}", platRes.Error().message);
        return -1;
    }

    // 2. Window Subsystem
    WindowConfig winConfig{
        .title = "EngineN — Metroidvania Runtime",
        .size = {1280, 720},
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

    // 3. Hardware RenderDevice
    RenderDevice device;
    auto devRes = device.Init(window, {.vsync = true});
    if (!devRes) {
        LOG_FATAL("RenderDevice initialization failed: {}", devRes.Error().message);
        return -1;
    }

    // 4. Asset Subsystem
    AssetManager assets;
    auto assetRes = assets.Init(device, {.textureCapacity = 128});
    if (!assetRes) {
        LOG_FATAL("AssetManager initialization failed: {}", assetRes.Error().message);
        return -1;
    }

    // 5. Input Subsystem
    InputSystem input;

    // 6. Deterministic 120Hz Time Manager
    TimeManager time;
    time.Init({.maxCatchupTicks = 8});

    // 7. Render Subsystems
    FrameArena arena;
    RenderQueue queue{arena.CurrentSlot()};
    Batcher batcher;
    Presenter presenter;
    SimEvents events;

    EngineContext ctx{
        .platform  = &platform,
        .window    = &window,
        .input     = &input,
        .time      = &time,
        .assets    = &assets,
        .arena     = &arena.CurrentSlot(),
        .queue     = &queue,
        .batcher   = &batcher,
        .backend   = &device,
        .presenter = &presenter,
        .events    = &events
    };

    DemoGame game{ctx};
    if (auto r = game.Init(); !r) {
        LOG_FATAL("Game initialization failed: {}", r.Error().message);
        return -1;
    }

    LOG_INFO("EngineN initialized successfully at 120Hz deterministic simulation.");

    // Primary 14-Step Frame Execution Loop
    while (!platform.WasQuitRequested()) {
        if (auto r = RunFrame(ctx, game); !r) {
            LOG_FATAL("Frame execution aborted: {}", r.Error().message);
            break;
        }
    }

    // Reverse-order teardown
    game.Shutdown();
    assets.Shutdown();
    device.Shutdown();
    platform.Shutdown();

    return 0;
}

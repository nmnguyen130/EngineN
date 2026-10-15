#include "debug/DebugSystem.h"

#include "assets/AssetBank.h"
#include "audio/AudioSystem.h"
#include "gameplay/GameplayPipeline.h"
#include "platform/input/ActionBuffer.h"
#include "platform/input/InputPipeline.h"
#include "renderer/Renderer.h"
#include "renderer/RendererList.h"
#include "world/World.h"

#include <cstdlib>
#include <new>

namespace en::debug {

#if EN_ENABLE_DEBUG_TOOLS

namespace {

void CmdHelp(void* /*ctx*/, const ConsoleArgs& /*args*/, DebugConsole& console) noexcept {
    console.Print(LogSeverity::Info, "Registered Commands & CVars:");
    for (const ConsoleSymbol& sym : console.Symbols()) {
        if (sym.kind != SymbolKind::Empty) {
            console.Print(LogSeverity::Info, std::string_view(sym.namePtr, sym.nameLen));
        }
    }
}

void CmdClear(void* /*ctx*/, const ConsoleArgs& /*args*/, DebugConsole& console) noexcept {
    console.ClearLog();
}

void CmdOverlay(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr || args.Count() < 2) {
        console.Print(LogSeverity::Warning, "Usage: overlay <all|none|bounds|hitboxes|prof|time|input|scrub>");
        return;
    }
    const std::string_view cat = args[1];
    OverlayFlags cur = sys->ActiveOverlays();
    if (cat == "all")         cur = OverlayFlags::AllWorld | OverlayFlags::AllHud;
    else if (cat == "none")   cur = OverlayFlags::None;
    else if (cat == "bounds") cur = cur ^ (OverlayFlags::KinematicBounds | OverlayFlags::VelocityVectors);
    else if (cat == "hitboxes") cur = cur ^ OverlayFlags::CombatHitboxes;
    else if (cat == "prof")   cur = cur ^ OverlayFlags::ProfilerHud;
    else if (cat == "time")   cur = cur ^ OverlayFlags::TimeChannelHud;
    else if (cat == "input")  cur = cur ^ OverlayFlags::InputViewerHud;
    else if (cat == "scrub")  cur = cur ^ OverlayFlags::FlightScrubber;
    else {
        console.Print(LogSeverity::Error, "Unknown overlay category.");
        return;
    }
    sys->SetActiveOverlays(cur);
    console.Print(LogSeverity::Success, "Overlay mask updated.");
}

void CmdTimePause(void* ctx, const ConsoleArgs& /*args*/, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys != nullptr) {
        sys->RequestPauseToggle();
        console.Print(LogSeverity::Success, "Toggled simulation pause.");
    }
}

void CmdTimeStep(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr) return;
    u16 ticks = 1;
    if (args.Count() >= 2) {
        if (auto v = args.GetInt(1)) {
            ticks = static_cast<u16>(std::clamp<i64>(*v, 1, 1200));
        }
    }
    sys->RequestSteps(ticks);
    console.Print(LogSeverity::Success, "Stepping simulation ticks.");
}

void CmdWorldTp(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr || sys->ActiveWorld() == nullptr || args.Count() < 4) {
        console.Print(LogSeverity::Warning, "Usage: world.tp <entityIdx> <x> <y>");
        return;
    }
    const auto idxRes = args.GetInt(1);
    const auto xRes   = args.GetFloat(2);
    const auto yRes   = args.GetFloat(3);
    if (!idxRes || !xRes || !yRes) {
        console.Print(LogSeverity::Error, "Invalid arguments for world.tp.");
        return;
    }

    world::World& w = *sys->ActiveWorld();
    const u16 targetIdx = static_cast<u16>(*idxRes);
    for (const world::EntityId id : w.View<world::LocalTransform2D>()) {
        if (id.index == targetIdx) {
            world::LocalTransform2D lt = w.GetLocalTransform(id);
            lt.x = *xRes;
            lt.y = *yRes;
            w.Teleport(id, lt);
            console.Print(LogSeverity::Success, "Entity teleported cleanly.");
            return;
        }
    }
    console.Print(LogSeverity::Error, "Entity index not found or not spatial.");
}

void CmdWorldFlag(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr || sys->ActiveGameplay() == nullptr || args.Count() < 2) {
        console.Print(LogSeverity::Warning, "Usage: world.flag <id 0..4095> [0|1]");
        return;
    }
    const auto idRes = args.GetInt(1);
    if (!idRes || *idRes < 0 || *idRes >= 4096) {
        console.Print(LogSeverity::Error, "Flag ID must be in [0..4095].");
        return;
    }
    const u16 flagId = static_cast<u16>(*idRes);
    if (args.Count() >= 3) {
        if (auto bRes = args.GetBool(2)) {
            sys->ActiveGameplay()->SetWorldFlag(flagId, *bRes);
        }
    }
    const bool cur = sys->ActiveGameplay()->TestWorldFlag(flagId);
    console.Print(LogSeverity::Success, cur ? "Flag = true" : "Flag = false");
}

void CmdAudioBus(void* ctx, const ConsoleArgs& args, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr || sys->ActiveAudio() == nullptr || args.Count() < 3) {
        console.Print(LogSeverity::Warning, "Usage: audio.bus <busIdx 0..7> <gain 0.0..2.0>");
        return;
    }
    const auto busRes  = args.GetInt(1);
    const auto gainRes = args.GetFloat(2);
    if (!busRes || !gainRes || *busRes < 0 || *busRes >= 8) {
        console.Print(LogSeverity::Error, "Invalid bus index or gain.");
        return;
    }
    sys->ActiveAudio()->SetBusGain(static_cast<audio::AudioBusId>(*busRes), *gainRes);
    console.Print(LogSeverity::Success, "Updated audio bus gain.");
}

void CmdAuditHash(void* ctx, const ConsoleArgs& /*args*/, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(ctx);
    if (sys == nullptr || sys->ActiveWorld() == nullptr ||
        sys->ActiveGameplay() == nullptr || sys->ActiveAudio() == nullptr) {
        return;
    }
    const u64 wHash = sys->ActiveWorld()->ComputeAuditHash();
    const u64 gHash = sys->ActiveGameplay()->ComputeAuditHash(*sys->ActiveWorld());
    const u64 aHash = sys->ActiveAudio()->ComputeAuditHash();

    std::array<char, 80> buf{};
    char* p = buf.data();
    char* end = buf.data() + buf.size();
    for (char c : std::string_view("W:")) *p++ = c;
    p = DebugBatcher::FormatU64Hex(p, end, wHash, 16);
    for (char c : std::string_view(" G:")) *p++ = c;
    p = DebugBatcher::FormatU64Hex(p, end, gHash, 16);
    for (char c : std::string_view(" A:")) *p++ = c;
    p = DebugBatcher::FormatU64Hex(p, end, aHash, 16);
    console.Print(LogSeverity::Success, std::string_view(buf.data(), static_cast<usize>(p - buf.data())));
}

} // namespace

DebugSystem::DebugSystem(DebugSystem&& other) noexcept
    : m_arenaSlab(other.m_arenaSlab),
      m_debugAtlas(other.m_debugAtlas),
      m_overlays(other.m_overlays),
      m_toggleConsoleScancode(other.m_toggleConsoleScancode),
      m_savedActionMask(other.m_savedActionMask),
      m_pendingStepTicks(other.m_pendingStepTicks),
      m_pendingTimeScaleChannel(other.m_pendingTimeScaleChannel),
      m_pendingPauseToggle(other.m_pendingPauseToggle),
      m_boundWorld(other.m_boundWorld),
      m_boundGameplay(other.m_boundGameplay),
      m_boundAudio(other.m_boundAudio),
      m_shapePool(other.m_shapePool),
      m_inspector(other.m_inspector),
      m_profiler(other.m_profiler),
      m_console(other.m_console) {
    other.m_arenaSlab  = nullptr;
    other.m_debugAtlas = {};
}

DebugSystem& DebugSystem::operator=(DebugSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arenaSlab               = other.m_arenaSlab;
        m_debugAtlas              = other.m_debugAtlas;
        m_overlays                = other.m_overlays;
        m_toggleConsoleScancode   = other.m_toggleConsoleScancode;
        m_savedActionMask         = other.m_savedActionMask;
        m_pendingStepTicks        = other.m_pendingStepTicks;
        m_pendingTimeScaleChannel = other.m_pendingTimeScaleChannel;
        m_pendingPauseToggle      = other.m_pendingPauseToggle;
        m_boundWorld              = other.m_boundWorld;
        m_boundGameplay           = other.m_boundGameplay;
        m_boundAudio              = other.m_boundAudio;
        m_shapePool               = other.m_shapePool;
        m_inspector               = other.m_inspector;
        m_profiler                = other.m_profiler;
        m_console                 = other.m_console;
        other.m_arenaSlab         = nullptr;
        other.m_debugAtlas        = {};
    }
    return *this;
}

Result<void, DebugErrc> DebugSystem::Init(renderer::Renderer& renderer,
                                         const DebugSystemConfig& cfg) noexcept {
    Shutdown(&renderer);

    void* rawMem = ::operator new(kArenaSizeBytes, std::align_val_t{64}, std::nothrow);
    if (rawMem == nullptr) {
        return Err(DebugErrc::OutOfMemory);
    }
    m_arenaSlab = static_cast<u8*>(rawMem);

    // Deterministic 192 KB Slab Partitioning:
    // [0 .. 64KB)     : DebugShapeCmd[2048] (65,536 B)
    // [64KB .. 96KB)  : FrameProfileSample[256] (32,768 B)
    // [96KB .. 100KB) : ActorTickRecord[64] (4,096 B)
    // [100KB .. 108KB): ConsoleSymbol[256] (8,192 B)
    // [108KB .. 112KB): LogEntryMeta[512] (4,096 B)
    // [112KB .. 128KB): char LogText[16384] (16,384 B)
    // [128KB .. 132KB): char History[32][128] (4,096 B)
    // [132KB .. 192KB): Scratch buffer used for 32 KB RGBA8 atlas unpacking at Init()
    auto* shapePtr   = reinterpret_cast<DebugShapeCmd*>(m_arenaSlab + 0);
    auto* profPtr    = reinterpret_cast<FrameProfileSample*>(m_arenaSlab + 65536);
    auto* tickPtr    = reinterpret_cast<ActorTickRecord*>(m_arenaSlab + 98304);
    auto* symPtr     = reinterpret_cast<ConsoleSymbol*>(m_arenaSlab + 102400);
    auto* logMetaPtr = reinterpret_cast<LogEntryMeta*>(m_arenaSlab + 110592);
    auto* logTextPtr = reinterpret_cast<char*>(m_arenaSlab + 114688);
    auto* histPtr    = reinterpret_cast<std::array<char, 128>*>(m_arenaSlab + 131072);
    auto* atlasBytes = m_arenaSlab + 135168;

    m_shapePool.BindArena(std::span<DebugShapeCmd, DebugShapePool::kMaxShapes>(shapePtr, DebugShapePool::kMaxShapes));
    m_profiler.BindArena(std::span<FrameProfileSample, DebugProfiler::kHistoryFrames>(profPtr, DebugProfiler::kHistoryFrames));
    m_inspector.BindArena(std::span<ActorTickRecord, DebugInspector::kHistoryTicks>(tickPtr, DebugInspector::kHistoryTicks));
    m_console.BindArena(
        std::span<ConsoleSymbol, DebugConsole::kMaxSymbols>(symPtr, DebugConsole::kMaxSymbols),
        std::span<LogEntryMeta, DebugConsole::kMaxLogEntries>(logMetaPtr, DebugConsole::kMaxLogEntries),
        std::span<char, DebugConsole::kLogTextBytes>(logTextPtr, DebugConsole::kLogTextBytes),
        std::span<std::array<char, 128>, DebugConsole::kMaxHistory>(histPtr, DebugConsole::kMaxHistory));

    // Unpack 8x8 ASCII glyphs + solid white UV block into the 32 KB scratch slice and upload once.
    std::span<u8, kDebugAtlasWidth * kDebugAtlasHeight * 4> pixelSpan(
        atlasBytes, kDebugAtlasWidth * kDebugAtlasHeight * 4);
    UnpackDebugAtlasRGBA8(pixelSpan);

    renderer::TextureDesc texDesc{};
    texDesc.width  = kDebugAtlasWidth;
    texDesc.height = kDebugAtlasHeight;
    texDesc.format = renderer::TextureFormat::RGBA8;
    texDesc.filter = renderer::TextureFilter::Nearest;
    texDesc.pixels = pixelSpan;
    texDesc.pitch  = kDebugAtlasWidth * 4u;

    auto texRes = renderer.CreateTexture(texDesc);
    if (!texRes) {
        Shutdown();
        return Err(DebugErrc::AtlasCreateFailed);
    }
    m_debugAtlas            = *texRes;
    m_overlays              = cfg.initialOverlays;
    m_toggleConsoleScancode = cfg.toggleConsoleScancode;

    RegisterBuiltInCommands();
    return Ok;
}

void DebugSystem::Shutdown(renderer::Renderer* renderer) noexcept {
    if (renderer != nullptr && m_debugAtlas.index != 0) {
        renderer->DestroyTexture(m_debugAtlas);
    }
    m_debugAtlas = {};
    if (m_arenaSlab != nullptr) {
        ::operator delete(m_arenaSlab, std::align_val_t{64});
        m_arenaSlab = nullptr;
    }
}

void DebugSystem::RegisterBuiltInCommands() noexcept {
    (void)m_console.RegisterCommand("help", CmdHelp, this);
    (void)m_console.RegisterCommand("clear", CmdClear, this);
    (void)m_console.RegisterCommand("overlay", CmdOverlay, this);
    (void)m_console.RegisterCommand("time.pause", CmdTimePause, this);
    (void)m_console.RegisterCommand("time.step", CmdTimeStep, this);
    (void)m_console.RegisterCommand("world.tp", CmdWorldTp, this);
    (void)m_console.RegisterCommand("world.flag", CmdWorldFlag, this);
    (void)m_console.RegisterCommand("audio.bus", CmdAudioBus, this);
    (void)m_console.RegisterCommand("audit.hash", CmdAuditHash, this);
}

bool DebugSystem::OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl,
                                     platform::input::InputPipeline& input) noexcept {
    if (!down) {
        return m_console.IsOpen();
    }

    if (scancode == m_toggleConsoleScancode) {
        const bool nextOpen = !m_console.IsOpen();
        m_console.SetOpen(nextOpen);
        if (nextOpen) {
            m_savedActionMask = input.GetActionMask();
            input.SetActionMask(0);
        } else {
            input.SetActionMask(m_savedActionMask);
        }
        return true;
    }

    if (m_console.IsOpen()) {
        m_console.OnKeyPress(scancode, shift, ctrl);
        return true;
    }

    // Function key hotkeys: F1=58, F2=59, F3=60, F4=61, F5=62, F6=63; '['=47, ']'=48.
    switch (scancode) {
        case 58: // F1: Profiler + Time Channel HUD
            m_overlays = m_overlays ^ (OverlayFlags::ProfilerHud | OverlayFlags::TimeChannelHud);
            return true;
        case 59: // F2: Kinematic Bounds + Velocity Vectors + Safe Breadcrumbs
            m_overlays = m_overlays ^ (OverlayFlags::KinematicBounds | OverlayFlags::VelocityVectors | OverlayFlags::SafeBreadcrumbs);
            return true;
        case 60: // F3: Combat Hitboxes & Hurtboxes
            m_overlays = m_overlays ^ OverlayFlags::CombatHitboxes;
            return true;
        case 61: // F4: Entity Inspector + Input Viewer + Flight Scrubber
            m_overlays = m_overlays ^ (OverlayFlags::EntityInspector | OverlayFlags::InputViewerHud | OverlayFlags::FlightScrubber);
            return true;
        case 62: // F5: Toggle Simulation Pause
            m_pendingPauseToggle = true;
            m_inspector.ResetScrubOffset();
            return true;
        case 63: // F6: Single-Step 1 Tick
            m_pendingStepTicks = 1;
            m_inspector.ResetScrubOffset();
            return true;
        case 47: // '[': Scrub Backward 1 Tick in Flight Recorder
            if (HasOverlay(m_overlays, OverlayFlags::FlightScrubber)) {
                m_inspector.StepScrubOffset(+1);
                return true;
            }
            break;
        case 48: // ']': Scrub Forward 1 Tick in Flight Recorder
            if (HasOverlay(m_overlays, OverlayFlags::FlightScrubber)) {
                m_inspector.StepScrubOffset(-1);
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

bool DebugSystem::OnPlatformTextInput(std::string_view textUtf8) noexcept {
    if (!m_console.IsOpen()) {
        return false;
    }
    m_console.OnTextInput(textUtf8);
    return true;
}

void DebugSystem::EndSimTick(const world::World& world,
                             time::SimStepView step,
                             const platform::input::TickInput& tickInput,
                             const platform::input::ActionBuffer& actionBuffer) noexcept {
    m_shapePool.SetCurrentTick(step.TickIndex());
    m_inspector.RecordActorTick(world, step.TickIndex(), tickInput, actionBuffer);
}

void DebugSystem::EmitOverlays(const world::World& world,
                               const gameplay::GameplayPipeline& gameplay,
                               const audio::AudioSystem& audio,
                               const assets::AssetBank& assetBank,
                               time::RenderTimeView timeView,
                               const platform::input::FrameInput& frameInput,
                               platform::Extent2i viewportSize,
                               renderer::RenderList& outList) noexcept {
    // Configure screen-space camera on ViewSlot 15 for crisp 1:1 pixel HUD & Console rendering.
    renderer::Camera2D hudCam{};
    hudCam.worldCenterX = static_cast<f32>(viewportSize.width) * 0.5f;
    hudCam.worldCenterY = static_cast<f32>(viewportSize.height) * 0.5f;
    hudCam.zoom         = 1.0f;
    hudCam.viewport     = {0.0f, 0.0f, static_cast<f32>(viewportSize.width), static_cast<f32>(viewportSize.height)};
    hudCam.scissor      = hudCam.viewport;
    hudCam.screenSpace  = true;
    outList.SetView(kDebugHudViewSlot, hudCam);

    m_shapePool.EmitAndExpire(timeView.tickIndex, m_debugAtlas, outList);

    if (m_overlays != OverlayFlags::None) {
        m_inspector.EmitWorldAndInspectorOverlays(
            m_overlays, world, gameplay, audio, assetBank,
            timeView, frameInput, viewportSize, m_debugAtlas, outList);
    }

    m_profiler.EndFrame();
    DebugBatcher hudBatcher(outList, m_debugAtlas, kDebugHudPassId, kDebugHudViewSlot);
    m_profiler.EmitProfilerAndTimeHud(m_overlays, viewportSize, hudBatcher);
    m_console.EmitConsoleOverlay(viewportSize, hudBatcher);
}

#endif // EN_ENABLE_DEBUG_TOOLS

} // namespace en::debug

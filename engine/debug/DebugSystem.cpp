#include "debug/DebugSystem.h"

#include "input/InputPipeline.h"
#include "renderer/RenderList.h"
#include "renderer/Renderer.h"

#include <algorithm>
#include <new>

namespace en::debug {

#if EN_ENABLE_DEBUG_TOOLS

namespace {

inline constexpr usize kOffsetShapes   = 0;
inline constexpr usize kOffsetSamples  = kOffsetShapes  + sizeof(DebugShapeCmd) * DebugShapePool::kMaxShapes;
inline constexpr usize kOffsetTicks    = kOffsetSamples + sizeof(FrameProfileSample) * DebugProfiler::kHistoryFrames;
inline constexpr usize kOffsetSymbols  = kOffsetTicks   + sizeof(ActorTickRecord) * DebugInspector::kHistoryTicks;
inline constexpr usize kOffsetLogMeta  = kOffsetSymbols + sizeof(ConsoleSymbol) * DebugConsole::kMaxSymbols;
inline constexpr usize kOffsetLogText  = kOffsetLogMeta + sizeof(LogEntryMeta) * DebugConsole::kMaxLogEntries;
inline constexpr usize kOffsetHistory  = kOffsetLogText + DebugConsole::kLogTextBytes;
inline constexpr usize kTotalUsedArena = kOffsetHistory + sizeof(DebugConsole::LineBuffer) * DebugConsole::kMaxHistory;

static_assert(kTotalUsedArena == DebugSystem::kArenaSizeBytes);
static_assert((kOffsetSamples & 63u) == 0);
static_assert((kOffsetTicks   & 63u) == 0);
static_assert((kOffsetSymbols & 63u) == 0);
static_assert((kOffsetHistory & 63u) == 0);

struct OverlayNameEntry {
    std::string_view name;
    OverlayFlags flag;
};

inline constexpr std::array<OverlayNameEntry, 17> kOverlayNames{{
    {"none",        OverlayFlags::None},
    {"all",         OverlayFlags::AllWorld | OverlayFlags::AllHud},
    {"world",       OverlayFlags::AllWorld},
    {"hud",         OverlayFlags::AllHud},
    {"transforms",  OverlayFlags::SpatialTransforms},
    {"bounds",      OverlayFlags::KinematicBounds},
    {"velocity",    OverlayFlags::VelocityVectors},
    {"hitboxes",    OverlayFlags::CombatHitboxes},
    {"tiles",       OverlayFlags::TileCollisionMap},
    {"grid",        OverlayFlags::DynamicSpatialGrid},
    {"breadcrumbs", OverlayFlags::SafeBreadcrumbs},
    {"camera",      OverlayFlags::CameraRigOverlay},
    {"audio",       OverlayFlags::AudioEmitters},
    {"inspector",   OverlayFlags::EntityInspector},
    {"scrubber",    OverlayFlags::FlightScrubber},
    {"profiler",    OverlayFlags::ProfilerHud},
    {"time",        OverlayFlags::TimeChannelHud}
}};

} // namespace

DebugSystem::DebugSystem(DebugSystem&& other) noexcept {
    *this = std::move(other);
}

DebugSystem& DebugSystem::operator=(DebugSystem&& other) noexcept {
    if (this != &other) {
        Shutdown();
        m_arenaSlab             = std::exchange(other.m_arenaSlab, nullptr);
        m_debugAtlas            = std::exchange(other.m_debugAtlas, renderer::TextureHandle{});
        m_overlays              = other.m_overlays;
        m_pendingDirectives     = other.m_pendingDirectives;
        m_toggleConsoleScancode = other.m_toggleConsoleScancode;
        m_savedActionMask       = other.m_savedActionMask;
        m_inspector             = other.m_inspector;
        m_shapePool             = other.m_shapePool;
        m_profiler              = other.m_profiler;
        m_console               = other.m_console;
    }
    return *this;
}

Result<void, DebugErrc> DebugSystem::Init(renderer::Renderer& renderer, const DebugSystemConfig& cfg) noexcept {
    Shutdown(&renderer);

    void* rawMem = ::operator new(kArenaSizeBytes, std::align_val_t{64}, std::nothrow);
    if (rawMem == nullptr) {
        return Err(DebugErrc::OutOfMemory);
    }
    m_arenaSlab = static_cast<u8*>(rawMem);

    std::array<u8, kDebugAtlasWidth * kDebugAtlasHeight * 4> atlasPixels{};
    UnpackDebugAtlasRGBA8(atlasPixels);

    renderer::TextureDesc texDesc{};
    texDesc.width  = kDebugAtlasWidth;
    texDesc.height = kDebugAtlasHeight;
    texDesc.format = renderer::TextureFormat::RGBA8;
    texDesc.filter = renderer::TextureFilter::Nearest;
    texDesc.pixels = atlasPixels;
    texDesc.pitch  = kDebugAtlasWidth * 4;

    auto texRes = renderer.CreateTexture(texDesc);
    if (!texRes) {
        ::operator delete(m_arenaSlab, std::align_val_t{64});
        m_arenaSlab = nullptr;
        return Err(DebugErrc::AtlasCreateFailed);
    }
    m_debugAtlas            = *texRes;
    m_overlays              = cfg.initialOverlays;
    m_pendingDirectives     = {};
    m_toggleConsoleScancode = cfg.toggleConsoleScancode;
    m_savedActionMask       = 0xFFFFu;

    m_shapePool.BindArena(std::span<DebugShapeCmd, DebugShapePool::kMaxShapes>(
        reinterpret_cast<DebugShapeCmd*>(m_arenaSlab + kOffsetShapes), DebugShapePool::kMaxShapes));
    m_profiler.BindArena(std::span<FrameProfileSample, DebugProfiler::kHistoryFrames>(
        reinterpret_cast<FrameProfileSample*>(m_arenaSlab + kOffsetSamples), DebugProfiler::kHistoryFrames));
    m_inspector.BindArena(std::span<ActorTickRecord, DebugInspector::kHistoryTicks>(
        reinterpret_cast<ActorTickRecord*>(m_arenaSlab + kOffsetTicks), DebugInspector::kHistoryTicks));
    m_console.BindArena(
        std::span<ConsoleSymbol, DebugConsole::kMaxSymbols>(
            reinterpret_cast<ConsoleSymbol*>(m_arenaSlab + kOffsetSymbols), DebugConsole::kMaxSymbols),
        std::span<LogEntryMeta, DebugConsole::kMaxLogEntries>(
            reinterpret_cast<LogEntryMeta*>(m_arenaSlab + kOffsetLogMeta), DebugConsole::kMaxLogEntries),
        std::span<char, DebugConsole::kLogTextBytes>(
            reinterpret_cast<char*>(m_arenaSlab + kOffsetLogText), DebugConsole::kLogTextBytes),
        std::span<DebugConsole::LineBuffer, DebugConsole::kMaxHistory>(
            reinterpret_cast<DebugConsole::LineBuffer*>(m_arenaSlab + kOffsetHistory), DebugConsole::kMaxHistory));

    RegisterBuiltInCommands();
    return {};
}

void DebugSystem::Shutdown(renderer::Renderer* renderer) noexcept {
    if (renderer != nullptr && m_debugAtlas.generation != 0) {
        renderer->DestroyTexture(m_debugAtlas);
    }
    m_debugAtlas = {};
    if (m_arenaSlab != nullptr) {
        ::operator delete(m_arenaSlab, std::align_val_t{64});
        m_arenaSlab = nullptr;
    }
}

bool DebugSystem::OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl) noexcept {
    if (down && scancode == m_toggleConsoleScancode) {
        m_console.SetOpen(!m_console.IsOpen());
        return true;
    }
    if (!m_console.IsOpen()) {
        return false;
    }
    if (down) {
        m_console.OnKeyPress(scancode, shift, ctrl);
    }
    return true;
}

bool DebugSystem::OnPlatformKeyEvent(u16 scancode, bool down, bool shift, bool ctrl,
                                     input::InputPipeline& input) noexcept {
    const bool wasOpen = m_console.IsOpen();
    const bool handled = OnPlatformKeyEvent(scancode, down, shift, ctrl);
    const bool nowOpen = m_console.IsOpen();

    if (!wasOpen && nowOpen) {
        m_savedActionMask = input.GetActionMask();
        input.SetActionMask(0);
    } else if (wasOpen && !nowOpen) {
        input.SetActionMask(m_savedActionMask);
    }
    return handled;
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
                             const input::TickInput& tickInput,
                             const input::ActionBuffer& actionBuffer) noexcept {
    m_shapePool.SetCurrentTick(step.TickIndex());
    m_inspector.RecordActorTick(world, step.TickIndex(), tickInput, actionBuffer);
}

void DebugSystem::EmitOverlays(const DebugEmitOverlayContext& ctx,
                               renderer::RenderList& outList) noexcept {
    renderer::Camera2D hudCam{};
    hudCam.viewport    = {0.0f, 0.0f, static_cast<f32>(ctx.viewportSize.w), static_cast<f32>(ctx.viewportSize.h)};
    hudCam.scissor     = hudCam.viewport;
    hudCam.zoom        = 1.0f;
    hudCam.screenSpace = true;
    outList.SetView(kDebugHudViewSlot, hudCam);

    DebugBatcher worldBatcher(outList, m_debugAtlas, kDebugWorldPassId, 0);
    DebugBatcher hudBatcher(outList, m_debugAtlas, kDebugHudPassId, kDebugHudViewSlot);

    m_shapePool.EmitAndExpire(ctx.timeView.tickIndex, worldBatcher, hudBatcher);
    m_inspector.EmitWorldAndInspectorOverlays(m_overlays, ctx, worldBatcher, hudBatcher);
    m_profiler.EmitProfilerAndTimeHud(m_overlays, ctx.viewportSize, hudBatcher);
    m_console.EmitConsoleOverlay(ctx.viewportSize, hudBatcher);
}

void DebugSystem::RegisterBuiltInCommands() noexcept {
    (void)m_console.RegisterCommand("help", [](void*, const ConsoleArgs&, DebugConsole& con) noexcept {
        for (const ConsoleSymbol& sym : con.Symbols()) {
            if (sym.kind != SymbolKind::Empty) {
                con.Print(LogSeverity::Info, sym.Name());
            }
        }
    }, this);

    (void)m_console.RegisterCommand("clear", [](void*, const ConsoleArgs&, DebugConsole& con) noexcept {
        con.ClearLog();
    }, this);

    (void)m_console.RegisterCommand("pause", [](void* ctx, const ConsoleArgs&, DebugConsole& con) noexcept {
        static_cast<DebugSystem*>(ctx)->RequestPauseToggle();
        con.Print(LogSeverity::Success, "Toggled simulation pause");
    }, this);

    (void)m_console.RegisterCommand("step", [](void* ctx, const ConsoleArgs& args, DebugConsole& con) noexcept {
        i64 ticks = 1;
        if (args.Count() >= 2) {
            const auto res = args.GetInt(1);
            if (!res || *res < 1 || *res > 120) {
                con.Print(LogSeverity::Error, "Usage: step [1..120]");
                return;
            }
            ticks = *res;
        }
        static_cast<DebugSystem*>(ctx)->RequestSteps(static_cast<u16>(ticks));
        con.Print(LogSeverity::Success, "Queued simulation step");
    }, this);

    (void)m_console.RegisterCommand("timescale", [](void* ctx, const ConsoleArgs& args, DebugConsole& con) noexcept {
        const auto chRes = args.GetInt(1);
        const auto scRes = args.GetFloat(2);
        if (args.Count() < 3 || !chRes || !scRes || *chRes < 0 || *chRes > 7) {
            con.Print(LogSeverity::Error, "Usage: timescale <channel 0..7> <scale 0..4>");
            return;
        }
        static_cast<DebugSystem*>(ctx)->RequestChannelTimeScale(
            static_cast<u8>(*chRes), std::clamp(*scRes, 0.0f, 4.0f));
        con.Print(LogSeverity::Success, "Updated channel time scale");
    }, this);

    (void)m_console.RegisterCommand("overlay", [](void* ctx, const ConsoleArgs& args, DebugConsole& con) noexcept {
        if (args.Count() < 2) {
            con.Print(LogSeverity::Error, "Usage: overlay <name|all|none|world|hud>");
            return;
        }
        auto* self = static_cast<DebugSystem*>(ctx);
        for (const OverlayNameEntry& entry : kOverlayNames) {
            if (entry.name == args[1]) {
                self->m_overlays = (entry.flag == OverlayFlags::None)
                    ? OverlayFlags::None
                    : (self->m_overlays ^ entry.flag);
                con.Print(LogSeverity::Success, "Updated overlay flags");
                return;
            }
        }
        con.Print(LogSeverity::Error, "Unknown overlay flag");
    }, this);

    (void)m_console.RegisterCommand("scrub", [](void* ctx, const ConsoleArgs& args, DebugConsole& con) noexcept {
        if (args.Count() < 2) {
            con.Print(LogSeverity::Error, "Usage: scrub <deltaTicks|reset>");
            return;
        }
        auto* self = static_cast<DebugSystem*>(ctx);
        if (args[1] == "reset") {
            self->m_inspector.ResetScrubOffset();
            con.Print(LogSeverity::Success, "Reset flight scrubber");
            return;
        }
        const auto dRes = args.GetInt(1);
        if (!dRes) {
            con.Print(LogSeverity::Error, "Invalid scrub delta");
            return;
        }
        self->m_inspector.StepScrubOffset(static_cast<i32>(*dRes));
        con.Print(LogSeverity::Success, "Updated flight scrubber offset");
    }, this);
}

u64 DebugSystem::ComputeAuditHash() const noexcept {
    u64 h = MixAuditHash(0xDEB065957E401ull, static_cast<u32>(m_overlays));
    h = MixAuditHash(h, m_shapePool.ComputeAuditHash());
    h = MixAuditHash(h, m_inspector.ComputeAuditHash());
    h = MixAuditHash(h, m_console.ComputeAuditHash());
    return h;
}

#else

DebugSystem::DebugSystem(DebugSystem&&) noexcept = default;
DebugSystem& DebugSystem::operator=(DebugSystem&&) noexcept = default;

#endif

} // namespace en::debug

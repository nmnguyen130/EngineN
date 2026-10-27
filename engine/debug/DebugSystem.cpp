#include "debug/DebugSystem.h"
#include "platform/PlatformTypes.h"
#include "input/InputPipeline.h"

#include <charconv>

namespace en {

namespace {

[[nodiscard]] DebugShapePool& NullShapePool() noexcept {
    static DebugShapePool s_null{};
    return s_null;
}

[[nodiscard]] DebugProfiler& NullProfiler() noexcept {
    static DebugProfiler s_null{};
    return s_null;
}

[[nodiscard]] DebugConsole& NullConsole() noexcept {
    static DebugConsole s_null{};
    return s_null;
}

} // namespace

struct alignas(64) DebugSystem::DebugArenaState {
    DebugShapePool shapes{};
    DebugProfiler profiler{};
    DebugConsole console{};
    DebugSystemConfig cfg{};
    OverlayFlags activeOverlays{OverlayFlags::ProfilerHud};
    u16 savedActionMask{0xFFFFU};
    u8 actionMaskSaved{0U};
    std::array<DebugShapeCmd, kMaxDebugShapes> shapeStorage{};
    std::array<ActorTickRecord, kActorFlightHistoryTicks> actorHistoryStorage{};
    std::array<FrameProfileSample, kProfileHistoryFrames> profileStorage{};
    std::array<WatchPlotTrack, kMaxWatchPlots> plotStorage{};
    std::array<ConsoleSymbol, kMaxConsoleSymbols> symbolStorage{};
    std::array<LogEntryMeta, kMaxConsoleLogs> logMetaStorage{};
    std::array<char, kConsoleLogTextBytes> logTextStorage{};
};

#if EN_ENABLE_DEBUG_TOOLS

namespace {

inline constexpr u16 kScancodeReturn    = 40U;
inline constexpr u16 kScancodeEscape    = 41U;
inline constexpr u16 kScancodeBackspace = 42U;
inline constexpr u16 kScancodeTab       = 43U;
inline constexpr u16 kScancodeRight     = 79U;
inline constexpr u16 kScancodeLeft      = 80U;
inline constexpr u16 kScancodeDown      = 81U;
inline constexpr u16 kScancodeUp        = 82U;

struct OverlayShortcut {
    std::string_view name;
    OverlayFlags flag;
};

inline constexpr std::array<OverlayShortcut, 9> kOverlayTable{{
    {"profiler",   OverlayFlags::ProfilerHud},
    {"colliders",  OverlayFlags::PhysicsColliders},
    {"broadphase", OverlayFlags::SpatialBroadphase},
    {"hitboxes",   OverlayFlags::CombatHitboxes},
    {"velocity",   OverlayFlags::VelocityVectors},
    {"contacts",   OverlayFlags::ContactManifolds},
    {"inspector",  OverlayFlags::EntityInspector | OverlayFlags::FlightScrubber},
    {"input",      OverlayFlags::InputViewerHud},
    {"plots",      OverlayFlags::WatchPlotHud},
}};

void CmdHelp(std::string_view, void*, DebugConsole& console) noexcept {
    console.Print(
        LogSeverity::Info,
        "Built-in: help, clear, cvarlist, reset, pause, step [N], timescale <s [ch]>, overlay <name>, inspect <id>");
}

void CmdClear(std::string_view, void*, DebugConsole& console) noexcept {
    console.ClearLogs();
}

void CmdCVarList(std::string_view, void*, DebugConsole& console) noexcept {
    console.ListAllCVars();
}

void CmdReset(std::string_view, void* userCtx, DebugConsole& console) noexcept {
    if (auto* sys = static_cast<DebugSystem*>(userCtx)) {
        sys->RequestStateReset();
        console.Print(LogSeverity::Info, "Requested simulation state reset.");
    }
}

void CmdPause(std::string_view, void* userCtx, DebugConsole& console) noexcept {
    if (auto* sys = static_cast<DebugSystem*>(userCtx)) {
        sys->RequestPauseToggle();
        console.Print(LogSeverity::Info, "Toggled simulation pause.");
    }
}

void CmdStep(std::string_view args, void* userCtx, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(userCtx);
    if (!sys) {
        return;
    }
    u16 steps = 1U;
    if (!args.empty()) {
        u32 parsed = 1U;
        const auto r = std::from_chars(args.data(), args.data() + args.size(), parsed);
        if (r.ec == std::errc{}) {
            steps = static_cast<u16>(std::clamp<u32>(parsed, 1U, 1200U));
        }
    }
    sys->RequestStepTicks(steps);
    console.Print(LogSeverity::Info, "Scheduled simulation step.");
}

void CmdTimeScale(std::string_view args, void* userCtx, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(userCtx);
    if (!sys || args.empty()) {
        return;
    }

    const usize spacePos = args.find(' ');
    const std::string_view scaleStr = (spacePos == std::string_view::npos) ? args : args.substr(0U, spacePos);
    f32 scale = 1.0f;
    const auto rScale = std::from_chars(scaleStr.data(), scaleStr.data() + scaleStr.size(), scale);
    if (rScale.ec != std::errc{}) {
        return;
    }

    u8 chIdx = 0U;
    if (spacePos != std::string_view::npos) {
        const std::string_view chStr = args.substr(spacePos + 1U);
        u32 parsedCh = 0U;
        const auto rCh = std::from_chars(chStr.data(), chStr.data() + chStr.size(), parsedCh);
        if (rCh.ec == std::errc{}) {
            chIdx = static_cast<u8>(parsedCh & 7U);
        }
    }

    sys->RequestChannelTimeScale(scale, static_cast<TimeChannel>(chIdx));
    console.Print(LogSeverity::Info, "Updated channel time scale.");
}

void CmdOverlay(std::string_view args, void* userCtx, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(userCtx);
    if (!sys || args.empty()) {
        return;
    }
    if (args == "all") {
        sys->SetActiveOverlays(OverlayFlags::AllWorld | OverlayFlags::AllHud);
    } else if (args == "none") {
        sys->SetActiveOverlays(OverlayFlags::None);
    } else {
        for (const OverlayShortcut& entry : kOverlayTable) {
            if (args == entry.name) {
                sys->ToggleOverlay(entry.flag);
                break;
            }
        }
    }
    console.Print(LogSeverity::Info, "Updated debug overlays.");
}

void CmdInspect(std::string_view args, void* userCtx, DebugConsole& console) noexcept {
    auto* sys = static_cast<DebugSystem*>(userCtx);
    if (!sys || args.empty()) {
        return;
    }
    u32 idx = 0U;
    const auto r = std::from_chars(args.data(), args.data() + args.size(), idx);
    if (r.ec == std::errc{} && idx > 0U && idx <= 0xFFFFU) {
        sys->Inspector().SetPinnedEntity(EntityId{static_cast<u16>(idx), 1U});
        sys->ToggleOverlay(OverlayFlags::EntityInspector);
        console.Print(LogSeverity::Info, "Pinned entity slot.");
    }
}

} // namespace

Result<void, DebugErrc> DebugSystem::Init(const DebugSystemConfig& cfg) noexcept {
    Shutdown();
    const usize required = sizeof(DebugArenaState) + 256U;
    const usize totalBytes = std::max<usize>(cfg.arenaBytes, required);
    if (!m_arena.Init(totalBytes)) {
        return Err(DebugErrc::OutOfMemory);
    }

    auto stateSlice = m_arena.Carve<DebugArenaState>(1U);
    if (stateSlice.empty()) {
        Shutdown();
        return Err(DebugErrc::ArenaExhausted);
    }

    m_state = stateSlice.data();
    *m_state = DebugArenaState{};
    m_state->shapes.Init(m_state->shapeStorage);
    m_state->profiler.Init(m_state->profileStorage, m_state->plotStorage);
    m_state->console.Init(m_state->symbolStorage, m_state->logMetaStorage, m_state->logTextStorage);
    m_inspector.Init(m_state->actorHistoryStorage);

    m_state->cfg = cfg;
    m_state->activeOverlays = cfg.initialOverlays;
    m_pendingDirectives = DebugLoopDirectives{};

    (void)m_state->console.RegisterCommand("help", &CmdHelp, this);
    (void)m_state->console.RegisterCommand("clear", &CmdClear, this);
    (void)m_state->console.RegisterCommand("cvarlist", &CmdCVarList, this);
    (void)m_state->console.RegisterCommand("reset", &CmdReset, this);
    (void)m_state->console.RegisterCommand("pause", &CmdPause, this);
    (void)m_state->console.RegisterCommand("step", &CmdStep, this);
    (void)m_state->console.RegisterCommand("timescale", &CmdTimeScale, this);
    (void)m_state->console.RegisterCommand("overlay", &CmdOverlay, this);
    (void)m_state->console.RegisterCommand("inspect", &CmdInspect, this);

    return {};
}

void DebugSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_state = nullptr;
    m_inspector = DebugInspector{};
    m_pendingDirectives = DebugLoopDirectives{};
}

OverlayFlags DebugSystem::ActiveOverlays() const noexcept {
    return m_state ? m_state->activeOverlays : OverlayFlags::None;
}

void DebugSystem::SetActiveOverlays(OverlayFlags flags) noexcept {
    if (m_state) {
        m_state->activeOverlays = flags;
    }
}

void DebugSystem::ToggleOverlay(OverlayFlags flag) noexcept {
    if (m_state) {
        m_state->activeOverlays ^= flag;
    }
}

bool DebugSystem::OnPlatformKeyEvent(u16 scancode, bool down) noexcept {
    if (!m_state || !down) {
        return false;
    }

    if (scancode == m_state->cfg.toggleConsoleScancode) {
        m_state->console.ToggleOpen();
        if (m_state->console.IsOpen()) {
            m_state->activeOverlays |= OverlayFlags::ConsoleOverlay;
        } else {
            m_state->activeOverlays &= ~OverlayFlags::ConsoleOverlay;
        }
        return true;
    }

    if (m_state->console.IsOpen()) {
        if (scancode == kScancodeReturn) {
            m_state->console.SubmitInputLine();
        } else if (scancode == kScancodeBackspace) {
            m_state->console.BackspaceInput();
        } else if (scancode == kScancodeTab) {
            (void)m_state->console.AutocompleteInput();
        } else if (scancode == kScancodeUp) {
            m_state->console.NavigateHistory(1);
        } else if (scancode == kScancodeDown) {
            m_state->console.NavigateHistory(-1);
        } else if (scancode == kScancodeEscape) {
            m_state->console.SetOpen(false);
            m_state->activeOverlays &= ~OverlayFlags::ConsoleOverlay;
        }
        return true;
    }

    if (scancode == m_state->cfg.togglePauseScancode) {
        RequestPauseToggle();
        return true;
    }
    if (scancode == m_state->cfg.stepTickScancode) {
        RequestStepTicks(1U);
        return true;
    }
    if (scancode == m_state->cfg.toggleProfilerScancode) {
        ToggleOverlay(OverlayFlags::ProfilerHud);
        return true;
    }

    if (HasFlag(m_state->activeOverlays, OverlayFlags::EntityInspector | OverlayFlags::FlightScrubber) &&
        m_inspector.RecordedTickCount() > 0U) {
        if (scancode == kScancodeLeft) {
            m_inspector.StepScrubOffset(1);
            return true;
        }
        if (scancode == kScancodeRight) {
            m_inspector.StepScrubOffset(-1);
            return true;
        }
    }
    return false;
}

bool DebugSystem::OnPlatformKeyEvent(
    u16 scancode,
    bool down,
    InputPipeline& inputPipeline) noexcept {
    if (!m_state) {
        return false;
    }
    const bool wasOpen = m_state->console.IsOpen();
    const bool consumed = OnPlatformKeyEvent(scancode, down);
    const bool isOpen = m_state->console.IsOpen();

    if (!wasOpen && isOpen) {
        m_state->savedActionMask = inputPipeline.GetActionMask();
        m_state->actionMaskSaved = 1U;
        inputPipeline.SetActionMask(0U);
    } else if (wasOpen && !isOpen && m_state->actionMaskSaved != 0U) {
        inputPipeline.SetActionMask(m_state->savedActionMask);
        m_state->actionMaskSaved = 0U;
    }
    return consumed;
}

bool DebugSystem::OnPlatformTextInput(std::string_view textUtf8) noexcept {
    if (!m_state || !m_state->console.IsOpen()) {
        return false;
    }
    if (textUtf8 == "`" || textUtf8 == "~") {
        return true;
    }
    m_state->console.AppendInputText(textUtf8);
    return true;
}

DebugLoopDirectives DebugSystem::BeginFrame(const DebugFrameContext&) noexcept {
    if (!m_state) {
        return {};
    }
    m_state->console.FlushPendingCommands();
    m_state->profiler.BeginFrame();

    const DebugLoopDirectives out = m_pendingDirectives;
    m_pendingDirectives = DebugLoopDirectives{};
    return out;
}

void DebugSystem::EndSimTick(
    const World& world,
    u32 tickIndex,
    const TickInput* tickInput,
    const ActionBuffer* actionBuffer) noexcept {
    if (m_state) {
        m_state->shapes.SetCurrentTick(tickIndex);
        m_inspector.RecordActorTick(world, tickIndex, tickInput, actionBuffer);
    }
}

void DebugSystem::EmitOverlays(const DebugFrameContext& ctx, RenderList& outList) noexcept {
    if (!m_state) {
        return;
    }

    const OverlayFlags flags = m_state->activeOverlays;
    if (flags == OverlayFlags::None &&
        m_state->shapes.ActiveCount() == 0U &&
        !m_state->console.IsOpen()) {
        return;
    }

    RenderPassDesc passDesc{};
    passDesc.enabled = true;
    passDesc.clear = false;
    outList.SetPass(kDebugWorldPassId, passDesc);
    outList.SetPass(kDebugHudPassId, passDesc);

    DebugBatcher worldBatcher(outList, kDebugWorldPassId, ctx.worldViewSlot, kDebugFixedDepth);
    DebugBatcher hudBatcher(outList, kDebugHudPassId, kDebugHudViewSlot, kDebugFixedDepth);

    if (m_state->shapes.ActiveCount() > 0U) {
        m_state->shapes.EmitAndExpire(ctx.currentTickIndex, worldBatcher, hudBatcher);
    }
    if (flags != OverlayFlags::None) {
        m_inspector.EmitWorldAndInspectorOverlays(flags, ctx, worldBatcher, hudBatcher);
        m_state->profiler.EmitProfilerAndTimeHud(flags, ctx.viewportSize, hudBatcher);
    }
    if (m_state->console.IsOpen()) {
        m_state->console.EmitConsoleOverlay(ctx.viewportSize, hudBatcher);
    }
}

DebugShapePool& DebugSystem::Shapes() noexcept { return m_state ? m_state->shapes : NullShapePool(); }
const DebugShapePool& DebugSystem::Shapes() const noexcept { return m_state ? m_state->shapes : NullShapePool(); }

DebugProfiler& DebugSystem::Profiler() noexcept { return m_state ? m_state->profiler : NullProfiler(); }
const DebugProfiler& DebugSystem::Profiler() const noexcept { return m_state ? m_state->profiler : NullProfiler(); }

DebugConsole& DebugSystem::Console() noexcept { return m_state ? m_state->console : NullConsole(); }
const DebugConsole& DebugSystem::Console() const noexcept { return m_state ? m_state->console : NullConsole(); }

#else

DebugShapePool& DebugSystem::Shapes() noexcept { return NullShapePool(); }
const DebugShapePool& DebugSystem::Shapes() const noexcept { return NullShapePool(); }

DebugProfiler& DebugSystem::Profiler() noexcept { return NullProfiler(); }
const DebugProfiler& DebugSystem::Profiler() const noexcept { return NullProfiler(); }

DebugConsole& DebugSystem::Console() noexcept { return NullConsole(); }
const DebugConsole& DebugSystem::Console() const noexcept { return NullConsole(); }

#endif // EN_ENABLE_DEBUG_TOOLS

} // namespace en

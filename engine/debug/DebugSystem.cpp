#include "debug/DebugSystem.h"

#include "input/InputSystem.h"

#include <charconv>

namespace en {
namespace {

ShapePool& NullShapes() noexcept {
    static ShapePool s{};
    return s;
}
DebugProfiler& NullProfiler() noexcept {
    static DebugProfiler s{};
    return s;
}
DebugConsole& NullConsole() noexcept {
    static DebugConsole s{};
    return s;
}

} // namespace

struct DebugSystem::State {
    ShapePool                                   shapes{};
    DebugProfiler                               profiler{};
    DebugConsole                                console{};
    DebugConfig                                 cfg{};
    Overlay                                     active{Overlay::Profiler};
    u16                                         savedMask{0xFFFFU};
    u8                                          maskSaved{0};
    std::array<ShapeCmd, kMaxGizmos>            shapeStorage{};
    std::array<ActorSample, kFlightTicks>       flightStorage{};
    std::array<FrameSample, kProfileFrames>     profileStorage{};
    std::array<ConsoleSymbol, kMaxSymbols>      symbolStorage{};
    std::array<ConsoleLogMeta, kMaxConsoleLogs> logStorage{};
    std::array<char, kConsoleTextBytes>         textStorage{};
};

#if EN_ENABLE_DEBUG_TOOLS

namespace {

constexpr u16 kReturn    = 40U;
constexpr u16 kEscape    = 41U;
constexpr u16 kBackspace = 42U;
constexpr u16 kTab       = 43U;
constexpr u16 kRight     = 79U;
constexpr u16 kLeft      = 80U;
constexpr u16 kDown      = 81U;
constexpr u16 kUp        = 82U;

struct OverlayEntry {
    std::string_view name;
    Overlay          flag;
};

constexpr std::array<OverlayEntry, 7> kOverlays{{
    {"profiler",   Overlay::Profiler},
    {"colliders",  Overlay::Colliders},
    {"broadphase", Overlay::Broadphase},
    {"hitboxes",   Overlay::Hitboxes},
    {"velocity",   Overlay::Velocity},
    {"audio",      Overlay::Audio},
    {"inspector",  Overlay::Inspector},
}};

void CmdHelp(std::string_view, void*, DebugConsole& c) noexcept {
    c.Print(LogLevel::Info, "help clear cvarlist reset pause step timescale overlay inspect");
}
void CmdClear(std::string_view, void*, DebugConsole& c) noexcept { c.ClearLogs(); }
void CmdCVars(std::string_view, void*, DebugConsole& c) noexcept { c.ListCVars(); }

void CmdReset(std::string_view, void* ctx, DebugConsole& c) noexcept {
    if (auto* s = static_cast<DebugSystem*>(ctx)) {
        s->RequestReset();
        c.Print(LogLevel::Info, "reset requested");
    }
}
void CmdPause(std::string_view, void* ctx, DebugConsole& c) noexcept {
    if (auto* s = static_cast<DebugSystem*>(ctx)) {
        s->RequestPause();
        c.Print(LogLevel::Info, "pause toggled");
    }
}
void CmdStep(std::string_view args, void* ctx, DebugConsole& c) noexcept {
    auto* s = static_cast<DebugSystem*>(ctx);
    if (!s) return;
    u16 n = 1U;
    if (!args.empty()) {
        u32 v = 1U;
        if (std::from_chars(args.data(), args.data() + args.size(), v).ec == std::errc{}) {
            n = static_cast<u16>(std::clamp<u32>(v, 1U, 1200U));
        }
    }
    s->RequestStep(n);
    c.Print(LogLevel::Info, "step scheduled");
}
void CmdTimeScale(std::string_view args, void* ctx, DebugConsole& c) noexcept {
    auto* s = static_cast<DebugSystem*>(ctx);
    if (!s || args.empty()) return;
    const usize sp       = args.find(' ');
    const auto  scaleStr = (sp == std::string_view::npos) ? args : args.substr(0, sp);
    f32         scale    = 1.0f;
    if (std::from_chars(scaleStr.data(), scaleStr.data() + scaleStr.size(), scale).ec != std::errc{}) return;
    u8 ch = 0U;
    if (sp != std::string_view::npos) {
        u32        v    = 0U;
        const auto rest = args.substr(sp + 1);
        if (std::from_chars(rest.data(), rest.data() + rest.size(), v).ec == std::errc{}) {
            ch = static_cast<u8>(v & 7U);
        }
    }
    s->RequestTimeScale(scale, static_cast<TimeChannel>(ch));
    c.Print(LogLevel::Info, "timescale set");
}
void CmdOverlay(std::string_view args, void* ctx, DebugConsole& c) noexcept {
    auto* s = static_cast<DebugSystem*>(ctx);
    if (!s) return;
    if (args.empty()) {
        c.Print(LogLevel::Info, "overlay <profiler|colliders|broadphase|hitboxes|velocity|audio|inspector>");
        return;
    }
    for (const auto& e : kOverlays) {
        if (args == e.name) {
            s->Toggle(e.flag);
            c.Print(LogLevel::Info, "overlay toggled");
            return;
        }
    }
    c.Print(LogLevel::Warn, "unknown overlay");
}
void CmdInspect(std::string_view args, void* ctx, DebugConsole& c) noexcept {
    auto* s = static_cast<DebugSystem*>(ctx);
    if (!s || args.empty()) return;
    u32 id = 0U;
    if (std::from_chars(args.data(), args.data() + args.size(), id).ec != std::errc{}) return;
    s->Inspector().Pin(EntityId{static_cast<u16>(id & 0xFFFFU), 1U});
    s->SetOverlays(s->Overlays() | Overlay::Inspector);
    c.Print(LogLevel::Info, "entity pinned");
}

} // namespace

Result<void, DebugErrc> DebugSystem::Init(const DebugConfig& cfg) noexcept {
    Shutdown();
    const usize need = sizeof(State) + 256U;
    if (!m_arena.Init(std::max<usize>(cfg.arenaBytes, need))) {
        return Err(DebugErrc::OutOfMemory);
    }
    auto slice = m_arena.Alloc<State>(1U);
    if (slice.empty()) {
        Shutdown();
        return Err(DebugErrc::OutOfMemory);
    }
    m_state = slice.data();
    m_state->shapes.Init(m_state->shapeStorage);
    m_state->profiler.Init(m_state->profileStorage);
    m_state->console.Init(m_state->symbolStorage, m_state->logStorage, m_state->textStorage);
    m_inspector.Init(m_state->flightStorage);
    m_state->cfg    = cfg;
    m_state->active = cfg.overlays;
    m_pending       = {};

    (void)m_state->console.RegisterCmd("help", &CmdHelp, this);
    (void)m_state->console.RegisterCmd("clear", &CmdClear, this);
    (void)m_state->console.RegisterCmd("cvarlist", &CmdCVars, this);
    (void)m_state->console.RegisterCmd("reset", &CmdReset, this);
    (void)m_state->console.RegisterCmd("pause", &CmdPause, this);
    (void)m_state->console.RegisterCmd("step", &CmdStep, this);
    (void)m_state->console.RegisterCmd("timescale", &CmdTimeScale, this);
    (void)m_state->console.RegisterCmd("overlay", &CmdOverlay, this);
    (void)m_state->console.RegisterCmd("inspect", &CmdInspect, this);
    return {};
}

void DebugSystem::Shutdown() noexcept {
    m_arena.Shutdown();
    m_state     = nullptr;
    m_inspector = DebugInspector{};
    m_pending   = {};
}

Overlay DebugSystem::Overlays() const noexcept {
    return m_state ? m_state->active : Overlay::None;
}
void DebugSystem::SetOverlays(Overlay flags) noexcept {
    if (m_state) m_state->active = flags;
}
void DebugSystem::Toggle(Overlay flag) noexcept {
    if (m_state) m_state->active ^= flag;
}

bool DebugSystem::OnKey(u16 scancode, bool down) noexcept {
    if (!m_state || !down) return false;

    if (scancode == m_state->cfg.keyConsole) {
        m_state->console.Toggle();
        if (m_state->console.IsOpen()) m_state->active |= Overlay::Console;
        else m_state->active &= ~Overlay::Console;
        return true;
    }

    if (m_state->console.IsOpen()) {
        if (scancode == kReturn) m_state->console.Submit();
        else if (scancode == kBackspace) m_state->console.Backspace();
        else if (scancode == kTab) (void)m_state->console.Autocomplete();
        else if (scancode == kUp) m_state->console.History(1);
        else if (scancode == kDown) m_state->console.History(-1);
        else if (scancode == kEscape) {
            m_state->console.SetOpen(false);
            m_state->active &= ~Overlay::Console;
        }
        return true;
    }

    if (scancode == m_state->cfg.keyPause) {
        RequestPause();
        return true;
    }
    if (scancode == m_state->cfg.keyStep) {
        RequestStep(1U);
        return true;
    }
    if (scancode == m_state->cfg.keyProfiler) {
        Toggle(Overlay::Profiler);
        return true;
    }

    if (HasFlag(m_state->active, Overlay::Inspector) && m_inspector.Recorded() > 0U) {
        if (scancode == kLeft) {
            m_inspector.Scrub(1);
            return true;
        }
        if (scancode == kRight) {
            m_inspector.Scrub(-1);
            return true;
        }
    }
    return false;
}

bool DebugSystem::OnKey(u16 scancode, bool down, InputSystem& input) noexcept {
    if (!m_state) return false;
    const bool wasOpen  = m_state->console.IsOpen();
    const bool consumed = OnKey(scancode, down);
    const bool isOpen   = m_state->console.IsOpen();
    if (!wasOpen && isOpen) {
        m_state->savedMask = 0xFFFFU;
        m_state->maskSaved = 1U;
        input.SetActionMask(0U);
    } else if (wasOpen && !isOpen && m_state->maskSaved != 0U) {
        input.SetActionMask(m_state->savedMask);
        m_state->maskSaved = 0U;
    }
    return consumed;
}

bool DebugSystem::OnText(std::string_view utf8) noexcept {
    if (!m_state || !m_state->console.IsOpen()) return false;
    m_state->console.Append(utf8);
    return true;
}

LoopDirectives DebugSystem::BeginFrame(const FrameCtx&) noexcept {
    if (!m_state) return {};
    m_state->console.Flush();
    m_state->profiler.BeginFrame();
    const LoopDirectives out = m_pending;
    m_pending                = {};
    return out;
}

void DebugSystem::EndTick(
    const World& world,
    u32 tick,
    const TickInput* input,
    const ActionBuffer* buffer) noexcept {
    if (!m_state) return;
    m_state->shapes.SetTick(tick);
    m_inspector.Record(world, tick, input, buffer);
}

void DebugSystem::Emit(const FrameCtx& ctx, RenderList& list) noexcept {
    if (!m_state) return;
    DebugDraw world{list, kDebugWorldPass, ctx.worldView, kDebugDepth};
    DebugDraw hud{list, kDebugHudPass, kDebugHudView, kDebugDepth};

    m_state->shapes.Emit(ctx.tick, world, hud);
    m_inspector.Emit(m_state->active, ctx, world, hud);
    m_state->profiler.EmitHud(m_state->active, ctx.viewport, hud);
    if (HasFlag(m_state->active, Overlay::Console) || m_state->console.IsOpen()) {
        m_state->console.Emit(ctx.viewport, hud);
    }
    m_state->profiler.EndFrame();
}

ShapePool& DebugSystem::Shapes() noexcept {
    return m_state ? m_state->shapes : NullShapes();
}
const ShapePool& DebugSystem::Shapes() const noexcept {
    return m_state ? m_state->shapes : NullShapes();
}
DebugProfiler& DebugSystem::Profiler() noexcept {
    return m_state ? m_state->profiler : NullProfiler();
}
const DebugProfiler& DebugSystem::Profiler() const noexcept {
    return m_state ? m_state->profiler : NullProfiler();
}
DebugConsole& DebugSystem::Console() noexcept {
    return m_state ? m_state->console : NullConsole();
}
const DebugConsole& DebugSystem::Console() const noexcept {
    return m_state ? m_state->console : NullConsole();
}

#else

ShapePool& DebugSystem::Shapes() noexcept { return NullShapes(); }
const ShapePool& DebugSystem::Shapes() const noexcept { return NullShapes(); }
DebugProfiler& DebugSystem::Profiler() noexcept { return NullProfiler(); }
const DebugProfiler& DebugSystem::Profiler() const noexcept { return NullProfiler(); }
DebugConsole& DebugSystem::Console() noexcept { return NullConsole(); }
const DebugConsole& DebugSystem::Console() const noexcept { return NullConsole(); }

#endif

} // namespace en

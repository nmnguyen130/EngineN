#include "debug/DebugProfilerAndConsole.h"
#include <algorithm>
#include <charconv>
#include "core/Serialization.h"

namespace en {

bool DebugProfiler::Init(AlignedArena& arena) noexcept {
    m_samples = arena.Carve<FrameProfileSample>(kHistoryFrames);
    m_buckets.fill(0);
    m_currentIdx = 0;
    m_validFrames = 0;
    return !m_samples.empty();
}

void DebugProfiler::BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept {
    if (m_samples.empty()) return;
    m_currentIdx = static_cast<u32>(frameIndex % kHistoryFrames);
    FrameProfileSample& s = m_samples[m_currentIdx];
    s = FrameProfileSample{};
    s.frameIndex = static_cast<u32>(frameIndex);
    s.simTickIndex = static_cast<u32>(simTickIndex);
}

void DebugProfiler::RecordScopeUs(ProfileScopeId scope, u16 durationUs) noexcept {
    if (m_samples.empty()) return;
    const u8 idx = static_cast<u8>(scope);
    if (idx < 12U) m_samples[m_currentIdx].scopeUs[idx] = durationUs;
}

void DebugProfiler::EndFrame(
    u16 aliveEntities, const RenderStats& renderStats, const AudioTelemetrySnapshot& audioStats) noexcept {
    if (m_samples.empty()) return;
    FrameProfileSample& s = m_samples[m_currentIdx];
    s.aliveEntities = aliveEntities;
    s.submittedQuads = static_cast<u16>(std::min<u32>(renderStats.submittedQuads, 0xFFFFU));
    s.culledQuads = static_cast<u16>(std::min<u32>(renderStats.culledQuads, 0xFFFFU));
    s.drawBatches = static_cast<u16>(std::min<u32>(renderStats.drawBatches, 0xFFFFU));
    s.radixPassesExecuted = renderStats.radixPassesRun;
    s.activePhysVoices = static_cast<u8>(std::min<u16>(audioStats.activeVoices, 255));
    const u32 totalUs = s.scopeUs[static_cast<u8>(ProfileScopeId::FrameTotal)];
    const usize bIdx = std::min<usize>(totalUs / 500U, kBucketCount - 1U);
    ++m_buckets[bIdx];
    if (m_validFrames < kHistoryFrames) ++m_validFrames;
}

u32 DebugProfiler::ComputePercentileUs(u32 rankOutOf256) const noexcept {
    u32 acc = 0;
    for (usize i = 0; i < kBucketCount; ++i) {
        acc += m_buckets[i];
        if (acc >= rankOutOf256) return static_cast<u32>((i + 1U) * 500U);
    }
    return 8333U;
}

usize DebugProfiler::ExportChromeTraceJson(std::span<char> outJsonBuf) const noexcept {
    if (outJsonBuf.size() < 64) return 0;
    const FrameProfileSample& s = Latest();
    JsonLiteWriter jw(outJsonBuf);
    jw.AppendRaw("{\"traceEvents\":[{\"name\":\"FrameTotal\",\"ph\":\"X\",\"ts\":0,\"dur\":");
    jw.AppendInt(s.scopeUs[0]);
    jw.AppendRaw(",\"pid\":1,\"tid\":1}]}");
    return jw.Size();
}

void DebugProfiler::EmitProfilerHud(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept {
    const f32 x = static_cast<f32>(viewportSize.w) - 240.0f;
    hudBatcher.PushSolidRect({x, 12.0f, 228.0f, 64.0f}, Color::FromRGBA8(12, 16, 24, 215));
    hudBatcher.PushText({x + 8.0f, 20.0f}, "PROFILER TELEMETRY", Color::FromRGBA8(110, 245, 175, 255));
    const FrameProfileSample& s = Latest();
    char buf[64]{};
    JsonLiteWriter lw(buf);
    lw.AppendRaw("ENT:");
    lw.AppendInt(s.aliveEntities);
    lw.AppendRaw(" QUADS:");
    lw.AppendInt(s.submittedQuads);
    lw.AppendRaw(" BATCH:");
    lw.AppendInt(s.drawBatches);
    hudBatcher.PushText({x + 8.0f, 42.0f}, lw.View(), Color::FromRGBA8(230, 240, 255, 255));
}

bool DebugConsole::Init(AlignedArena& arena, u32 maxSymbols) noexcept {
    m_symbols = arena.Carve<ConsoleSymbol>(maxSymbols);
    m_symbolCount = 0;
    m_logCount = 0;
    m_open = false;
    return !m_symbols.empty();
}

bool DebugConsole::RegisterCommand(std::string_view name, CommandFn fn, void* userCtx) noexcept {
    if (name.empty() || !fn || m_symbolCount >= m_symbols.size()) return false;
    ConsoleSymbol& s = m_symbols[m_symbolCount++];
    s.id = StringId::Hash(name);
    s.name.Assign(name);
    s.targetPtr = userCtx;
    s.cmdFn = fn;
    s.kind = SymbolKind::Command;
    return true;
}

bool DebugConsole::RegisterCVarF32(std::string_view name, f32* target, f32 minVal, f32 maxVal) noexcept {
    if (name.empty() || !target || m_symbolCount >= m_symbols.size()) return false;
    ConsoleSymbol& s = m_symbols[m_symbolCount++];
    s.id = StringId::Hash(name);
    s.name.Assign(name);
    s.targetPtr = target;
    s.minVal = minVal;
    s.maxVal = maxVal;
    s.kind = SymbolKind::Float;
    return true;
}

bool DebugConsole::RegisterCVarI32(std::string_view name, i32* target, i32 minVal, i32 maxVal) noexcept {
    if (name.empty() || !target || m_symbolCount >= m_symbols.size()) return false;
    ConsoleSymbol& s = m_symbols[m_symbolCount++];
    s.id = StringId::Hash(name);
    s.name.Assign(name);
    s.targetPtr = target;
    s.minVal = static_cast<f32>(minVal);
    s.maxVal = static_cast<f32>(maxVal);
    s.kind = SymbolKind::Int;
    return true;
}

bool DebugConsole::RegisterCVarBool(std::string_view name, bool* target) noexcept {
    if (name.empty() || !target || m_symbolCount >= m_symbols.size()) return false;
    ConsoleSymbol& s = m_symbols[m_symbolCount++];
    s.id = StringId::Hash(name);
    s.name.Assign(name);
    s.targetPtr = target;
    s.kind = SymbolKind::Bool;
    return true;
}

void DebugConsole::Print(LogSeverity /*severity*/, std::string_view message) noexcept {
    m_logs[m_logCount % m_logs.size()].Assign(message);
    ++m_logCount;
}

void DebugConsole::ExecuteImmediate(std::string_view cmdLine) noexcept {
    while (!cmdLine.empty() && cmdLine.front() == ' ') cmdLine.remove_prefix(1);
    if (cmdLine.empty()) return;
    const usize spacePos = cmdLine.find(' ');
    const std::string_view token = (spacePos == std::string_view::npos) ? cmdLine : cmdLine.substr(0, spacePos);
    std::string_view rest = (spacePos == std::string_view::npos) ? std::string_view{} : cmdLine.substr(spacePos + 1U);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);

    const StringId id = StringId::Hash(token);
    for (usize i = 0; i < m_symbolCount; ++i) {
        ConsoleSymbol& s = m_symbols[i];
        if (s.id != id) continue;
        switch (s.kind) {
            case SymbolKind::Command:
                if (s.cmdFn) s.cmdFn(rest, s.targetPtr);
                return;
            case SymbolKind::Float:
                if (!rest.empty() && s.targetPtr) {
                    f32 sign = 1.0f;
                    std::string_view sv = rest;
                    if (!sv.empty() && sv.front() == '-') { sign = -1.0f; sv.remove_prefix(1); }
                    i64 whole = 0;
                    const usize dotPos = sv.find('.');
                    const std::string_view wholePart = (dotPos == std::string_view::npos) ? sv : sv.substr(0, dotPos);
                    std::from_chars(wholePart.data(), wholePart.data() + wholePart.size(), whole);
                    f32 frac = 0.0f;
                    if (dotPos != std::string_view::npos) {
                        const std::string_view fracPart = sv.substr(dotPos + 1U);
                        f32 scale = 0.1f;
                        for (char c : fracPart) {
                            if (c < '0' || c > '9') break;
                            frac += static_cast<f32>(c - '0') * scale;
                            scale *= 0.1f;
                        }
                    }
                    const f32 v = sign * (static_cast<f32>(whole) + frac);
                    *static_cast<f32*>(s.targetPtr) = std::clamp(v, s.minVal, s.maxVal);
                }
                return;
            case SymbolKind::Int:
                if (!rest.empty() && s.targetPtr) {
                    i32 v = 0;
                    std::from_chars(rest.data(), rest.data() + rest.size(), v);
                    *static_cast<i32*>(s.targetPtr) = std::clamp(v, static_cast<i32>(s.minVal), static_cast<i32>(s.maxVal));
                }
                return;
            case SymbolKind::Bool:
                if (s.targetPtr) {
                    if (rest == "1" || rest == "true") *static_cast<bool*>(s.targetPtr) = true;
                    else if (rest == "0" || rest == "false") *static_cast<bool*>(s.targetPtr) = false;
                    else *static_cast<bool*>(s.targetPtr) = !(*static_cast<bool*>(s.targetPtr));
                }
                return;
            default:
                return;
        }
    }
}

void DebugConsole::EmitConsoleOverlay(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept {
    if (!m_open) return;
    const f32 w = static_cast<f32>(viewportSize.w);
    hudBatcher.PushSolidRect({0.0f, 0.0f, w, 180.0f}, Color::FromRGBA8(10, 12, 20, 230));
    hudBatcher.PushText({12.0f, 10.0f}, "DEVELOPER CONSOLE", Color::FromRGBA8(255, 215, 90, 255));
    const usize count = std::min<usize>(m_logCount, 8U);
    for (usize i = 0; i < count; ++i) {
        const auto& line = m_logs[(m_logCount - count + i) % m_logs.size()];
        hudBatcher.PushText({12.0f, 34.0f + static_cast<f32>(i) * 16.0f}, line.View(), Color::FromRGBA8(215, 225, 240, 255));
    }
}

bool DebugSystem::Init(usize arenaBytes) noexcept {
    Shutdown();
    if (!m_arena.Init(arenaBytes)) return false;
    return m_shapes.Init(m_arena) && m_inspector.Init(m_arena) &&
           m_profiler.Init(m_arena) && m_console.Init(m_arena);
}

void DebugSystem::Shutdown() noexcept {
    m_arena.Shutdown();
}

DebugLoopDirectives DebugSystem::BeginFrame(const DebugFrameContext& ctx) noexcept {
    m_profiler.BeginFrame(ctx.frameInput.frameIndex, ctx.timeView.tickIndex);
    const DebugLoopDirectives out = m_pendingDirectives;
    m_pendingDirectives = DebugLoopDirectives{};
    return out;
}

void DebugSystem::EndSimTick(
    const World& world, const SimStepView& step,
    const TickInput& tickInput, const ActionBuffer& actionBuffer) noexcept {
    m_inspector.RecordActorTick(world, step.tickIndex, tickInput, actionBuffer);
}

void DebugSystem::EmitOverlays(const DebugFrameContext& ctx, RenderList& outList) noexcept {
    DebugBatcher worldBatcher(outList, 0, 0);
    DebugBatcher hudBatcher(outList, 0, 15);
    m_shapes.EmitAndExpire(ctx.timeView.tickIndex, worldBatcher);
    if (m_overlays != OverlayFlags::None) {
        m_inspector.EmitWorldAndInspectorOverlays(m_overlays, ctx, worldBatcher, hudBatcher);
    }
    if (HasFlag(m_overlays, OverlayFlags::ProfilerHud)) {
        m_profiler.EmitProfilerHud(ctx.viewportSize, hudBatcher);
    }
    m_console.EmitConsoleOverlay(ctx.viewportSize, hudBatcher);
}

} // namespace en

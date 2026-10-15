#include "debug/DebugProfilerAndConsole.h"

#include <algorithm>
#include <cstring>

namespace en::debug {

void DebugProfiler::BindArena(std::span<FrameProfileSample, kHistoryFrames> samples) noexcept {
    m_samples = samples;
    Clear();
}

void DebugProfiler::Clear() noexcept {
    for (auto& s : m_samples) {
        s = FrameProfileSample{};
    }
    m_buckets.fill(0);
    m_writeIdx    = 0;
    m_latestIdx   = 0;
    m_validFrames = 0;
}

void DebugProfiler::BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept {
    if (m_samples.empty()) return;

    FrameProfileSample& slot = m_samples[m_writeIdx];
    if (m_validFrames == kHistoryFrames) {
        const u32 oldBucket = std::min<u32>(kBucketCount - 1u, slot.scopeUs[0] >> 8u);
        if (m_buckets[oldBucket] > 0) {
            --m_buckets[oldBucket];
        }
    }

    slot = FrameProfileSample{};
    slot.frameIndex   = frameIndex;
    slot.simTickIndex = simTickIndex;
}

void DebugProfiler::RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept {
    if (m_samples.empty()) return;
    const usize idx = static_cast<usize>(scope);
    if (idx < kProfileScopeCount) {
        m_samples[m_writeIdx].scopeUs[idx] = static_cast<u16>(std::min<u32>(65535u, durationUs));
    }
}

void DebugProfiler::RecordSubsystemTelemetry(u8 catchupTicks, u16 aliveEntities,
                                             u16 submittedQuads, u16 culledQuads, u16 drawBatches,
                                             u8 radixPasses,
                                             const audio::AudioTelemetrySnapshot& audioSnap,
                                             std::span<const f32, 8> channelScales,
                                             u8 frozenChannelMask) noexcept {
    if (m_samples.empty()) return;
    FrameProfileSample& s = m_samples[m_writeIdx];
    s.catchupTicks        = catchupTicks;
    s.aliveEntities       = aliveEntities;
    s.submittedQuads      = submittedQuads;
    s.culledQuads         = culledQuads;
    s.drawBatches         = drawBatches;
    s.radixPassesExecuted = radixPasses;
    s.activePhysVoices    = static_cast<u8>(std::popcount(audioSnap.activeVoiceMask));
    s.activeVirtVoices    = static_cast<u8>(std::min<u32>(255u, audioSnap.activeVirtualVoices));
    s.audioCpuPermille    = static_cast<u16>(std::clamp(audioSnap.callbackCpuLoadRatio * 1000.0f, 0.0f, 1000.0f));
    s.audioPeakL_Q10      = static_cast<u16>(std::clamp(audioSnap.masterPeakL * 1024.0f, 0.0f, 65535.0f));
    s.audioPeakR_Q10      = static_cast<u16>(std::clamp(audioSnap.masterPeakR * 1024.0f, 0.0f, 65535.0f));
    s.audioUnderruns      = static_cast<u8>(std::min<u32>(255u, audioSnap.underrunCount));
    s.frozenChannelMask   = frozenChannelMask;

    for (usize ch = 0; ch < 8; ++ch) {
        s.channelScaleQ8[ch] = static_cast<u16>(std::clamp(channelScales[ch] * 256.0f, 0.0f, 65535.0f));
    }
}

void DebugProfiler::EndFrame() noexcept {
    if (m_samples.empty()) return;
    FrameProfileSample& s = m_samples[m_writeIdx];
    if (s.scopeUs[0] == 0) {
        u32 total = 0;
        for (usize i = 1; i < kProfileScopeCount; ++i) {
            total += s.scopeUs[i];
        }
        s.scopeUs[0] = static_cast<u16>(std::min<u32>(65535u, total));
    }

    const u32 newBucket = std::min<u32>(kBucketCount - 1u, s.scopeUs[0] >> 8u);
    ++m_buckets[newBucket];

    m_latestIdx = m_writeIdx;
    m_writeIdx  = (m_writeIdx + 1u) & (kHistoryFrames - 1u);
    if (m_validFrames < kHistoryFrames) {
        ++m_validFrames;
    }
}

u32 DebugProfiler::ComputePercentileUs(u32 targetRankOutOf256) const noexcept {
    if (m_validFrames == 0) return 0;
    const u32 scaledTarget = (targetRankOutOf256 * m_validFrames) / kHistoryFrames;
    u32 accum = 0;
    for (u32 b = 0; b < kBucketCount; ++b) {
        accum += m_buckets[b];
        if (accum > scaledTarget) {
            return (b << 8u) + 128u;
        }
    }
    return (kBucketCount << 8u);
}

void DebugProfiler::EmitProfilerAndTimeHud(OverlayFlags flags,
                                           platform::Extent2i viewportSize,
                                           DebugBatcher& hudBatcher) const noexcept {
    if (m_samples.empty() || m_validFrames == 0) return;
    const FrameProfileSample& latest = m_samples[m_latestIdx];
    std::array<char, 80> buf{};
    char* end = buf.data() + buf.size();

    if (HasOverlay(flags, OverlayFlags::ProfilerHud)) {
        const f32 panelW = 296.0f;
        const f32 panelH = 164.0f;
        const f32 panelX = static_cast<f32>(viewportSize.w) - panelW - 12.0f;
        const f32 panelY = 12.0f;

        hudBatcher.PushSolidRect({panelX, panelY, panelW, panelH}, renderer::Color::FromRGBA8(0x0D1117DD));
        hudBatcher.PushBoxOutline({panelX, panelY, panelW, panelH}, 1.0f, renderer::Color::FromRGBA8(0x33AAFFFF));

        const f32 p50Ms = static_cast<f32>(ComputePercentileUs(128)) * 0.001f;
        const f32 p95Ms = static_cast<f32>(ComputePercentileUs(243)) * 0.001f;
        const f32 p99Ms = static_cast<f32>(ComputePercentileUs(253)) * 0.001f;

        {
            char* p = buf.data();
            for (char c : std::string_view("MS P50:")) *p++ = c;
            p = DebugBatcher::FormatF32Fixed2(p, end, p50Ms);
            for (char c : std::string_view(" P95:")) *p++ = c;
            p = DebugBatcher::FormatF32Fixed2(p, end, p95Ms);
            for (char c : std::string_view(" P99:")) *p++ = c;
            p = DebugBatcher::FormatF32Fixed2(p, end, p99Ms);
            hudBatcher.PushText(panelX + 8.0f, panelY + 8.0f,
                                std::string_view(buf.data(), static_cast<usize>(p - buf.data())),
                                renderer::Color::FromRGBA8(0x00FF88FF));
        }

        // 256-frame stacked bar graph (1px per frame).
        const f32 graphX = panelX + 20.0f;
        const f32 graphY = panelY + 24.0f;
        const f32 graphH = 64.0f;
        hudBatcher.PushSolidRect({graphX, graphY, 256.0f, graphH}, renderer::Color::FromRGBA8(0x161B22FF));

        for (u32 i = 0; i < kHistoryFrames; ++i) {
            const u32 idx = (m_writeIdx + i) & (kHistoryFrames - 1u);
            const FrameProfileSample& s = m_samples[idx];
            const f32 x = graphX + static_cast<f32>(i);

            const f32 simH = std::min(graphH, static_cast<f32>(s.scopeUs[static_cast<usize>(ProfileScopeId::SimTickTotal)]) * (graphH / 16667.0f));
            const f32 renH = std::min(graphH - simH, static_cast<f32>(s.scopeUs[static_cast<usize>(ProfileScopeId::RenderSubmit)]) * (graphH / 16667.0f));

            if (simH > 0.5f) {
                hudBatcher.PushSolidRect({x, graphY + graphH - simH, 1.0f, simH}, renderer::Color::FromRGBA8(0x2288FFFF));
            }
            if (renH > 0.5f) {
                hudBatcher.PushSolidRect({x, graphY + graphH - simH - renH, 1.0f, renH}, renderer::Color::FromRGBA8(0x00FF66FF));
            }
        }

        // 8.33ms (120Hz) and 16.67ms (60Hz) reference lines.
        hudBatcher.PushLine(graphX, graphY + graphH * 0.5f, graphX + 256.0f, graphY + graphH * 0.5f,
                            1.0f, renderer::Color::FromRGBA8(0xFFFF0066));

        auto emitStat = [&](f32 yOff, std::string_view label, i64 v0, i64 v1, i64 v2) {
            char* p = buf.data();
            for (char c : label) *p++ = c;
            p = DebugBatcher::FormatI64(p, end, v0);
            if (p < end) *p++ = '/';
            p = DebugBatcher::FormatI64(p, end, v1);
            if (p < end) *p++ = '/';
            p = DebugBatcher::FormatI64(p, end, v2);
            hudBatcher.PushText(panelX + 8.0f, panelY + yOff,
                                std::string_view(buf.data(), static_cast<usize>(p - buf.data())),
                                renderer::Color::FromRGBA8(0xE0E6F0FF));
        };

        emitStat(96.0f,  "ENT/TICK/RADIX: ", latest.aliveEntities, latest.catchupTicks, latest.radixPassesExecuted);
        emitStat(112.0f, "QUAD/CULL/BTCH: ", latest.submittedQuads, latest.culledQuads, latest.drawBatches);
        emitStat(128.0f, "AUD V/VIRT/CPU: ", latest.activePhysVoices, latest.activeVirtVoices, latest.audioCpuPermille);
        emitStat(144.0f, "PEAK L/R / UDR: ", latest.audioPeakL_Q10, latest.audioPeakR_Q10, latest.audioUnderruns);
    }

    if (HasOverlay(flags, OverlayFlags::TimeChannelHud)) {
        const f32 panelX = 12.0f;
        const f32 panelY = 12.0f;
        const f32 panelW = 240.0f;
        const f32 panelH = 92.0f;

        hudBatcher.PushSolidRect({panelX, panelY, panelW, panelH}, renderer::Color::FromRGBA8(0x0D1117DD));
        hudBatcher.PushBoxOutline({panelX, panelY, panelW, panelH}, 1.0f, renderer::Color::FromRGBA8(0x00E5FFFF));
        hudBatcher.PushText(panelX + 8.0f, panelY + 6.0f, "TIME CHANNELS [0..7]",
                            renderer::Color::FromRGBA8(0x00E5FFFF));

        for (u8 ch = 0; ch < 8; ++ch) {
            const f32 rowY = panelY + 20.0f + static_cast<f32>(ch) * 8.5f;
            const bool frozen = (latest.frozenChannelMask & (1u << ch)) != 0;
            const f32 scale = static_cast<f32>(latest.channelScaleQ8[ch]) * (1.0f / 256.0f);
            const f32 barW  = std::clamp(scale * 80.0f, 0.0f, 160.0f);

            hudBatcher.PushSolidRect({panelX + 12.0f, rowY, barW, 6.0f},
                                     frozen ? renderer::Color::FromRGBA8(0x00FFFFFF)
                                            : renderer::Color::FromRGBA8(0x00CC66FF));
        }
    }
}

ConsoleArgs::ConsoleArgs(std::string_view rawLine) noexcept {
    usize i = 0;
    const usize n = rawLine.size();
    while (i < n && m_count < kMaxTokens) {
        while (i < n && (rawLine[i] == ' ' || rawLine[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        if (rawLine[i] == '"') {
            ++i;
            const usize start = i;
            while (i < n && rawLine[i] != '"') {
                ++i;
            }
            m_tokens[m_count++] = rawLine.substr(start, i - start);
            if (i < n) ++i;
        } else {
            const usize start = i;
            while (i < n && rawLine[i] != ' ' && rawLine[i] != '\t') {
                ++i;
            }
            m_tokens[m_count++] = rawLine.substr(start, i - start);
        }
    }
}

Result<i64, DebugErrc> ConsoleArgs::GetInt(usize idx) const noexcept {
    if (idx >= m_count || m_tokens[idx].empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const std::string_view s = m_tokens[idx];
    usize pos = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        pos = 1;
    } else if (s[0] == '+') {
        pos = 1;
    }
    if (pos >= s.size()) return Err(DebugErrc::InvalidArguments);

    i64 val = 0;
    for (; pos < s.size(); ++pos) {
        const char c = s[pos];
        if (c < '0' || c > '9') return Err(DebugErrc::InvalidArguments);
        val = val * 10 + (c - '0');
    }
    return neg ? -val : val;
}

Result<f32, DebugErrc> ConsoleArgs::GetFloat(usize idx) const noexcept {
    if (idx >= m_count || m_tokens[idx].empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const std::string_view s = m_tokens[idx];
    usize pos = 0;
    f32 sign = 1.0f;
    if (s[0] == '-') {
        sign = -1.0f;
        pos = 1;
    } else if (s[0] == '+') {
        pos = 1;
    }
    if (pos >= s.size()) return Err(DebugErrc::InvalidArguments);

    f32 result = 0.0f;
    bool seenDot = false;
    f32 fracScale = 0.1f;

    for (; pos < s.size(); ++pos) {
        const char c = s[pos];
        if (c == '.') {
            if (seenDot) return Err(DebugErrc::InvalidArguments);
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') return Err(DebugErrc::InvalidArguments);
        const f32 digit = static_cast<f32>(c - '0');
        if (!seenDot) {
            result = result * 10.0f + digit;
        } else {
            result += digit * fracScale;
            fracScale *= 0.1f;
        }
    }
    return result * sign;
}

Result<bool, DebugErrc> ConsoleArgs::GetBool(usize idx) const noexcept {
    if (idx >= m_count || m_tokens[idx].empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const std::string_view s = m_tokens[idx];
    if (s == "1" || s == "true" || s == "on") return true;
    if (s == "0" || s == "false" || s == "off") return false;
    return Err(DebugErrc::InvalidArguments);
}

void DebugConsole::BindArena(std::span<ConsoleSymbol, kMaxSymbols> symbolTable,
                             std::span<LogEntryMeta, kMaxLogEntries> logMeta,
                             std::span<char, kLogTextBytes> logText,
                             std::span<std::array<char, 128>, kMaxHistory> historyBuf) noexcept {
    m_symbols = symbolTable;
    m_logMeta = logMeta;
    m_logText = logText;
    m_history = historyBuf;

    for (auto& sym : m_symbols) sym = ConsoleSymbol{};
    ClearLog();
}

void DebugConsole::ClearLog() noexcept {
    for (auto& m : m_logMeta) m = LogEntryMeta{};
    m_textWriteCursor = 0;
    m_logWriteIdx     = 0;
    m_logCount        = 0;
    m_scrollOffset    = 0;
}

Result<ConsoleSymbol*, DebugErrc> DebugConsole::InsertSymbolSlot(std::string_view staticName) noexcept {
    if (m_symbols.empty() || staticName.empty() || staticName.size() > 255u) {
        return Err(DebugErrc::InvalidArguments);
    }
    const u64 h = HashSymbolName(staticName);
    // Fibonacci hashing multiplier for 256-slot table (shift 64 - 8 = 56).
    u32 slot = static_cast<u32>((h * 11400714819323198485ull) >> 56u);

    for (usize probe = 0; probe < kMaxSymbols; ++probe) {
        ConsoleSymbol& entry = m_symbols[slot];
        if (entry.kind == SymbolKind::Empty) {
            entry.nameHash = h;
            entry.namePtr  = staticName.data();
            entry.nameLen  = static_cast<u8>(staticName.size());
            return &entry;
        }
        if (entry.nameHash == h &&
            std::string_view(entry.namePtr, entry.nameLen) == staticName) {
            return Err(DebugErrc::DuplicateSymbol);
        }
        slot = (slot + 1u) & (kMaxSymbols - 1u);
    }
    return Err(DebugErrc::SymbolTableFull);
}

ConsoleSymbol* DebugConsole::FindSymbol(std::string_view name) noexcept {
    if (m_symbols.empty() || name.empty()) return nullptr;
    const u64 h = HashSymbolName(name);
    u32 slot = static_cast<u32>((h * 11400714819323198485ull) >> 56u);

    for (usize probe = 0; probe < kMaxSymbols; ++probe) {
        ConsoleSymbol& entry = m_symbols[slot];
        if (entry.kind == SymbolKind::Empty) {
            return nullptr;
        }
        if (entry.nameHash == h &&
            std::string_view(entry.namePtr, entry.nameLen) == name) {
            return &entry;
        }
        slot = (slot + 1u) & (kMaxSymbols - 1u);
    }
    return nullptr;
}

Result<void, DebugErrc> DebugConsole::RegisterCommand(
    std::string_view staticName,
    ConsoleCmdCallback fn,
    void* userCtx) noexcept {
    if (fn == nullptr) return Err(DebugErrc::InvalidArguments);
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) return Err(slotRes.error());
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::Command;
    sym->targetPtr = userCtx;
    sym->cmdFn     = fn;
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarF32(
    std::string_view staticName,
    f32* target, f32 minVal, f32 maxVal) noexcept {
    if (target == nullptr) return Err(DebugErrc::InvalidArguments);
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) return Err(slotRes.error());
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarF32;
    sym->targetPtr = target;
    sym->f32Bounds = {minVal, maxVal};
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarI32(
    std::string_view staticName,
    i32* target, i32 minVal, i32 maxVal) noexcept {
    if (target == nullptr) return Err(DebugErrc::InvalidArguments);
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) return Err(slotRes.error());
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarI32;
    sym->targetPtr = target;
    sym->i32Bounds = {minVal, maxVal};
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarBool(
    std::string_view staticName,
    bool* target) noexcept {
    if (target == nullptr) return Err(DebugErrc::InvalidArguments);
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) return Err(slotRes.error());
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarBool;
    sym->targetPtr = target;
    return Ok;
}

void DebugConsole::Print(LogSeverity severity, std::string_view msg, u64 tickIndex) noexcept {
    if (m_logMeta.empty() || m_logText.empty() || msg.empty()) return;

    const u8 len = static_cast<u8>(std::min<usize>(msg.size(), 240u));
    if (static_cast<usize>(m_textWriteCursor) + len > kLogTextBytes) {
        m_textWriteCursor = 0;
    }

    for (u8 i = 0; i < len; ++i) {
        m_logText[m_textWriteCursor + i] = msg[i];
    }

    LogEntryMeta& meta = m_logMeta[m_logWriteIdx];
    meta.textOffset = m_textWriteCursor;
    meta.textLength = len;
    meta.severity   = severity;
    meta.tickLow16  = static_cast<u16>(tickIndex);

    m_textWriteCursor = static_cast<u16>(m_textWriteCursor + len);
    m_logWriteIdx     = (m_logWriteIdx + 1u) & (kMaxLogEntries - 1u);
    if (m_logCount < kMaxLogEntries) {
        ++m_logCount;
    }
}

void DebugConsole::EnqueueCommandLine(std::string_view line) noexcept {
    if (line.empty() || m_pendingCount >= kMaxPendingCmds) return;
    const u8 len = static_cast<u8>(std::min<usize>(line.size(), 120u));
    for (u8 i = 0; i < len; ++i) {
        m_pendingCmds[m_pendingCount][i] = line[i];
    }
    m_pendingLens[m_pendingCount] = len;
    ++m_pendingCount;
}

void DebugConsole::FlushPendingCommands(u64 currentTickIndex) noexcept {
    for (u8 i = 0; i < m_pendingCount; ++i) {
        const std::string_view cmdLine(m_pendingCmds[i].data(), m_pendingLens[i]);
        ExecuteImmediate(cmdLine, currentTickIndex);
    }
    m_pendingCount = 0;
}

void DebugConsole::ExecuteImmediate(std::string_view line, u64 currentTickIndex) noexcept {
    const ConsoleArgs args(line);
    if (args.Count() == 0) return;

    Print(LogSeverity::Echo, line, currentTickIndex);

    ConsoleSymbol* sym = FindSymbol(args[0]);
    if (sym == nullptr) {
        Print(LogSeverity::Error, "Unknown command or CVar. Type 'help' for list.", currentTickIndex);
        return;
    }

    std::array<char, 96> reply{};
    char* end = reply.data() + reply.size();

    switch (sym->kind) {
        case SymbolKind::Command:
            if (sym->cmdFn != nullptr) {
                sym->cmdFn(sym->targetPtr, args, *this);
            }
            break;
        case SymbolKind::CVarF32: {
            f32* ptr = static_cast<f32*>(sym->targetPtr);
            if (args.Count() >= 2) {
                if (auto val = args.GetFloat(1)) {
                    *ptr = std::clamp(*val, sym->f32Bounds.minVal, sym->f32Bounds.maxVal);
                } else {
                    Print(LogSeverity::Error, "Invalid float value.", currentTickIndex);
                    return;
                }
            }
            char* p = reply.data();
            for (char c : std::string_view(sym->namePtr, sym->nameLen)) *p++ = c;
            for (char c : std::string_view(" = ")) *p++ = c;
            p = DebugBatcher::FormatF32Fixed2(p, end, *ptr);
            Print(LogSeverity::Success, std::string_view(reply.data(), static_cast<usize>(p - reply.data())), currentTickIndex);
            break;
        }
        case SymbolKind::CVarI32: {
            i32* ptr = static_cast<i32*>(sym->targetPtr);
            if (args.Count() >= 2) {
                if (auto val = args.GetInt(1)) {
                    const i64 clamped = std::clamp<i64>(*val, sym->i32Bounds.minVal, sym->i32Bounds.maxVal);
                    *ptr = static_cast<i32>(clamped);
                } else {
                    Print(LogSeverity::Error, "Invalid integer value.", currentTickIndex);
                    return;
                }
            }
            char* p = reply.data();
            for (char c : std::string_view(sym->namePtr, sym->nameLen)) *p++ = c;
            for (char c : std::string_view(" = ")) *p++ = c;
            p = DebugBatcher::FormatI64(p, end, *ptr);
            Print(LogSeverity::Success, std::string_view(reply.data(), static_cast<usize>(p - reply.data())), currentTickIndex);
            break;
        }
        case SymbolKind::CVarBool: {
            bool* ptr = static_cast<bool*>(sym->targetPtr);
            if (args.Count() >= 2) {
                if (auto val = args.GetBool(1)) {
                    *ptr = *val;
                } else {
                    Print(LogSeverity::Error, "Invalid bool value (use 0/1/true/false).", currentTickIndex);
                    return;
                }
            }
            char* p = reply.data();
            for (char c : std::string_view(sym->namePtr, sym->nameLen)) *p++ = c;
            for (char c : (*ptr ? std::string_view(" = true") : std::string_view(" = false"))) *p++ = c;
            Print(LogSeverity::Success, std::string_view(reply.data(), static_cast<usize>(p - reply.data())), currentTickIndex);
            break;
        }
        default:
            break;
    }
}

void DebugConsole::OnTextInput(std::string_view utf8) noexcept {
    if (!m_isOpen) return;
    for (const char c : utf8) {
        if (c == '`' || c == '~') continue;
        if (c >= 0x20 && c < 0x7F && m_inputLen < m_inputBuf.size() - 1u) {
            for (u8 i = m_inputLen; i > m_cursorPos; --i) {
                m_inputBuf[i] = m_inputBuf[i - 1u];
            }
            m_inputBuf[m_cursorPos++] = c;
            ++m_inputLen;
        }
    }
}

void DebugConsole::AutoCompleteInput() noexcept {
    const std::string_view prefix(m_inputBuf.data(), m_inputLen);
    if (prefix.empty()) return;

    for (const ConsoleSymbol& sym : m_symbols) {
        if (sym.kind == SymbolKind::Empty) continue;
        const std::string_view name(sym.namePtr, sym.nameLen);
        if (name.starts_with(prefix)) {
            const u8 copyLen = static_cast<u8>(std::min<usize>(name.size(), m_inputBuf.size() - 2u));
            for (u8 i = 0; i < copyLen; ++i) {
                m_inputBuf[i] = name[i];
            }
            m_inputBuf[copyLen] = ' ';
            m_inputLen  = copyLen + 1u;
            m_cursorPos = m_inputLen;
            return;
        }
    }
}

void DebugConsole::OnKeyPress(u16 scancode, bool /*shift*/, bool /*ctrl*/) noexcept {
    if (!m_isOpen) return;

    // Standard SDL/USB HID scancodes: Return=40, Escape=41, Backspace=42, Tab=43, Right=79, Left=80, Down=81, Up=82.
    switch (scancode) {
        case 40: { // Return / Enter
            if (m_inputLen > 0) {
                const std::string_view line(m_inputBuf.data(), m_inputLen);
                if (!m_history.empty()) {
                    auto& hSlot = m_history[m_historyWriteIdx];
                    hSlot.fill(0);
                    for (u8 i = 0; i < m_inputLen; ++i) hSlot[i] = m_inputBuf[i];
                    m_historyWriteIdx = (m_historyWriteIdx + 1u) & (kMaxHistory - 1u);
                    if (m_historyCount < kMaxHistory) ++m_historyCount;
                }
                EnqueueCommandLine(line);
                m_inputLen         = 0;
                m_cursorPos        = 0;
                m_historyBrowseIdx = -1;
            }
            break;
        }
        case 42: { // Backspace
            if (m_cursorPos > 0) {
                for (u8 i = m_cursorPos - 1u; i + 1u < m_inputLen; ++i) {
                    m_inputBuf[i] = m_inputBuf[i + 1u];
                }
                --m_cursorPos;
                --m_inputLen;
            }
            break;
        }
        case 43: // Tab
            AutoCompleteInput();
            break;
        case 80: // Left Arrow
            if (m_cursorPos > 0) --m_cursorPos;
            break;
        case 79: // Right Arrow
            if (m_cursorPos < m_inputLen) ++m_cursorPos;
            break;
        case 82: { // Up Arrow (History recall)
            if (m_historyCount > 0 && !m_history.empty()) {
                m_historyBrowseIdx = static_cast<i8>(std::min<i32>(m_historyCount - 1, m_historyBrowseIdx + 1));
                const u32 idx = (m_historyWriteIdx + kMaxHistory - 1u - static_cast<u32>(m_historyBrowseIdx)) & (kMaxHistory - 1u);
                const auto& hSlot = m_history[idx];
                u8 len = 0;
                while (len < 120u && hSlot[len] != 0) {
                    m_inputBuf[len] = hSlot[len];
                    ++len;
                }
                m_inputLen  = len;
                m_cursorPos = len;
            }
            break;
        }
        default:
            break;
    }
}

void DebugConsole::EmitConsoleOverlay(platform::Extent2i viewportSize, DebugBatcher& hudBatcher) const noexcept {
    if (!m_isOpen) return;

    const f32 consoleW = static_cast<f32>(viewportSize.w);
    const f32 consoleH = static_cast<f32>(viewportSize.h) * 0.45f;

    hudBatcher.PushSolidRect({0.0f, 0.0f, consoleW, consoleH}, renderer::Color::FromRGBA8(0x0A0E14EE));
    hudBatcher.PushLine(0.0f, consoleH, consoleW, consoleH, 2.0f, renderer::Color::FromRGBA8(0x00E5FFFF));

    const u32 maxVisibleLines = static_cast<u32>((consoleH - 32.0f) / 11.0f);
    const u32 visibleCount = std::min(m_logCount, maxVisibleLines);

    for (u32 i = 0; i < visibleCount; ++i) {
        const u32 logOffset = visibleCount - 1u - i + m_scrollOffset;
        if (logOffset >= m_logCount) continue;

        const u32 ringIdx = (m_logWriteIdx + kMaxLogEntries - 1u - logOffset) & (kMaxLogEntries - 1u);
        const LogEntryMeta& meta = m_logMeta[ringIdx];
        const std::string_view text(&m_logText[meta.textOffset], meta.textLength);

        renderer::Color col = renderer::Color::FromRGBA8(0xD0D8E8FF);
        switch (meta.severity) {
            case LogSeverity::Success: col = renderer::Color::FromRGBA8(0x00FF88FF); break;
            case LogSeverity::Warning: col = renderer::Color::FromRGBA8(0xFFCC00FF); break;
            case LogSeverity::Error:   col = renderer::Color::FromRGBA8(0xFF4444FF); break;
            case LogSeverity::Echo:    col = renderer::Color::FromRGBA8(0x66CCFFFF); break;
            default: break;
        }

        const f32 lineY = 8.0f + static_cast<f32>(i) * 11.0f;
        hudBatcher.PushText(10.0f, lineY, text, col);
    }

    const f32 inputY = consoleH - 20.0f;
    hudBatcher.PushSolidRect({6.0f, inputY - 3.0f, consoleW - 12.0f, 16.0f}, renderer::Color::FromRGBA8(0x161F2CFF));
    hudBatcher.PushText(10.0f, inputY, "> ", renderer::Color::FromRGBA8(0x00E5FFFF));
    hudBatcher.PushText(26.0f, inputY, std::string_view(m_inputBuf.data(), m_inputLen),
                        renderer::Color::FromRGBA8(0xFFFFFFFF));

    const f32 cursorX = 26.0f + static_cast<f32>(m_cursorPos) * 8.0f;
    hudBatcher.PushSolidRect({cursorX, inputY, 6.0f, 9.0f}, renderer::Color::FromRGBA8(0x00FF88CC));
}

} // namespace en::debug

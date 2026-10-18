#include "debug/DebugProfilerAndConsole.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace en::debug {

namespace {

inline constexpr u16 kKeyReturn    = 40;
inline constexpr u16 kKeyEscape    = 41;
inline constexpr u16 kKeyBackspace = 42;
inline constexpr u16 kKeyTab       = 43;
inline constexpr u16 kKeyHome      = 74;
inline constexpr u16 kKeyPageUp    = 75;
inline constexpr u16 kKeyDelete    = 76;
inline constexpr u16 kKeyEnd       = 77;
inline constexpr u16 kKeyPageDown  = 78;
inline constexpr u16 kKeyRight     = 79;
inline constexpr u16 kKeyLeft      = 80;
inline constexpr u16 kKeyDown      = 81;
inline constexpr u16 kKeyUp        = 82;

inline constexpr std::array<std::string_view, 8> kChannelNames{{
    "World", "Player", "Enemy", "Proj", "Vfx", "Camera", "Ui", "Custom"
}};

[[nodiscard]] constexpr u32 QuantizeFrameBucket(u16 frameTotalUs) noexcept {
    // 256 us per bucket (>> 8), covering 0..16.38 ms across 64 buckets.
    return std::min<u32>(static_cast<u32>(frameTotalUs) >> 8u, 63u);
}

} // namespace

void DebugProfiler::BindArena(std::span<FrameProfileSample, kHistoryFrames> samples) noexcept {
    m_samples = samples;
    Clear();
}

void DebugProfiler::Clear() noexcept {
    for (FrameProfileSample& s : m_samples) {
        s = FrameProfileSample{};
    }
    m_buckets.fill(0);
    m_activeBucketMask = 0;
    m_currentIdx       = 0;
    m_validFrames      = 0;
}

void DebugProfiler::BeginFrame(u64 frameIndex, u64 simTickIndex) noexcept {
    if (m_samples.empty()) {
        return;
    }
    if (m_validFrames > 0) {
        m_currentIdx = (m_currentIdx + 1u) & static_cast<u32>(kHistoryFrames - 1);
    }
    if (m_validFrames == kHistoryFrames) {
        const u16 oldUs     = m_samples[m_currentIdx].scopeUs[static_cast<usize>(ProfileScopeId::FrameTotal)];
        const u32 oldBucket = QuantizeFrameBucket(oldUs);
        if (m_buckets[oldBucket] > 0 && --m_buckets[oldBucket] == 0) {
            m_activeBucketMask &= ~(1ull << oldBucket);
        }
    }

    FrameProfileSample& cur = m_samples[m_currentIdx];
    cur              = FrameProfileSample{};
    cur.frameIndex   = frameIndex;
    cur.simTickIndex = simTickIndex;
    cur.channelScaleQ8.fill(256);
}

void DebugProfiler::RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept {
    if (m_samples.empty()) {
        return;
    }
    const usize idx = static_cast<usize>(scope);
    if (idx < kProfileScopeCount) {
        m_samples[m_currentIdx].scopeUs[idx] = static_cast<u16>(std::min<u32>(durationUs, 65535u));
    }
}

void DebugProfiler::RecordSubsystemTelemetry(const SubsystemTelemetryDesc& desc) noexcept {
    if (m_samples.empty()) {
        return;
    }
    FrameProfileSample& s = m_samples[m_currentIdx];
    s.aliveEntities       = desc.aliveEntities;
    s.submittedQuads      = desc.submittedQuads;
    s.culledQuads         = desc.culledQuads;
    s.drawBatches         = desc.drawBatches;
    s.catchupTicks        = desc.catchupTicks;
    s.radixPassesExecuted = desc.radixPasses;
    s.frozenChannelMask   = desc.frozenChannelMask;
    s.activePhysVoices    = desc.activePhysVoices;
    s.activeVirtVoices    = desc.activeVirtVoices;
    s.audioUnderruns      = desc.audioUnderruns;
    s.audioCpuPermille    = static_cast<u16>(std::clamp(desc.audioCpuLoadRatio * 1000.0f, 0.0f, 65535.0f));
    s.audioPeakL_Q10      = static_cast<u16>(std::clamp(desc.audioPeakL * 1024.0f, 0.0f, 65535.0f));
    s.audioPeakR_Q10      = static_cast<u16>(std::clamp(desc.audioPeakR * 1024.0f, 0.0f, 65535.0f));

    const usize count = std::min<usize>(desc.channelScales.size(), 8u);
    for (usize i = 0; i < count; ++i) {
        s.channelScaleQ8[i] = static_cast<u16>(std::clamp(desc.channelScales[i] * 256.0f, 0.0f, 65535.0f));
    }
}

void DebugProfiler::EndFrame() noexcept {
    if (m_samples.empty()) {
        return;
    }
    const u16 totalUs = m_samples[m_currentIdx].scopeUs[static_cast<usize>(ProfileScopeId::FrameTotal)];
    const u32 bucket  = QuantizeFrameBucket(totalUs);
    ++m_buckets[bucket];
    m_activeBucketMask |= (1ull << bucket);
    if (m_validFrames < kHistoryFrames) {
        ++m_validFrames;
    }
}

u32 DebugProfiler::ComputePercentileUs(u32 targetRankOutOf256) const noexcept {
    if (m_validFrames == 0 || m_activeBucketMask == 0ull) {
        return 0;
    }
    const u32 rankTarget = std::max(1u, (std::min(targetRankOutOf256, 256u) * m_validFrames) >> 8u);
    u32 cumulative       = 0;
    u64 mask             = m_activeBucketMask;

    while (mask != 0ull) {
        const u32 bucket = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1ull);
        cumulative += m_buckets[bucket];
        if (cumulative >= rankTarget) {
            return (bucket << 8u) + 128u;
        }
    }
    return 16384u;
}

void DebugProfiler::EmitProfilerAndTimeHud(OverlayFlags flags,
                                           platform::Extent2i,
                                           DebugBatcher& hudBatcher) const noexcept {
    if (m_samples.empty()) {
        return;
    }
    const FrameProfileSample& cur = m_samples[m_currentIdx];
    f32 panelY = 12.0f;

    if (HasOverlay(flags, OverlayFlags::ProfilerHud)) {
        const f32 x = 12.0f;
        const f32 y = panelY;
        const f32 w = 276.0f;
        const f32 h = 132.0f;

        hudBatcher.PushSolidRect({x, y, w, h}, renderer::Color::FromRGBA8(0x10141EE0u));
        hudBatcher.PushBoxOutline({x, y, w, h}, 1.0f, renderer::Color::FromRGBA8(0x4488CCFFu));

        const f32 frameMs = static_cast<f32>(cur.scopeUs[0]) * 0.001f;
        const f32 p50Ms   = static_cast<f32>(ComputePercentileUs(128)) * 0.001f;
        const f32 p99Ms   = static_cast<f32>(ComputePercentileUs(253)) * 0.001f;

        DebugTextLine line{};
        line.Str("FRAME ").F32(frameMs).Str("ms  P50:").F32(p50Ms).Str("  P99:").F32(p99Ms);
        hudBatcher.PushText(x + 8.0f, y + 6.0f, line.View(), renderer::Color::FromRGBA8(0x88FFAAFFu));

        line.Clear();
        line.Str("Sim:").F32(static_cast<f32>(cur.scopeUs[3]) * 0.001f)
            .Str("ms Rdr:").F32(static_cast<f32>(cur.scopeUs[11]) * 0.001f)
            .Str("ms Ticks:").I64(cur.catchupTicks);
        hudBatcher.PushText(x + 8.0f, y + 18.0f, line.View(), renderer::Color::FromRGBA8(0xDDE8FFFFu));

        line.Clear();
        line.Str("Ent:").I64(cur.aliveEntities)
            .Str(" Quads:").I64(cur.submittedQuads)
            .Str(" Bch:").I64(cur.drawBatches)
            .Str(" Rdx:").I64(cur.radixPassesExecuted);
        hudBatcher.PushText(x + 8.0f, y + 30.0f, line.View(), renderer::Color::FromRGBA8(0xAACCFFFFu));

        line.Clear();
        line.Str("AudVoices:").I64(cur.activePhysVoices).Char('/').I64(cur.activeVirtVoices)
            .Str(" Cpu:").F32(static_cast<f32>(cur.audioCpuPermille) * 0.1f).Char('%');
        hudBatcher.PushText(x + 8.0f, y + 42.0f, line.View(), renderer::Color::FromRGBA8(0xFFCC88FFu));

        const f32 graphX = x + 10.0f;
        const f32 graphY = y + 58.0f;
        const f32 graphH = 64.0f;
        hudBatcher.PushSolidRect({graphX, graphY, 256.0f, graphH}, renderer::Color::FromRGBA8(0x080A10FFu));

        const u32 count = std::min<u32>(m_validFrames, 256u);
        for (u32 i = 0; i < count; ++i) {
            const u32 idx   = (m_currentIdx + kHistoryFrames - i) & static_cast<u32>(kHistoryFrames - 1);
            const f32 ms    = static_cast<f32>(m_samples[idx].scopeUs[0]) * 0.001f;
            const f32 barH  = std::clamp(ms * 3.5f, 1.0f, graphH);
            const renderer::Color col = (ms > 16.67f)
                ? renderer::Color::FromRGBA8(0xFF4444FFu)
                : (ms > 8.33f ? renderer::Color::FromRGBA8(0xFFCC33FFu)
                              : renderer::Color::FromRGBA8(0x33DD77FFu));
            hudBatcher.PushSolidRect({graphX + 255.0f - static_cast<f32>(i), graphY + graphH - barH, 1.0f, barH}, col);
        }

        panelY += h + 8.0f;
    }

    if (HasOverlay(flags, OverlayFlags::TimeChannelHud)) {
        const f32 x = 12.0f;
        const f32 y = panelY;
        const f32 w = 220.0f;
        const f32 h = 104.0f;

        hudBatcher.PushSolidRect({x, y, w, h}, renderer::Color::FromRGBA8(0x10141EE0u));
        hudBatcher.PushBoxOutline({x, y, w, h}, 1.0f, renderer::Color::FromRGBA8(0x4488CCFFu));
        hudBatcher.PushText(x + 8.0f, y + 6.0f, "TIME CHANNELS (120Hz)", renderer::Color::FromRGBA8(0xFFFF88FFu));

        for (u8 ch = 0; ch < 8; ++ch) {
            const f32 rowY    = y + 18.0f + static_cast<f32>(ch) * 10.0f;
            const bool frozen = (cur.frozenChannelMask & (1u << ch)) != 0;
            const f32 scale   = static_cast<f32>(cur.channelScaleQ8[ch]) * (1.0f / 256.0f);

            hudBatcher.PushText(x + 8.0f, rowY, kChannelNames[ch], renderer::Color::FromRGBA8(0xCCD8E8FFu));
            const f32 barW = std::clamp(scale * 60.0f, 0.0f, 120.0f);
            const renderer::Color barCol = frozen
                ? renderer::Color::FromRGBA8(0xFF4466FFu)
                : renderer::Color::FromRGBA8(0x44CCFFFFu);
            hudBatcher.PushSolidRect({x + 68.0f, rowY + 1.0f, barW, 6.0f}, barCol);
            if (frozen) {
                hudBatcher.PushText(x + 156.0f, rowY, "FROZEN", renderer::Color::FromRGBA8(0xFF6688FFu));
            }
        }
    }
}

ConsoleArgs::ConsoleArgs(std::string_view rawLine) noexcept {
    usize i = 0;
    const usize n = rawLine.size();
    while (i < n && m_count < kMaxTokens) {
        while (i < n && static_cast<u8>(rawLine[i]) <= 0x20u) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        if (rawLine[i] == '"') {
            const usize start = ++i;
            while (i < n && rawLine[i] != '"') {
                ++i;
            }
            m_tokens[m_count++] = rawLine.substr(start, i - start);
            if (i < n) {
                ++i;
            }
        } else {
            const usize start = i;
            while (i < n && static_cast<u8>(rawLine[i]) > 0x20u) {
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
    const std::string_view tok = m_tokens[idx];
    i64 val = 0;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
    if (ec != std::errc{} || ptr != tok.data() + tok.size()) {
        return Err(DebugErrc::InvalidArguments);
    }
    return val;
}

Result<f32, DebugErrc> ConsoleArgs::GetFloat(usize idx) const noexcept {
    if (idx >= m_count || m_tokens[idx].empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const std::string_view tok = m_tokens[idx];
    f32 val = 0.0f;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
    if (ec != std::errc{} || ptr != tok.data() + tok.size() || !std::isfinite(val)) {
        return Err(DebugErrc::InvalidArguments);
    }
    return val;
}

Result<bool, DebugErrc> ConsoleArgs::GetBool(usize idx) const noexcept {
    if (idx >= m_count || m_tokens[idx].empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const std::string_view t = m_tokens[idx];
    if (t == "1" || t == "true" || t == "on" || t == "yes") {
        return true;
    }
    if (t == "0" || t == "false" || t == "off" || t == "no") {
        return false;
    }
    return Err(DebugErrc::InvalidArguments);
}

void DebugConsole::BindArena(std::span<ConsoleSymbol, kMaxSymbols> symbolTable,
                             std::span<LogEntryMeta, kMaxLogEntries> logMeta,
                             std::span<char, kLogTextBytes> logText,
                             std::span<std::array<char, kMaxLineChars>, kMaxHistory> historyBuf) noexcept {
    m_symbols = symbolTable;
    m_logMeta = logMeta;
    m_logText = logText;
    m_history = historyBuf;

    for (ConsoleSymbol& sym : m_symbols) {
        sym = ConsoleSymbol{};
    }
    for (auto& h : m_history) {
        h.fill('\0');
    }
    m_pendingCount     = 0;
    m_inputLen         = 0;
    m_cursorPos        = 0;
    m_historyWriteIdx  = 0;
    m_historyCount     = 0;
    m_historyBrowseIdx = -1;
    m_isOpen           = false;
    ClearLog();
}

void DebugConsole::ClearLog() noexcept {
    for (LogEntryMeta& meta : m_logMeta) {
        meta = LogEntryMeta{};
    }
    if (!m_logText.empty()) {
        std::memset(m_logText.data(), 0, m_logText.size());
    }
    m_textWriteCursor = 0;
    m_logWriteIdx     = 0;
    m_logCount        = 0;
    m_scrollOffset    = 0;
}

Result<ConsoleSymbol*, DebugErrc> DebugConsole::InsertSymbolSlot(std::string_view staticName) noexcept {
    if (staticName.empty() || staticName.size() > 255 || m_symbols.empty()) {
        return Err(DebugErrc::InvalidArguments);
    }
    const u32 hash  = HashSymbolName(staticName);
    u32 slot        = (hash * 2654435769u) >> 24u;
    for (usize i = 0; i < kMaxSymbols; ++i) {
        ConsoleSymbol& sym = m_symbols[slot];
        if (sym.kind == SymbolKind::Empty) {
            sym.nameHash = hash;
            sym.nameLen  = static_cast<u8>(staticName.size());
            sym.namePtr  = staticName.data();
            return &sym;
        }
        if (sym.nameHash == hash && sym.nameLen == staticName.size() &&
            std::string_view(sym.namePtr, sym.nameLen) == staticName) {
            return Err(DebugErrc::DuplicateSymbol);
        }
        slot = (slot + 1u) & static_cast<u32>(kMaxSymbols - 1);
    }
    return Err(DebugErrc::SymbolTableFull);
}

ConsoleSymbol* DebugConsole::FindSymbol(std::string_view name) noexcept {
    if (name.empty() || m_symbols.empty()) {
        return nullptr;
    }
    const u32 hash = HashSymbolName(name);
    u32 slot       = (hash * 2654435769u) >> 24u;
    for (usize i = 0; i < kMaxSymbols; ++i) {
        ConsoleSymbol& sym = m_symbols[slot];
        if (sym.kind == SymbolKind::Empty) {
            return nullptr;
        }
        if (sym.nameHash == hash && sym.nameLen == name.size() &&
            std::string_view(sym.namePtr, sym.nameLen) == name) {
            return &sym;
        }
        slot = (slot + 1u) & static_cast<u32>(kMaxSymbols - 1);
    }
    return nullptr;
}

Result<void, DebugErrc> DebugConsole::RegisterCommand(
    std::string_view staticName, ConsoleCmdCallback fn, void* userCtx) noexcept {
    if (fn == nullptr) {
        return Err(DebugErrc::InvalidArguments);
    }
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::Command;
    sym->targetPtr = userCtx;
    sym->cmdFn     = fn;
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarF32(
    std::string_view staticName, f32* target, f32 minVal, f32 maxVal) noexcept {
    if (target == nullptr || minVal > maxVal) {
        return Err(DebugErrc::InvalidArguments);
    }
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarF32;
    sym->targetPtr = target;
    sym->f32Bounds = {minVal, maxVal};
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarI32(
    std::string_view staticName, i32* target, i32 minVal, i32 maxVal) noexcept {
    if (target == nullptr || minVal > maxVal) {
        return Err(DebugErrc::InvalidArguments);
    }
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarI32;
    sym->targetPtr = target;
    sym->i32Bounds = {minVal, maxVal};
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarU32(
    std::string_view staticName, u32* target, u32 minVal, u32 maxVal) noexcept {
    if (target == nullptr || minVal > maxVal) {
        return Err(DebugErrc::InvalidArguments);
    }
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarU32;
    sym->targetPtr = target;
    sym->u32Bounds = {minVal, maxVal};
    return Ok;
}

Result<void, DebugErrc> DebugConsole::RegisterCVarBool(
    std::string_view staticName, bool* target) noexcept {
    if (target == nullptr) {
        return Err(DebugErrc::InvalidArguments);
    }
    auto slotRes = InsertSymbolSlot(staticName);
    if (!slotRes) {
        return Err(slotRes.error());
    }
    ConsoleSymbol* sym = *slotRes;
    sym->kind      = SymbolKind::CVarBool;
    sym->targetPtr = target;
    return Ok;
}

void DebugConsole::Print(LogSeverity severity, std::string_view msg, u64 tickIndex) noexcept {
    if (m_logMeta.empty() || m_logText.empty() || msg.empty()) {
        return;
    }
    const u8 len = static_cast<u8>(std::min<usize>(msg.size(), 255u));
    if (static_cast<usize>(m_textWriteCursor) + len > kLogTextBytes) {
        m_textWriteCursor = 0;
    }
    std::memcpy(m_logText.data() + m_textWriteCursor, msg.data(), len);

    const u32 slot       = m_logWriteIdx & static_cast<u32>(kMaxLogEntries - 1);
    m_logMeta[slot]      = {m_textWriteCursor, static_cast<u16>(tickIndex), len, severity, 0};
    m_textWriteCursor    = static_cast<u16>(m_textWriteCursor + len);
    ++m_logWriteIdx;
    if (m_logCount < kMaxLogEntries) {
        ++m_logCount;
    }
}

void DebugConsole::EnqueueCommandLine(std::string_view line) noexcept {
    if (line.empty() || m_pendingCount >= kMaxPendingCmds) {
        return;
    }
    const u8 len = static_cast<u8>(std::min<usize>(line.size(), kMaxLineChars));
    std::memcpy(m_pendingCmds[m_pendingCount].data(), line.data(), len);
    m_pendingLens[m_pendingCount] = len;
    ++m_pendingCount;
}

void DebugConsole::FlushPendingCommands(u64 currentTickIndex) noexcept {
    for (u8 i = 0; i < m_pendingCount; ++i) {
        ExecuteImmediate({m_pendingCmds[i].data(), m_pendingLens[i]}, currentTickIndex);
    }
    m_pendingCount = 0;
}

void DebugConsole::ExecuteImmediate(std::string_view line, u64 currentTickIndex) noexcept {
    const ConsoleArgs args(line);
    if (args.Count() == 0) {
        return;
    }
    Print(LogSeverity::Echo, line, currentTickIndex);

    ConsoleSymbol* sym = FindSymbol(args[0]);
    if (sym == nullptr) {
        DebugTextLine err{};
        err.Str("Unknown symbol: ").Str(args[0]);
        Print(LogSeverity::Error, err.View(), currentTickIndex);
        return;
    }

    if (sym->kind == SymbolKind::Command) {
        sym->cmdFn(sym->targetPtr, args, *this);
        return;
    }

    DebugTextLine out{};
    out.Str({sym->namePtr, sym->nameLen}).Str(" = ");

    if (args.Count() == 1) {
        switch (sym->kind) {
            case SymbolKind::CVarF32:  out.F32(*static_cast<const f32*>(sym->targetPtr)); break;
            case SymbolKind::CVarI32:  out.I64(*static_cast<const i32*>(sym->targetPtr)); break;
            case SymbolKind::CVarU32:  out.I64(*static_cast<const u32*>(sym->targetPtr)); break;
            case SymbolKind::CVarBool: out.Str(*static_cast<const bool*>(sym->targetPtr) ? "true" : "false"); break;
            default: break;
        }
        Print(LogSeverity::Info, out.View(), currentTickIndex);
        return;
    }

    switch (sym->kind) {
        case SymbolKind::CVarF32: {
            const auto res = args.GetFloat(1);
            if (!res) { Print(LogSeverity::Error, "Invalid float argument", currentTickIndex); return; }
            const f32 v = std::clamp(*res, sym->f32Bounds.minVal, sym->f32Bounds.maxVal);
            *static_cast<f32*>(sym->targetPtr) = v;
            out.F32(v);
            break;
        }
        case SymbolKind::CVarI32: {
            const auto res = args.GetInt(1);
            if (!res) { Print(LogSeverity::Error, "Invalid integer argument", currentTickIndex); return; }
            const i32 v = static_cast<i32>(std::clamp<i64>(*res, sym->i32Bounds.minVal, sym->i32Bounds.maxVal));
            *static_cast<i32*>(sym->targetPtr) = v;
            out.I64(v);
            break;
        }
        case SymbolKind::CVarU32: {
            const auto res = args.GetInt(1);
            if (!res || *res < 0) { Print(LogSeverity::Error, "Invalid unsigned argument", currentTickIndex); return; }
            const u32 v = static_cast<u32>(std::clamp<i64>(*res, sym->u32Bounds.minVal, sym->u32Bounds.maxVal));
            *static_cast<u32*>(sym->targetPtr) = v;
            out.I64(v);
            break;
        }
        case SymbolKind::CVarBool: {
            const auto res = args.GetBool(1);
            if (!res) { Print(LogSeverity::Error, "Invalid bool argument", currentTickIndex); return; }
            *static_cast<bool*>(sym->targetPtr) = *res;
            out.Str(*res ? "true" : "false");
            break;
        }
        default: break;
    }
    Print(LogSeverity::Success, out.View(), currentTickIndex);
}

void DebugConsole::PushHistoryLine(std::string_view line) noexcept {
    if (m_history.empty() || line.empty()) {
        return;
    }
    const usize len = std::min<usize>(line.size(), kMaxLineChars - 1);
    auto& slot      = m_history[m_historyWriteIdx];
    slot.fill('\0');
    std::memcpy(slot.data(), line.data(), len);
    m_historyWriteIdx = static_cast<u8>((m_historyWriteIdx + 1u) & (kMaxHistory - 1u));
    if (m_historyCount < kMaxHistory) {
        ++m_historyCount;
    }
    m_historyBrowseIdx = -1;
}

void DebugConsole::OnTextInput(std::string_view utf8) noexcept {
    if (!m_isOpen) {
        return;
    }
    for (const char c : utf8) {
        const u8 code = static_cast<u8>(c);
        if (code < 0x20u || code > 0x7Eu || c == '`' || c == '~') {
            continue;
        }
        if (static_cast<usize>(m_inputLen) + 1u < kMaxLineChars) {
            for (u8 i = m_inputLen; i > m_cursorPos; --i) {
                m_inputBuf[i] = m_inputBuf[i - 1];
            }
            m_inputBuf[m_cursorPos++] = c;
            ++m_inputLen;
        }
    }
}

void DebugConsole::OnKeyPress(u16 scancode, bool, bool) noexcept {
    if (!m_isOpen) {
        return;
    }
    switch (scancode) {
        case kKeyReturn: {
            if (m_inputLen > 0) {
                const std::string_view cmd(m_inputBuf.data(), m_inputLen);
                PushHistoryLine(cmd);
                EnqueueCommandLine(cmd);
                m_inputLen     = 0;
                m_cursorPos    = 0;
                m_scrollOffset = 0;
            }
            break;
        }
        case kKeyEscape:
            m_isOpen = false;
            break;
        case kKeyBackspace:
            if (m_cursorPos > 0) {
                for (u8 i = m_cursorPos - 1; i + 1 < m_inputLen; ++i) {
                    m_inputBuf[i] = m_inputBuf[i + 1];
                }
                --m_cursorPos;
                --m_inputLen;
            }
            break;
        case kKeyDelete:
            if (m_cursorPos < m_inputLen) {
                for (u8 i = m_cursorPos; i + 1 < m_inputLen; ++i) {
                    m_inputBuf[i] = m_inputBuf[i + 1];
                }
                --m_inputLen;
            }
            break;
        case kKeyLeft:
            if (m_cursorPos > 0) --m_cursorPos;
            break;
        case kKeyRight:
            if (m_cursorPos < m_inputLen) ++m_cursorPos;
            break;
        case kKeyHome:
            m_cursorPos = 0;
            break;
        case kKeyEnd:
            m_cursorPos = m_inputLen;
            break;
        case kKeyTab:
            AutoCompleteInput();
            break;
        case kKeyUp:
            if (m_historyCount > 0 && !m_history.empty()) {
                if (m_historyBrowseIdx + 1 < static_cast<i8>(m_historyCount)) {
                    ++m_historyBrowseIdx;
                }
                const u8 idx = static_cast<u8>((m_historyWriteIdx + kMaxHistory - 1u - m_historyBrowseIdx) & (kMaxHistory - 1u));
                const std::string_view h(m_history[idx].data());
                m_inputLen  = static_cast<u8>(std::min<usize>(h.size(), kMaxLineChars - 1));
                std::memcpy(m_inputBuf.data(), h.data(), m_inputLen);
                m_cursorPos = m_inputLen;
            }
            break;
        case kKeyDown:
            if (m_historyBrowseIdx > 0 && !m_history.empty()) {
                --m_historyBrowseIdx;
                const u8 idx = static_cast<u8>((m_historyWriteIdx + kMaxHistory - 1u - m_historyBrowseIdx) & (kMaxHistory - 1u));
                const std::string_view h(m_history[idx].data());
                m_inputLen  = static_cast<u8>(std::min<usize>(h.size(), kMaxLineChars - 1));
                std::memcpy(m_inputBuf.data(), h.data(), m_inputLen);
                m_cursorPos = m_inputLen;
            } else {
                m_historyBrowseIdx = -1;
                m_inputLen         = 0;
                m_cursorPos        = 0;
            }
            break;
        case kKeyPageUp:
            if (m_logCount > 1) {
                m_scrollOffset = std::min<u32>(m_scrollOffset + 5u, m_logCount - 1u);
            }
            break;
        case kKeyPageDown:
            m_scrollOffset = (m_scrollOffset > 5u) ? (m_scrollOffset - 5u) : 0u;
            break;
        default:
            break;
    }
}

void DebugConsole::AutoCompleteInput() noexcept {
    if (m_inputLen == 0 || m_symbols.empty()) {
        return;
    }
    const std::string_view prefix(m_inputBuf.data(), m_inputLen);
    for (const char c : prefix) {
        if (c == ' ') return;
    }

    const ConsoleSymbol* best = nullptr;
    for (const ConsoleSymbol& sym : m_symbols) {
        if (sym.kind == SymbolKind::Empty || sym.nameLen < prefix.size()) {
            continue;
        }
        const std::string_view candidate(sym.namePtr, sym.nameLen);
        if (candidate.starts_with(prefix)) {
            if (best == nullptr || candidate < std::string_view(best->namePtr, best->nameLen)) {
                best = &sym;
            }
        }
    }

    if (best != nullptr) {
        const u8 copyLen = static_cast<u8>(std::min<usize>(best->nameLen, kMaxLineChars - 2));
        std::memcpy(m_inputBuf.data(), best->namePtr, copyLen);
        m_inputBuf[copyLen] = ' ';
        m_inputLen          = static_cast<u8>(copyLen + 1u);
        m_cursorPos         = m_inputLen;
    }
}

void DebugConsole::EmitConsoleOverlay(platform::Extent2i viewportSize, DebugBatcher& hudBatcher) const noexcept {
    if (!m_isOpen) {
        return;
    }
    const f32 w = static_cast<f32>(viewportSize.w);
    const f32 h = std::min(static_cast<f32>(viewportSize.h) * 0.45f, 280.0f);

    hudBatcher.PushSolidRect({0.0f, 0.0f, w, h}, renderer::Color::FromRGBA8(0x0B0E17EB));
    hudBatcher.PushSolidRect({0.0f, h - 22.0f, w, 22.0f}, renderer::Color::FromRGBA8(0x161C2EFF));
    hudBatcher.PushLine(0.0f, h, w, h, 1.5f, renderer::Color::FromRGBA8(0x4488CCFF));

    const u32 maxRows  = static_cast<u32>((h - 28.0f) / 11.0f);
    const u32 avail    = (m_logCount > m_scrollOffset) ? (m_logCount - m_scrollOffset) : 0u;
    const u32 showRows = std::min(maxRows, avail);

    for (u32 i = 0; i < showRows; ++i) {
        const u32 backIdx = m_scrollOffset + (showRows - 1u - i);
        const u32 slot    = (m_logWriteIdx + kMaxLogEntries - 1u - backIdx) & static_cast<u32>(kMaxLogEntries - 1);
        const LogEntryMeta& meta = m_logMeta[slot];

        renderer::Color col = renderer::Color::FromRGBA8(0xD0DAE8FFu);
        switch (meta.severity) {
            case LogSeverity::Success: col = renderer::Color::FromRGBA8(0x66FF99FFu); break;
            case LogSeverity::Warning: col = renderer::Color::FromRGBA8(0xFFCC44FFu); break;
            case LogSeverity::Error:   col = renderer::Color::FromRGBA8(0xFF5566FFu); break;
            case LogSeverity::Echo:    col = renderer::Color::FromRGBA8(0x88BBFFFFu); break;
            case LogSeverity::Info:    break;
        }

        const std::string_view txt(m_logText.data() + meta.textOffset, meta.textLength);
        hudBatcher.PushText(8.0f, 6.0f + static_cast<f32>(i) * 11.0f, txt, col);
    }

    DebugTextLine prompt{};
    prompt.Str("> ").Str({m_inputBuf.data(), m_inputLen});
    hudBatcher.PushText(8.0f, h - 16.0f, prompt.View(), renderer::Color::FromRGBA8(0xFFFFFFFFu));

    const f32 cursorX = 8.0f + static_cast<f32>(m_cursorPos + 2u) * DebugBatcher::kGlyphWidth;
    hudBatcher.PushSolidRect({cursorX, h - 16.0f, 6.0f, 9.0f}, renderer::Color::FromRGBA8(0x66CCFFFFu));
}

u64 DebugConsole::ComputeAuditHash() const noexcept {
    u64 h = MixAuditHash(0xC04501E99001ull, m_logCount);
    h = MixAuditHash(h, m_logWriteIdx);
    for (const ConsoleSymbol& sym : m_symbols) {
        if (sym.kind != SymbolKind::Empty) {
            h = MixAuditHash(h, (static_cast<u64>(sym.nameHash) << 8) | static_cast<u8>(sym.kind));
        }
    }
    return h;
}

} // namespace en::debug

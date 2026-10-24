#include "debug/DebugProfilerAndConsole.h"

#include <charconv>
#include <cstdlib>
#include <cstring>

namespace en {

namespace {

[[nodiscard]] constexpr u32 HashConsoleName(std::string_view str) noexcept {
    u32 h = 2166136261U;
    for (char c : str) {
        h ^= static_cast<u8>(c);
        h *= 16777619U;
    }
    return (h == 0U) ? 1U : h;
}

[[nodiscard]] constexpr std::string_view TrimAscii(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1U);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1U);
    }
    return s;
}

inline void PackLineBuffer(std::array<char, 128>& dst, std::string_view text) noexcept {
    const u8 len = static_cast<u8>(std::min<usize>(text.size(), 126U));
    std::memcpy(dst.data(), text.data(), len);
    dst[len] = '\0';
    dst[127] = static_cast<char>(len);
}

[[nodiscard]] inline std::string_view UnpackLineBuffer(const std::array<char, 128>& src) noexcept {
    const u8 len = static_cast<u8>(src[127]) & 0x7FU;
    return {src.data(), std::min<u8>(len, 126U)};
}

} // namespace

void DebugProfiler::BeginFrame(u32 frameIndex, u32 simTickIndex) noexcept {
    if (!m_samples) return;
    const u16 slot = static_cast<u16>(m_currentIdx & (kProfileHistoryFrames - 1U));

    if (m_validFrames >= kProfileHistoryFrames) {
        const u16 oldTotalUs = m_samples[slot].scopeUs[static_cast<usize>(ProfileScopeId::FrameTotal)];
        const u8 oldB = DurationToBucket(oldTotalUs);
        if (m_buckets[oldB] > 0U) {
            if (--m_buckets[oldB] == 0U) {
                m_activeBucketMask &= ~(1ULL << oldB);
            }
        }
    }

    FrameProfileSample& s = m_samples[slot];
    s = FrameProfileSample{};
    s.frameIndex = frameIndex;
    s.simTickIndex = simTickIndex;
}

void DebugProfiler::RecordScopeUs(ProfileScopeId scope, u32 durationUs) noexcept {
    if (!m_samples || scope >= ProfileScopeId::Count) return;
    const u16 slot = static_cast<u16>(m_currentIdx & (kProfileHistoryFrames - 1U));
    m_samples[slot].scopeUs[static_cast<usize>(scope)] =
        static_cast<u16>(std::min<u32>(65535U, durationUs));
}

void DebugProfiler::RecordSubsystemTelemetry(const SubsystemTelemetryDesc& desc) noexcept {
    if (!m_samples) return;
    const u16 slot = static_cast<u16>(m_currentIdx & (kProfileHistoryFrames - 1U));
    FrameProfileSample& s = m_samples[slot];
    s.aliveEntities = desc.aliveEntities;
    s.submittedQuads = desc.submittedQuads;
    s.culledQuads = desc.culledQuads;
    s.drawBatches = desc.drawBatches;
    s.activeContacts = desc.activeContacts;
    s.activeBodies = desc.activeBodies;
    s.catchupTicks = desc.catchupTicks;
    s.audioActiveVoices = desc.audioActiveVoices;
    s.audioVirtualVoices = desc.audioVirtualVoices;
    s.audioStolenVoices = desc.audioStolenVoices;
    s.frozenChannelMask = desc.frozenChannelMask;
    s.radixPasses = desc.radixPasses;
    s.audioMusicStems = desc.audioMusicStems;
    m_spikeThresholdUs = static_cast<u16>(std::max<u8>(1U, desc.spikeThresholdMs) * 1000U);

    if (desc.channelScales) {
        for (usize i = 0U; i < kMaxTimeChannels; ++i) {
            s.channelScaleQ6[i] = static_cast<u8>(std::clamp(desc.channelScales[i] * 64.0f + 0.5f, 0.0f, 255.0f));
        }
    }
}

void DebugProfiler::EndFrame() noexcept {
    if (!m_samples) return;
    const u16 slot = static_cast<u16>(m_currentIdx & (kProfileHistoryFrames - 1U));
    FrameProfileSample& s = m_samples[slot];
    u32 totalUs = s.scopeUs[static_cast<usize>(ProfileScopeId::FrameTotal)];
    if (totalUs == 0U) {
        for (usize i = 1U; i < static_cast<usize>(ProfileScopeId::Count); ++i) {
            totalUs += s.scopeUs[i];
        }
        s.scopeUs[static_cast<usize>(ProfileScopeId::FrameTotal)] =
            static_cast<u16>(std::min<u32>(65535U, totalUs));
    }

    if (totalUs > m_spikeThresholdUs) {
        s.frameSpikeFlag = 1U;
        ++m_spikeCount;
    }

    const u8 b = DurationToBucket(totalUs);
    ++m_buckets[b];
    m_activeBucketMask |= (1ULL << b);

    m_currentIdx = static_cast<u16>((slot + 1U) & (kProfileHistoryFrames - 1U));
    if (m_validFrames < kProfileHistoryFrames) {
        ++m_validFrames;
    }
}

void DebugProfiler::PlotValue(std::string_view label, f32 value) noexcept {
    if (!m_plots || label.empty()) return;
    const u32 h = HashConsoleName(label);
    WatchPlotTrack* target = nullptr;
    for (u32 i = 0U; i < kMaxWatchPlots; ++i) {
        if (m_plots[i].nameHash == h && m_plots[i].Label() == label) {
            target = &m_plots[i];
            break;
        }
        if (!target && m_plots[i].nameHash == 0U) {
            target = &m_plots[i];
            target->nameHash = h;
            target->labelLen = static_cast<u8>(std::min<usize>(label.size(), 15U));
            std::memcpy(target->label.data(), label.data(), target->labelLen);
            target->label[target->labelLen] = '\0';
            target->minVal = value;
            target->maxVal = value;
        }
    }
    if (!target) return;

    const u16 slot = target->writeIdx & (kWatchPlotHistorySamples - 1U);
    target->samples[slot] = value;
    target->writeIdx = static_cast<u16>((slot + 1U) & (kWatchPlotHistorySamples - 1U));
    if (target->validCount < kWatchPlotHistorySamples) {
        ++target->validCount;
    }
    target->minVal = std::min(target->minVal, value);
    target->maxVal = std::max(target->maxVal, value);
}

const WatchPlotTrack* DebugProfiler::FindPlotTrack(std::string_view label) const noexcept {
    if (!m_plots || label.empty()) return nullptr;
    const u32 h = HashConsoleName(label);
    for (u32 i = 0U; i < kMaxWatchPlots; ++i) {
        if (m_plots[i].nameHash == h && m_plots[i].Label() == label) {
            return &m_plots[i];
        }
    }
    return nullptr;
}

u32 DebugProfiler::ComputePercentileUs(u32 targetRankOutOf256) const noexcept {
    if (m_validFrames == 0U || m_activeBucketMask == 0ULL) return 0U;
    const u32 scaledTarget = std::max<u32>(1U, (targetRankOutOf256 * m_validFrames) / kProfileHistoryFrames);
    u32 accumulated = 0U;
    u64 mask = m_activeBucketMask;
    while (mask != 0ULL) {
        const u32 b = static_cast<u32>(std::countr_zero(mask));
        mask &= (mask - 1ULL);
        accumulated += m_buckets[b];
        if (accumulated >= scaledTarget) {
            return (b * 500U) + 250U;
        }
    }
    return 31750U;
}

const FrameProfileSample& DebugProfiler::Latest() const noexcept {
    static constexpr FrameProfileSample kEmpty{};
    if (!m_samples || m_validFrames == 0U) return kEmpty;
    const u32 prevSlot = (static_cast<u32>(m_currentIdx) + kProfileHistoryFrames - 1U) & (kProfileHistoryFrames - 1U);
    return m_samples[prevSlot];
}

void DebugProfiler::EmitProfilerAndTimeHud(
    OverlayFlags flags,
    ViewportExtent viewportSize,
    DebugBatcher& hudBatcher) const noexcept {
    const f32 panelW = 320.0f;
    const f32 baseX = (viewportSize.w > 344) ? (static_cast<f32>(viewportSize.w) - panelW - 12.0f) : 948.0f;
    const f32 subW = 220.0f;
    const f32 subX = baseX + (panelW - subW);

    if (m_samples && m_validFrames > 0U) {
        const FrameProfileSample& latest = Latest();
        if (HasOverlay(flags, OverlayFlags::ProfilerHud)) {
            hudBatcher.PushSolidRect(Rect{baseX, 12.0f, panelW, 118.0f}, Color{0.05f, 0.07f, 0.11f, 0.85f});
            hudBatcher.PushBoxOutline(Rect{baseX, 12.0f, panelW, 118.0f}, 1.0f, Color{0.25f, 0.85f, 0.55f, 0.9f});

            char buf[96]{};
            char* end = buf + sizeof(buf);
            char* p = buf;
            std::memcpy(p, "FRAME ", 6U); p += 6;
            p = DebugBatcher::FormatF32Fixed2(p, end, static_cast<f32>(latest.scopeUs[0]) * 0.001f);
            std::memcpy(p, "ms P99 ", 7U); p += 7;
            p = DebugBatcher::FormatF32Fixed2(p, end, static_cast<f32>(ComputePercentileUs(253U)) * 0.001f);
            std::memcpy(p, "ms", 2U); p += 2;
            hudBatcher.PushText(baseX + 8.0f, 20.0f, {buf, static_cast<usize>(p - buf)}, Color{0.3f, 1.0f, 0.6f, 1.0f}, 1.1f);

            p = buf;
            std::memcpy(p, "ENT ", 4U); p += 4;
            p = DebugBatcher::FormatI64(p, end, latest.aliveEntities);
            std::memcpy(p, " QUADS ", 7U); p += 7;
            p = DebugBatcher::FormatI64(p, end, latest.submittedQuads);
            std::memcpy(p, " BATCH ", 7U); p += 7;
            p = DebugBatcher::FormatI64(p, end, latest.drawBatches);
            hudBatcher.PushText(baseX + 8.0f, 38.0f, {buf, static_cast<usize>(p - buf)}, Color{0.85f, 0.95f, 1.0f, 1.0f}, 1.0f);

            p = buf;
            std::memcpy(p, "PHYS CONT ", 10U); p += 10;
            p = DebugBatcher::FormatI64(p, end, latest.activeContacts);
            std::memcpy(p, " AUD V ", 7U); p += 7;
            p = DebugBatcher::FormatI64(p, end, latest.audioActiveVoices);
            std::memcpy(p, "/", 1U); p += 1;
            p = DebugBatcher::FormatI64(p, end, latest.audioVirtualVoices);
            hudBatcher.PushText(baseX + 8.0f, 54.0f, {buf, static_cast<usize>(p - buf)}, Color{0.9f, 0.85f, 0.4f, 1.0f}, 1.0f);

            const u32 count = std::min<u32>(m_validFrames, 64U);
            for (u32 i = 0U; i < count; ++i) {
                const u32 idx = (static_cast<u32>(m_currentIdx) + kProfileHistoryFrames - 1U - i) & (kProfileHistoryFrames - 1U);
                const f32 ms = static_cast<f32>(m_samples[idx].scopeUs[0]) * 0.001f;
                const f32 h = std::clamp(ms * 2.5f, 2.0f, 48.0f);
                const Color barCol = (m_samples[idx].frameSpikeFlag != 0U)
                                         ? Color{1.0f, 0.25f, 0.25f, 0.9f}
                                         : Color{0.2f, 0.85f, 0.55f, 0.85f};
                const f32 bx = (baseX + panelW - 12.0f) - static_cast<f32>(i) * 4.5f;
                hudBatcher.PushSolidRect(Rect{bx, 122.0f - h, 3.5f, h}, barCol);
            }
        }

        if (HasOverlay(flags, OverlayFlags::TimeChannelHud)) {
            hudBatcher.PushSolidRect(Rect{subX, 136.0f, subW, 36.0f}, Color{0.05f, 0.07f, 0.11f, 0.85f});
            char buf[64]{};
            char* end = buf + sizeof(buf);
            char* p = buf;
            std::memcpy(p, "WRLD ", 5U); p += 5;
            p = DebugBatcher::FormatF32Fixed2(p, end, static_cast<f32>(latest.channelScaleQ6[0]) * (1.0f / 64.0f));
            std::memcpy(p, "x PLR ", 6U); p += 6;
            p = DebugBatcher::FormatF32Fixed2(p, end, static_cast<f32>(latest.channelScaleQ6[1]) * (1.0f / 64.0f));
            *p++ = 'x';
            hudBatcher.PushText(subX + 8.0f, 148.0f, {buf, static_cast<usize>(p - buf)}, Color{0.6f, 0.9f, 1.0f, 1.0f}, 1.0f);
        }
    }

    // Real-time WatchPlotHud sparkline waveforms (Top-Right aligned below profiler)
    if (HasOverlay(flags, OverlayFlags::WatchPlotHud) && m_plots) {
        f32 plotY = 178.0f;
        for (u32 t = 0U; t < kMaxWatchPlots; ++t) {
            const WatchPlotTrack& tr = m_plots[t];
            if (tr.nameHash == 0U || tr.validCount == 0U) continue;
            hudBatcher.PushSolidRect(Rect{subX, plotY, subW, 38.0f}, Color{0.05f, 0.07f, 0.11f, 0.85f});
            hudBatcher.PushText(subX + 6.0f, plotY + 4.0f, tr.Label(), Color{1.0f, 0.85f, 0.3f, 1.0f}, 1.0f);

            const f32 span = std::max(1e-3f, tr.maxVal - tr.minVal);
            f32 prevX = 0.0f, prevY = 0.0f;
            for (u8 s = 0U; s < tr.validCount; ++s) {
                const u32 idx = (static_cast<u32>(tr.writeIdx) + kWatchPlotHistorySamples - tr.validCount + s) &
                                (kWatchPlotHistorySamples - 1U);
                const f32 norm = std::clamp((tr.samples[idx] - tr.minVal) / span, 0.0f, 1.0f);
                const f32 sx = subX + 6.0f + static_cast<f32>(s) * 3.1f;
                const f32 sy = plotY + 34.0f - norm * 18.0f;
                if (s > 0U) {
                    hudBatcher.PushLine(prevX, prevY, sx, sy, 1.2f, Color{0.3f, 0.95f, 1.0f, 0.95f});
                }
                prevX = sx;
                prevY = sy;
            }
            plotY += 42.0f;
        }
    }
}

u64 DebugProfiler::ComputeAuditHash() const noexcept {
    u64 h = 14695981039346656037ULL;
    h ^= m_activeBucketMask;
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_validFrames);
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_spikeCount);
    h *= 1099511628211ULL;
    return h;
}

void DebugConsole::Init(
    std::span<ConsoleSymbol, kMaxConsoleSymbols> symbolStorage,
    std::span<LogEntryMeta, kMaxConsoleLogs> logMetaStorage,
    std::span<char, kConsoleLogTextBytes> logTextStorage) noexcept {
    m_symbols = symbolStorage.data();
    m_logMeta = logMetaStorage.data();
    m_logText = logTextStorage.data();
    Clear();
}

void DebugConsole::Clear() noexcept {
    if (m_symbols) {
        for (u32 i = 0U; i < kMaxConsoleSymbols; ++i) {
            m_symbols[i] = ConsoleSymbol{};
        }
    }
    if (m_logMeta) {
        for (u32 i = 0U; i < kMaxConsoleLogs; ++i) {
            m_logMeta[i] = LogEntryMeta{};
        }
    }
    if (m_logText) {
        std::memset(m_logText, 0, kConsoleLogTextBytes);
    }
    m_symbolHash = 14695981039346656037ULL;
    m_executedCmdCount = 0ULL;
    m_autocompleteCursor = 0ULL;
    m_logByteWriteCursor = 0U;
    m_logWriteIdx = 0U;
    m_logCount = 0U;
    m_symbolCount = 0U;
    m_historyWriteIdx = 0U;
    m_historyCount = 0U;
    m_historyNavOffset = -1;
    m_pendingCount = 0U;
    m_inputLen = 0U;
    m_isOpen = 0U;
    m_inputLine.fill('\0');
}

ConsoleSymbol* DebugConsole::FindOrInsertSlot(std::string_view name, bool allowInsert) noexcept {
    if (!m_symbols || name.empty() || name.size() > 63U) return nullptr;
    const u32 h = HashConsoleName(name);
    u32 slot = (h * 2654435769U) >> 24U;
    for (u32 probe = 0U; probe < kMaxConsoleSymbols; ++probe) {
        ConsoleSymbol& sym = m_symbols[slot];
        if (sym.kind == ConsoleSymbolKind::Empty) {
            return allowInsert ? &sym : nullptr;
        }
        if (sym.nameHash == h && sym.Name() == name) {
            return &sym;
        }
        slot = (slot + 1U) & (kMaxConsoleSymbols - 1U);
    }
    return nullptr;
}

Result<void, DebugErrc> DebugConsole::RegisterCommand(
    std::string_view name,
    void (*fn)(std::string_view args, void* userCtx, DebugConsole& console) noexcept,
    void* userCtx) noexcept {
    if (!m_symbols || name.empty() || !fn) return Err(DebugErrc::InvalidArguments);
    if (m_symbolCount >= kMaxConsoleSymbols - 16U) return Err(DebugErrc::SymbolTableFull);

    ConsoleSymbol* sym = FindOrInsertSlot(name, true);
    if (!sym) return Err(DebugErrc::SymbolTableFull);
    if (sym->kind != ConsoleSymbolKind::Empty) return Err(DebugErrc::DuplicateSymbol);

    sym->namePtr = name.data();
    sym->targetPtr = userCtx;
    sym->spec.cmdFn = fn;
    sym->nameHash = HashConsoleName(name);
    sym->kind = ConsoleSymbolKind::Command;
    sym->nameLen = static_cast<u8>(name.size());
    ++m_symbolCount;
    m_symbolHash ^= sym->nameHash;
    m_symbolHash *= 1099511628211ULL;
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterCVarF32(
    std::string_view name,
    f32* target,
    f32 minVal,
    f32 maxVal) noexcept {
    if (!m_symbols || name.empty() || !target || minVal > maxVal) return Err(DebugErrc::InvalidArguments);
    if (m_symbolCount >= kMaxConsoleSymbols - 16U) return Err(DebugErrc::SymbolTableFull);

    ConsoleSymbol* sym = FindOrInsertSlot(name, true);
    if (!sym) return Err(DebugErrc::SymbolTableFull);
    if (sym->kind != ConsoleSymbolKind::Empty) return Err(DebugErrc::DuplicateSymbol);

    sym->namePtr = name.data();
    sym->targetPtr = target;
    sym->spec.f32Bounds = {minVal, maxVal};
    sym->nameHash = HashConsoleName(name);
    sym->kind = ConsoleSymbolKind::CVarF32;
    sym->nameLen = static_cast<u8>(name.size());
    ++m_symbolCount;
    m_symbolHash ^= sym->nameHash;
    m_symbolHash *= 1099511628211ULL;
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterCVarI32(
    std::string_view name,
    i32* target,
    i32 minVal,
    i32 maxVal) noexcept {
    if (!m_symbols || name.empty() || !target || minVal > maxVal) return Err(DebugErrc::InvalidArguments);
    if (m_symbolCount >= kMaxConsoleSymbols - 16U) return Err(DebugErrc::SymbolTableFull);

    ConsoleSymbol* sym = FindOrInsertSlot(name, true);
    if (!sym) return Err(DebugErrc::SymbolTableFull);
    if (sym->kind != ConsoleSymbolKind::Empty) return Err(DebugErrc::DuplicateSymbol);

    sym->namePtr = name.data();
    sym->targetPtr = target;
    sym->spec.i32Bounds = {minVal, maxVal};
    sym->nameHash = HashConsoleName(name);
    sym->kind = ConsoleSymbolKind::CVarI32;
    sym->nameLen = static_cast<u8>(name.size());
    ++m_symbolCount;
    m_symbolHash ^= sym->nameHash;
    m_symbolHash *= 1099511628211ULL;
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterCVarU32(
    std::string_view name,
    u32* target,
    u32 minVal,
    u32 maxVal) noexcept {
    if (!m_symbols || name.empty() || !target || minVal > maxVal) return Err(DebugErrc::InvalidArguments);
    if (m_symbolCount >= kMaxConsoleSymbols - 16U) return Err(DebugErrc::SymbolTableFull);

    ConsoleSymbol* sym = FindOrInsertSlot(name, true);
    if (!sym) return Err(DebugErrc::SymbolTableFull);
    if (sym->kind != ConsoleSymbolKind::Empty) return Err(DebugErrc::DuplicateSymbol);

    sym->namePtr = name.data();
    sym->targetPtr = target;
    sym->spec.u32Bounds = {minVal, maxVal};
    sym->nameHash = HashConsoleName(name);
    sym->kind = ConsoleSymbolKind::CVarU32;
    sym->nameLen = static_cast<u8>(name.size());
    ++m_symbolCount;
    m_symbolHash ^= sym->nameHash;
    m_symbolHash *= 1099511628211ULL;
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterCVarBool(
    std::string_view name,
    bool* target) noexcept {
    if (!m_symbols || name.empty() || !target) return Err(DebugErrc::InvalidArguments);
    if (m_symbolCount >= kMaxConsoleSymbols - 16U) return Err(DebugErrc::SymbolTableFull);

    ConsoleSymbol* sym = FindOrInsertSlot(name, true);
    if (!sym) return Err(DebugErrc::SymbolTableFull);
    if (sym->kind != ConsoleSymbolKind::Empty) return Err(DebugErrc::DuplicateSymbol);

    sym->namePtr = name.data();
    sym->targetPtr = target;
    sym->nameHash = HashConsoleName(name);
    sym->kind = ConsoleSymbolKind::CVarBool;
    sym->nameLen = static_cast<u8>(name.size());
    ++m_symbolCount;
    m_symbolHash ^= sym->nameHash;
    m_symbolHash *= 1099511628211ULL;
    return {};
}

void DebugConsole::Print(LogSeverity severity, std::string_view msg, u32 tickIndex) noexcept {
    if (!m_logMeta || !m_logText || msg.empty()) return;
    const u8 copyLen = static_cast<u8>(std::min<usize>(msg.size(), 255U));
    if (m_logByteWriteCursor + copyLen > kConsoleLogTextBytes) {
        m_logByteWriteCursor = 0U;
    }
    std::memcpy(m_logText + m_logByteWriteCursor, msg.data(), copyLen);

    const u16 slot = m_logWriteIdx & (kMaxConsoleLogs - 1U);
    m_logMeta[slot] = LogEntryMeta{
        m_logByteWriteCursor,
        static_cast<u16>(tickIndex & 0xFFFFU),
        copyLen,
        severity,
    };
    m_logByteWriteCursor += copyLen;
    m_logWriteIdx = static_cast<u16>((slot + 1U) & (kMaxConsoleLogs - 1U));
    if (m_logCount < kMaxConsoleLogs) {
        ++m_logCount;
    }
}

void DebugConsole::ListAllCVars(u32 tickIndex) noexcept {
    if (!m_symbols) return;
    for (u32 i = 0U; i < kMaxConsoleSymbols; ++i) {
        const ConsoleSymbol& sym = m_symbols[i];
        if (sym.kind == ConsoleSymbolKind::Empty || sym.kind == ConsoleSymbolKind::Command) continue;
        char line[96]{};
        char* end = line + sizeof(line);
        const usize nLen = std::min<usize>(sym.nameLen, 48U);
        std::memcpy(line, sym.namePtr, nLen);
        char* p = line + nLen;
        std::memcpy(p, " = ", 3U);
        p += 3;
        if (sym.kind == ConsoleSymbolKind::CVarF32) {
            p = DebugBatcher::FormatF32Fixed2(p, end, *static_cast<const f32*>(sym.targetPtr));
        } else if (sym.kind == ConsoleSymbolKind::CVarI32) {
            p = DebugBatcher::FormatI64(p, end, *static_cast<const i32*>(sym.targetPtr));
        } else if (sym.kind == ConsoleSymbolKind::CVarU32) {
            p = DebugBatcher::FormatI64(p, end, static_cast<i64>(*static_cast<const u32*>(sym.targetPtr)));
        } else if (sym.kind == ConsoleSymbolKind::CVarBool) {
            const bool v = *static_cast<const bool*>(sym.targetPtr);
            std::memcpy(p, v ? "true" : "false", v ? 4U : 5U);
            p += v ? 4 : 5;
        }
        Print(LogSeverity::Info, {line, static_cast<usize>(p - line)}, tickIndex);
    }
}

void DebugConsole::EnqueueCommandLine(std::string_view line) noexcept {
    const std::string_view trimmed = TrimAscii(line);
    if (trimmed.empty()) return;

    PackLineBuffer(m_historyLines[m_historyWriteIdx & 15U], trimmed);
    m_historyWriteIdx = static_cast<u8>((m_historyWriteIdx + 1U) & 15U);
    if (m_historyCount < 16U) ++m_historyCount;
    m_historyNavOffset = -1;

    if (m_pendingCount < m_pendingLines.size()) {
        PackLineBuffer(m_pendingLines[m_pendingCount++], trimmed);
    }
}

void DebugConsole::FlushPendingCommands(u32 currentTickIndex) noexcept {
    for (u8 i = 0U; i < m_pendingCount; ++i) {
        ExecuteImmediate(UnpackLineBuffer(m_pendingLines[i]), currentTickIndex);
    }
    m_pendingCount = 0U;
}

void DebugConsole::ExecuteImmediate(std::string_view line, u32 currentTickIndex) noexcept {
    const std::string_view trimmed = TrimAscii(line);
    if (trimmed.empty()) return;
    Print(LogSeverity::Command, trimmed, currentTickIndex);
    ++m_executedCmdCount;

    const usize spacePos = trimmed.find(' ');
    const std::string_view head = (spacePos == std::string_view::npos) ? trimmed : trimmed.substr(0U, spacePos);
    const std::string_view args = (spacePos == std::string_view::npos)
                                      ? std::string_view{}
                                      : TrimAscii(trimmed.substr(spacePos + 1U));

    ConsoleSymbol* sym = FindOrInsertSlot(head, false);
    if (!sym) {
        Print(LogSeverity::Error, "Unknown command or CVar", currentTickIndex);
        return;
    }

    switch (sym->kind) {
        case ConsoleSymbolKind::Empty:
            break;
        case ConsoleSymbolKind::Command:
            if (sym->spec.cmdFn) {
                sym->spec.cmdFn(args, sym->targetPtr, *this);
            }
            break;
        case ConsoleSymbolKind::CVarF32: {
            auto* ptr = static_cast<f32*>(sym->targetPtr);
            if (!args.empty()) {
                char numBuf[32]{};
                const usize len = std::min<usize>(args.size(), 31U);
                std::memcpy(numBuf, args.data(), len);
                char* endPtr = nullptr;
                const f32 val = std::strtof(numBuf, &endPtr);
                if (endPtr != numBuf) {
                    *ptr = std::clamp(val, sym->spec.f32Bounds.minVal, sym->spec.f32Bounds.maxVal);
                }
            }
            char out[64]{};
            char* p = DebugBatcher::FormatF32Fixed2(out, out + sizeof(out), *ptr);
            Print(LogSeverity::Info, {out, static_cast<usize>(p - out)}, currentTickIndex);
            break;
        }
        case ConsoleSymbolKind::CVarI32: {
            auto* ptr = static_cast<i32*>(sym->targetPtr);
            if (!args.empty()) {
                i32 val = 0;
                const auto r = std::from_chars(args.data(), args.data() + args.size(), val);
                if (r.ec == std::errc{}) {
                    *ptr = std::clamp(val, sym->spec.i32Bounds.minVal, sym->spec.i32Bounds.maxVal);
                }
            }
            char out[64]{};
            char* p = DebugBatcher::FormatI64(out, out + sizeof(out), *ptr);
            Print(LogSeverity::Info, {out, static_cast<usize>(p - out)}, currentTickIndex);
            break;
        }
        case ConsoleSymbolKind::CVarU32: {
            auto* ptr = static_cast<u32*>(sym->targetPtr);
            if (!args.empty()) {
                u32 val = 0U;
                const auto r = std::from_chars(args.data(), args.data() + args.size(), val);
                if (r.ec == std::errc{}) {
                    *ptr = std::clamp(val, sym->spec.u32Bounds.minVal, sym->spec.u32Bounds.maxVal);
                }
            }
            char out[64]{};
            char* p = DebugBatcher::FormatI64(out, out + sizeof(out), static_cast<i64>(*ptr));
            Print(LogSeverity::Info, {out, static_cast<usize>(p - out)}, currentTickIndex);
            break;
        }
        case ConsoleSymbolKind::CVarBool: {
            auto* ptr = static_cast<bool*>(sym->targetPtr);
            if (!args.empty()) {
                if (args == "1" || args == "true" || args == "on") *ptr = true;
                else if (args == "0" || args == "false" || args == "off") *ptr = false;
                else if (args == "toggle") *ptr = !(*ptr);
            }
            Print(LogSeverity::Info, *ptr ? "true" : "false", currentTickIndex);
            break;
        }
    }
}

bool DebugConsole::AutocompleteInput() noexcept {
    if (!m_symbols || m_inputLen == 0U) return false;
    const std::string_view prefix{m_inputLine.data(), m_inputLen};
    for (u32 i = 0U; i < kMaxConsoleSymbols; ++i) {
        const u32 idx = static_cast<u32>((m_autocompleteCursor + i) & (kMaxConsoleSymbols - 1U));
        const ConsoleSymbol& sym = m_symbols[idx];
        if (sym.kind == ConsoleSymbolKind::Empty) continue;
        const std::string_view name = sym.Name();
        if (name.size() >= prefix.size() && name.substr(0U, prefix.size()) == prefix) {
            const u8 copyLen = static_cast<u8>(std::min<usize>(name.size(), 125U));
            std::memcpy(m_inputLine.data(), name.data(), copyLen);
            m_inputLine[copyLen] = ' ';
            m_inputLen = static_cast<u8>(copyLen + 1U);
            m_inputLine[m_inputLen] = '\0';
            m_autocompleteCursor = idx + 1U;
            return true;
        }
    }
    return false;
}

void DebugConsole::NavigateHistory(i32 direction) noexcept {
    if (m_historyCount == 0U) return;
    if (m_historyNavOffset < 0) {
        if (direction < 0) return;
        m_historyNavOffset = 0;
    } else {
        const i32 next = static_cast<i32>(m_historyNavOffset) + direction;
        if (next < 0) {
            m_historyNavOffset = -1;
            m_inputLen = 0U;
            m_inputLine[0] = '\0';
            return;
        }
        m_historyNavOffset = static_cast<i8>(std::min<i32>(next, static_cast<i32>(m_historyCount) - 1));
    }
    const u8 slot = static_cast<u8>((m_historyWriteIdx + 16U - 1U - static_cast<u8>(m_historyNavOffset)) & 15U);
    const std::string_view hist = UnpackLineBuffer(m_historyLines[slot]);
    m_inputLen = static_cast<u8>(std::min<usize>(hist.size(), 126U));
    std::memcpy(m_inputLine.data(), hist.data(), m_inputLen);
    m_inputLine[m_inputLen] = '\0';
}

void DebugConsole::AppendInputText(std::string_view utf8) noexcept {
    for (char c : utf8) {
        if (c >= 32 && c < 127 && m_inputLen < 126U) {
            m_inputLine[m_inputLen++] = c;
            m_inputLine[m_inputLen] = '\0';
        }
    }
}

void DebugConsole::BackspaceInput() noexcept {
    if (m_inputLen > 0U) {
        m_inputLine[--m_inputLen] = '\0';
    }
}

void DebugConsole::SubmitInputLine() noexcept {
    if (m_inputLen == 0U) return;
    EnqueueCommandLine({m_inputLine.data(), m_inputLen});
    m_inputLen = 0U;
    m_inputLine[0] = '\0';
}

void DebugConsole::EmitConsoleOverlay(ViewportExtent viewportSize, DebugBatcher& hudBatcher) const noexcept {
    if (m_isOpen == 0U || !m_logMeta || !m_logText) return;
    const f32 w = static_cast<f32>(viewportSize.w) - 24.0f;
    const f32 h = 210.0f;
    hudBatcher.PushSolidRect(Rect{12.0f, 12.0f, w, h}, Color{0.03f, 0.04f, 0.07f, 0.92f});
    hudBatcher.PushBoxOutline(Rect{12.0f, 12.0f, w, h}, 1.0f, Color{0.4f, 0.8f, 1.0f, 0.95f});

    const u16 maxLines = std::min<u16>(m_logCount, 10U);
    for (u16 i = 0U; i < maxLines; ++i) {
        const u16 slot = static_cast<u16>((m_logWriteIdx + kMaxConsoleLogs - 1U - i) & (kMaxConsoleLogs - 1U));
        const LogEntryMeta& meta = m_logMeta[slot];
        const std::string_view line{m_logText + meta.textOffset, meta.textLength};
        Color col{0.85f, 0.9f, 0.95f, 1.0f};
        if (meta.severity == LogSeverity::Warning) col = Color{1.0f, 0.85f, 0.2f, 1.0f};
        else if (meta.severity == LogSeverity::Error) col = Color{1.0f, 0.3f, 0.3f, 1.0f};
        else if (meta.severity == LogSeverity::Command) col = Color{0.4f, 0.95f, 1.0f, 1.0f};

        const f32 y = 172.0f - static_cast<f32>(i) * 15.0f;
        hudBatcher.PushText(20.0f, y, line, col, 1.0f);
    }

    hudBatcher.PushText(20.0f, 196.0f, "> ", Color{0.3f, 1.0f, 0.6f, 1.0f}, 1.1f);
    if (m_inputLen > 0U) {
        hudBatcher.PushText(36.0f, 196.0f, {m_inputLine.data(), m_inputLen}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.1f);
    }
}

u64 DebugConsole::ComputeAuditHash() const noexcept {
    u64 h = m_symbolHash;
    h ^= m_executedCmdCount;
    h *= 1099511628211ULL;
    h ^= static_cast<u64>(m_logCount);
    h *= 1099511628211ULL;
    return h;
}

} // namespace en

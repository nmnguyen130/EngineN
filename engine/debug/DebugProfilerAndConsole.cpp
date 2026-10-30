#include "debug/DebugProfilerAndConsole.h"

#include <charconv>
#include <cstring>

namespace en {
namespace {

constexpr u32 HashName(std::string_view s) noexcept {
    u32 h = 2166136261U;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619U;
    }
    return h ? h : 1U;
}

constexpr std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

} // namespace

void DebugProfiler::BeginFrame() noexcept {
    if (!m_samples) return;
    const u16 slot = static_cast<u16>(m_idx & (kProfileFrames - 1U));
    if (m_valid >= kProfileFrames) {
        const u16 old = m_samples[slot].us[static_cast<usize>(ProfileScope::Frame)];
        const u8 b = ToBucket(old);
        if (m_buckets[b] > 0U && --m_buckets[b] == 0U) m_bucketMask &= ~(1ULL << b);
    }
    m_samples[slot] = FrameSample{};
}

void DebugProfiler::Record(ProfileScope scope, u32 us) noexcept {
    if (!m_samples || scope >= ProfileScope::Count) return;
    const u16 slot = static_cast<u16>(m_idx & (kProfileFrames - 1U));
    m_samples[slot].us[static_cast<usize>(scope)] =
        static_cast<u16>(std::min<u32>(65535U, us));
}

void DebugProfiler::RecordTelemetry(const Telemetry& t) noexcept {
    if (!m_samples) return;
    FrameSample& s = m_samples[m_idx & (kProfileFrames - 1U)];
    s.entities = t.entities;
    s.quads = t.quads;
    s.batches = t.batches;
    s.contacts = t.contacts;
    s.voices = t.voices;
    if (t.scales) {
        s.worldQ6 = static_cast<u8>(std::clamp(t.scales[0] * 64.0f + 0.5f, 0.0f, 255.0f));
        s.playerQ6 = static_cast<u8>(std::clamp(t.scales[1] * 64.0f + 0.5f, 0.0f, 255.0f));
    }
}

void DebugProfiler::EndFrame() noexcept {
    if (!m_samples) return;
    FrameSample& s = m_samples[m_idx & (kProfileFrames - 1U)];
    u32 total = s.us[static_cast<usize>(ProfileScope::Frame)];
    if (total == 0U) {
        for (usize i = 1; i < static_cast<usize>(ProfileScope::Count); ++i) total += s.us[i];
        s.us[static_cast<usize>(ProfileScope::Frame)] =
            static_cast<u16>(std::min<u32>(65535U, total));
    }
    const u8 b = ToBucket(s.us[static_cast<usize>(ProfileScope::Frame)]);
    ++m_buckets[b];
    m_bucketMask |= (1ULL << b);
    m_idx = static_cast<u16>((m_idx + 1U) & (kProfileFrames - 1U));
    if (m_valid < kProfileFrames) ++m_valid;
}

u32 DebugProfiler::PercentileUs(u32 rank256) const noexcept {
    if (m_valid == 0U) return 0U;
    const u32 target = std::max<u32>(1U, (rank256 * m_valid) / kProfileFrames);
    u32 acc = 0U;
    u64 mask = m_bucketMask;
    while (mask) {
        const u32 b = static_cast<u32>(std::countr_zero(mask));
        mask &= mask - 1ULL;
        acc += m_buckets[b];
        if (acc >= target) return b * 500U + 250U;
    }
    return (kProfileBuckets - 1U) * 500U + 250U;
}

const FrameSample& DebugProfiler::Latest() const noexcept {
    static const FrameSample kEmpty{};
    if (!m_samples || m_valid == 0U) return kEmpty;
    const u16 slot = static_cast<u16>((m_idx + kProfileFrames - 1U) & (kProfileFrames - 1U));
    return m_samples[slot];
}

void DebugProfiler::EmitHud(Overlay flags, Viewport vp, DebugDraw& hud) const noexcept {
    if (!HasFlag(flags, Overlay::Profiler) || !m_samples || m_valid == 0U) return;

    const FrameSample& L = Latest();
    const f32 x = static_cast<f32>(vp.w) - 220.0f;
    hud.Box({x, 8.0f, 208.0f, 100.0f}, 1.0f, {0.2f, 0.8f, 0.5f, 0.85f});

    char buf[64]{};
    char* p = buf;
    char* end = buf + sizeof(buf);
    p = DebugDraw::Append(p, end, "frame ");
    p = DebugDraw::FmtF2(p, end, static_cast<f32>(L.us[0]) * 0.001f);
    p = DebugDraw::Append(p, end, " ms");
    hud.Text(x + 8.0f, 16.0f, {buf, static_cast<usize>(p - buf)}, {0.3f, 1.0f, 0.6f, 1.0f}, 1.0f);

    p = buf;
    p = DebugDraw::Append(p, end, "e ");
    p = DebugDraw::FmtI64(p, end, L.entities);
    p = DebugDraw::Append(p, end, " q ");
    p = DebugDraw::FmtI64(p, end, L.quads);
    p = DebugDraw::Append(p, end, " c ");
    p = DebugDraw::FmtI64(p, end, L.contacts);
    hud.Text(x + 8.0f, 34.0f, {buf, static_cast<usize>(p - buf)}, {0.85f, 0.95f, 1.0f, 1.0f}, 1.0f);

    const u32 p99 = PercentileUs(250U);
    p = buf;
    p = DebugDraw::Append(p, end, "p99 ");
    p = DebugDraw::FmtF2(p, end, static_cast<f32>(p99) * 0.001f);
    p = DebugDraw::Append(p, end, " ms");
    hud.Text(x + 8.0f, 52.0f, {buf, static_cast<usize>(p - buf)}, {0.9f, 0.85f, 0.4f, 1.0f}, 1.0f);

    // Sparkline of last frames.
    const u32 n = std::min<u32>(m_valid, 64U);
    for (u32 i = 0U; i < n; ++i) {
        const u16 idx = static_cast<u16>((m_idx + kProfileFrames - n + i) & (kProfileFrames - 1U));
        const f32 h = std::clamp(static_cast<f32>(m_samples[idx].us[0]) * 0.0025f, 1.0f, 40.0f);
        hud.Rect({x + 8.0f + static_cast<f32>(i) * 3.0f, 96.0f - h, 2.0f, h},
                 {0.3f, 0.9f, 0.5f, 0.8f});
    }
}

// ---- Console ----

void DebugConsole::Init(
    std::span<ConsoleSymbol, kMaxSymbols> symbols,
    std::span<LogMeta, kMaxLogs> logs,
    std::span<char, kLogTextBytes> text) noexcept {
    m_symbols = symbols.data();
    m_logs = logs.data();
    m_text = text.data();
    Clear();
}

void DebugConsole::Clear() noexcept {
    if (m_symbols) {
        for (u32 i = 0U; i < kMaxSymbols; ++i) m_symbols[i] = ConsoleSymbol{};
    }
    ClearLogs();
    m_symbolCount = 0U;
    m_input = {};
    m_histWrite = 0U;
    m_histCount = 0U;
    m_histNav = -1;
    m_pending = 0U;
    m_open = 0U;
    m_autoCursor = 0U;
}

ConsoleSymbol* DebugConsole::FindSlot(std::string_view name, bool insert) noexcept {
    if (!m_symbols || name.empty() || name.size() > 31U) return nullptr;
    const u32 h = HashName(name);
    for (u32 i = 0U; i < kMaxSymbols; ++i) {
        const u32 idx = (h + i) & (kMaxSymbols - 1U);
        ConsoleSymbol& s = m_symbols[idx];
        if (s.kind == ConsoleSymbol::Kind::Empty) return insert ? &s : nullptr;
        if (s.hash == h && s.Name() == name) return &s;
    }
    return nullptr;
}

Result<ConsoleSymbol*, DebugErrc> DebugConsole::Alloc(
    std::string_view name, ConsoleSymbol::Kind kind, void* target) noexcept {
    if (auto* existing = FindSlot(name, false)) {
        if (existing->kind != ConsoleSymbol::Kind::Empty) return Err(DebugErrc::DuplicateSymbol);
    }
    ConsoleSymbol* slot = FindSlot(name, true);
    if (!slot) return Err(DebugErrc::SymbolFull);
    *slot = ConsoleSymbol{};
    slot->kind = kind;
    slot->target = target;
    slot->hash = HashName(name);
    slot->nameLen = static_cast<u8>(name.size());
    // Store name inline in a static fashion is not available; point at stable string if provided.
    // For registered names we require the caller to pass a string with static lifetime.
    slot->name = name.data();
    ++m_symbolCount;
    return slot;
}

Result<void, DebugErrc> DebugConsole::RegisterCmd(
    std::string_view name,
    void (*fn)(std::string_view, void*, DebugConsole&) noexcept,
    void* ctx) noexcept {
    auto r = Alloc(name, ConsoleSymbol::Kind::Cmd, ctx);
    if (!r) return Err(r.error());
    r.value()->spec.fn = fn;
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterF32(
    std::string_view name, f32* target, f32 minV, f32 maxV) noexcept {
    auto r = Alloc(name, ConsoleSymbol::Kind::CVarF32, target);
    if (!r) return Err(r.error());
    r.value()->spec.f32b = {minV, maxV};
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterI32(
    std::string_view name, i32* target, i32 minV, i32 maxV) noexcept {
    auto r = Alloc(name, ConsoleSymbol::Kind::CVarI32, target);
    if (!r) return Err(r.error());
    r.value()->spec.i32b = {minV, maxV};
    return {};
}

Result<void, DebugErrc> DebugConsole::RegisterBool(std::string_view name, bool* target) noexcept {
    auto r = Alloc(name, ConsoleSymbol::Kind::CVarBool, target);
    if (!r) return Err(r.error());
    return {};
}

void DebugConsole::Print(LogLevel level, std::string_view msg) noexcept {
    if (!m_logs || !m_text || msg.empty()) return;
    const u16 len = static_cast<u16>(std::min<usize>(msg.size(), 255U));
    if (m_textCursor + len + 1U > kLogTextBytes) m_textCursor = 0U;
    std::memcpy(m_text + m_textCursor, msg.data(), len);
    m_text[m_textCursor + len] = '\0';

    LogMeta& meta = m_logs[m_logWrite];
    meta = LogMeta{};
    meta.offset = m_textCursor;
    meta.length = static_cast<u8>(len);
    meta.level = level;
    m_textCursor = static_cast<u16>(m_textCursor + len + 1U);
    m_logWrite = static_cast<u16>((m_logWrite + 1U) & (kMaxLogs - 1U));
    if (m_logCount < kMaxLogs) ++m_logCount;
}

void DebugConsole::ListCVars() noexcept {
    if (!m_symbols) return;
    for (u32 i = 0U; i < kMaxSymbols; ++i) {
        const ConsoleSymbol& s = m_symbols[i];
        if (s.kind == ConsoleSymbol::Kind::Empty || s.kind == ConsoleSymbol::Kind::Cmd) continue;
        Print(LogLevel::Info, s.Name());
    }
}

void DebugConsole::Enqueue(std::string_view line) noexcept {
    if (m_pending >= m_queue.size()) return;
    m_queue[m_pending++] = FixedString<96>{Trim(line)};
}

void DebugConsole::Flush() noexcept {
    for (u8 i = 0U; i < m_pending; ++i) Exec(m_queue[i].View());
    m_pending = 0U;
}

void DebugConsole::Exec(std::string_view line) noexcept {
    line = Trim(line);
    if (line.empty() || !m_symbols) return;

    const usize sp = line.find(' ');
    const std::string_view cmd = (sp == std::string_view::npos) ? line : line.substr(0, sp);
    const std::string_view args = (sp == std::string_view::npos) ? std::string_view{} : Trim(line.substr(sp + 1));

    ConsoleSymbol* s = FindSlot(cmd, false);
    if (!s || s->kind == ConsoleSymbol::Kind::Empty) {
        Print(LogLevel::Warn, "unknown");
        return;
    }

    switch (s->kind) {
    case ConsoleSymbol::Kind::Cmd:
        if (s->spec.fn) s->spec.fn(args, s->target, *this);
        break;
    case ConsoleSymbol::Kind::CVarF32: {
        auto* v = static_cast<f32*>(s->target);
        if (!v) break;
        if (args.empty()) {
            char buf[48]{};
            char* p = DebugDraw::Append(buf, buf + 48, s->Name());
            p = DebugDraw::Append(p, buf + 48, " = ");
            p = DebugDraw::FmtF2(p, buf + 48, *v);
            Print(LogLevel::Info, {buf, static_cast<usize>(p - buf)});
        } else {
            f32 parsed = *v;
            if (std::from_chars(args.data(), args.data() + args.size(), parsed).ec == std::errc{}) {
                *v = std::clamp(parsed, s->spec.f32b.minV, s->spec.f32b.maxV);
            }
        }
        break;
    }
    case ConsoleSymbol::Kind::CVarI32: {
        auto* v = static_cast<i32*>(s->target);
        if (!v) break;
        if (args.empty()) {
            char buf[48]{};
            char* p = DebugDraw::Append(buf, buf + 48, s->Name());
            p = DebugDraw::Append(p, buf + 48, " = ");
            p = DebugDraw::FmtI64(p, buf + 48, *v);
            Print(LogLevel::Info, {buf, static_cast<usize>(p - buf)});
        } else {
            i32 parsed = *v;
            if (std::from_chars(args.data(), args.data() + args.size(), parsed).ec == std::errc{}) {
                *v = std::clamp(parsed, s->spec.i32b.minV, s->spec.i32b.maxV);
            }
        }
        break;
    }
    case ConsoleSymbol::Kind::CVarBool: {
        auto* v = static_cast<bool*>(s->target);
        if (!v) break;
        if (args.empty()) *v = !*v;
        else if (args == "1" || args == "true" || args == "on") *v = true;
        else if (args == "0" || args == "false" || args == "off") *v = false;
        break;
    }
    default: break;
    }
}

bool DebugConsole::Autocomplete() noexcept {
    if (!m_symbols || m_input.Empty()) return false;
    const std::string_view prefix = m_input.View();
    u8 found = 0U;
    std::string_view match{};
    for (u32 i = 0U; i < kMaxSymbols; ++i) {
        const ConsoleSymbol& s = m_symbols[i];
        if (s.kind == ConsoleSymbol::Kind::Empty) continue;
        if (s.Name().size() >= prefix.size() && s.Name().substr(0, prefix.size()) == prefix) {
            if (found == m_autoCursor) match = s.Name();
            ++found;
        }
    }
    if (found == 0U) return false;
    if (!match.empty()) {
        m_input = FixedString<96>{match};
        m_autoCursor = static_cast<u8>((m_autoCursor + 1U) % found);
        return true;
    }
    return false;
}

void DebugConsole::History(i32 dir) noexcept {
    if (m_histCount == 0U) return;
    if (dir > 0) {
        if (m_histNav < 0) m_histNav = static_cast<i8>(m_histCount - 1);
        else if (m_histNav > 0) --m_histNav;
    } else {
        if (m_histNav < 0) return;
        if (m_histNav + 1 < m_histCount) ++m_histNav;
        else { m_histNav = -1; m_input = {}; return; }
    }
    const u8 idx = static_cast<u8>((m_histWrite + m_history.size() - m_histCount + static_cast<u8>(m_histNav))
                                   % m_history.size());
    m_input = m_history[idx];
}

void DebugConsole::Append(std::string_view utf8) noexcept {
    if (utf8.empty()) return;
    const auto cur = m_input.View();
    char buf[96]{};
    const usize n = std::min(cur.size(), sizeof(buf) - 1U);
    std::memcpy(buf, cur.data(), n);
    const usize room = sizeof(buf) - 1U - n;
    const usize add = std::min(room, utf8.size());
    std::memcpy(buf + n, utf8.data(), add);
    m_input = FixedString<96>{std::string_view{buf, n + add}};
    m_autoCursor = 0U;
}

void DebugConsole::Backspace() noexcept {
    const auto cur = m_input.View();
    if (cur.empty()) return;
    m_input = FixedString<96>{cur.substr(0, cur.size() - 1)};
    m_autoCursor = 0U;
}

void DebugConsole::Submit() noexcept {
    const auto line = Trim(m_input.View());
    if (line.empty()) return;
    Print(LogLevel::Cmd, line);
    m_history[m_histWrite] = FixedString<96>{line};
    m_histWrite = static_cast<u8>((m_histWrite + 1U) % m_history.size());
    if (m_histCount < m_history.size()) ++m_histCount;
    m_histNav = -1;
    Enqueue(line);
    m_input = {};
    m_autoCursor = 0U;
}

void DebugConsole::Emit(Viewport vp, DebugDraw& hud) const noexcept {
    if (!m_open) return;
    const f32 h = 180.0f;
    const f32 y0 = static_cast<f32>(vp.h) - h - 8.0f;
    hud.Rect({8.0f, y0, static_cast<f32>(vp.w) - 16.0f, h}, {0.05f, 0.08f, 0.1f, 0.85f});
    hud.Box({8.0f, y0, static_cast<f32>(vp.w) - 16.0f, h}, 1.0f, {0.3f, 0.7f, 0.5f, 0.9f});

    // Recent logs (bottom-up).
    f32 ly = y0 + h - 36.0f;
    const u16 n = std::min<u16>(m_logCount, 8U);
    for (u16 i = 0U; i < n; ++i) {
        const u16 idx = static_cast<u16>((m_logWrite + kMaxLogs - 1U - i) & (kMaxLogs - 1U));
        const LogMeta& m = m_logs[idx];
        Color col{0.8f, 0.85f, 0.9f, 1.0f};
        if (m.level == LogLevel::Warn) col = {1.0f, 0.8f, 0.3f, 1.0f};
        else if (m.level == LogLevel::Error) col = {1.0f, 0.35f, 0.3f, 1.0f};
        else if (m.level == LogLevel::Cmd) col = {0.4f, 1.0f, 0.6f, 1.0f};
        hud.Text(16.0f, ly, {m_text + m.offset, m.length}, col, 0.95f);
        ly -= 16.0f;
    }

    char prompt[128]{};
    char* p = DebugDraw::Append(prompt, prompt + 128, "> ");
    p = DebugDraw::Append(p, prompt + 128, m_input.View());
    hud.Text(16.0f, y0 + h - 20.0f, {prompt, static_cast<usize>(p - prompt)},
             {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
}

} // namespace en

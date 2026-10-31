#include "debug/DebugTools.h"

#include <charconv>

namespace en {
namespace {

[[nodiscard]] constexpr u32 HashSymbolName(std::string_view s) noexcept {
    u32 h = 2166136261U;
    for (const char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619U;
    }
    return h != 0U ? h : 1U;
}

[[nodiscard]] constexpr std::string_view TrimWhitespace(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

} // namespace

void DebugConsole::Init(
    std::span<ConsoleSymbol, kMaxSymbols> symbols,
    std::span<ConsoleLogMeta, kMaxConsoleLogs> logs,
    std::span<char, kConsoleTextBytes> text) noexcept {
    m_symbols = symbols.data();
    m_logs    = logs.data();
    m_text    = text.data();
    Clear();
}

void DebugConsole::Clear() noexcept {
    if (m_symbols) {
        for (u32 i = 0U; i < kMaxSymbols; ++i) m_symbols[i] = ConsoleSymbol{};
    }
    ClearLogs();
    m_symbolCount = 0U;
    m_input       = {};
    m_histWrite   = 0U;
    m_histCount   = 0U;
    m_histNav     = -1;
    m_pending     = 0U;
    m_open        = 0U;
    m_autoCursor  = 0U;
}

ConsoleSymbol* DebugConsole::FindSlot(std::string_view name, bool insert) noexcept {
    if (!m_symbols || name.empty() || name.size() > 31U) return nullptr;
    const u32 h = HashSymbolName(name);
    for (u32 i = 0U; i < kMaxSymbols; ++i) {
        const u32      idx = (h + i) & (kMaxSymbols - 1U);
        ConsoleSymbol& s   = m_symbols[idx];
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
    *slot         = ConsoleSymbol{};
    slot->kind    = kind;
    slot->target  = target;
    slot->hash    = HashSymbolName(name);
    slot->nameLen = static_cast<u8>(name.size());
    slot->name    = name.data();
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

Result<void, DebugErrc> DebugConsole::RegisterBool(
    std::string_view name, bool* target) noexcept {
    auto r = Alloc(name, ConsoleSymbol::Kind::CVarBool, target);
    if (!r) return Err(r.error());
    return {};
}

void DebugConsole::Print(LogLevel level, std::string_view msg) noexcept {
    if (!m_logs || !m_text || msg.empty()) return;
    const u16 len = static_cast<u16>(std::min<usize>(msg.size(), 255U));
    if (m_textCursor + len + 1U > kConsoleTextBytes) m_textCursor = 0U;
    std::memcpy(m_text + m_textCursor, msg.data(), len);
    m_text[m_textCursor + len] = '\0';

    ConsoleLogMeta& meta = m_logs[m_logWrite];
    meta         = ConsoleLogMeta{.offset = m_textCursor, .length = static_cast<u8>(len), .level = level};
    m_textCursor = static_cast<u16>(m_textCursor + len + 1U);
    m_logWrite   = static_cast<u16>((m_logWrite + 1U) & (kMaxConsoleLogs - 1U));
    if (m_logCount < kMaxConsoleLogs) ++m_logCount;
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
    m_queue[m_pending++] = FixedString<96>{TrimWhitespace(line)};
}

void DebugConsole::Flush() noexcept {
    for (u8 i = 0U; i < m_pending; ++i) Exec(m_queue[i].View());
    m_pending = 0U;
}

void DebugConsole::Exec(std::string_view line) noexcept {
    line = TrimWhitespace(line);
    if (line.empty() || !m_symbols) return;

    const usize            sp   = line.find(' ');
    const std::string_view cmd  = (sp == std::string_view::npos) ? line : line.substr(0, sp);
    const std::string_view args = (sp == std::string_view::npos) ? std::string_view{} : TrimWhitespace(line.substr(sp + 1));

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
                char  buf[48]{};
                char* p = DebugDraw::Append(buf, buf + 48, s->Name());
                p       = DebugDraw::Append(p, buf + 48, " = ");
                p       = DebugDraw::FmtF2(p, buf + 48, *v);
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
                char  buf[48]{};
                char* p = DebugDraw::Append(buf, buf + 48, s->Name());
                p       = DebugDraw::Append(p, buf + 48, " = ");
                p       = DebugDraw::FmtI64(p, buf + 48, *v);
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
        default:
            break;
    }
}

bool DebugConsole::Autocomplete() noexcept {
    if (!m_symbols || m_input.Empty()) return false;
    const std::string_view prefix = m_input.View();
    u8                     found  = 0U;
    std::string_view       match{};
    for (u32 i = 0U; i < kMaxSymbols; ++i) {
        const ConsoleSymbol& s = m_symbols[i];
        if (s.kind == ConsoleSymbol::Kind::Empty) continue;
        if (s.Name().starts_with(prefix)) {
            if (found == m_autoCursor) match = s.Name();
            ++found;
        }
    }
    if (found == 0U || match.empty()) return false;
    m_input      = FixedString<96>{match};
    m_autoCursor = static_cast<u8>((m_autoCursor + 1U) % found);
    return true;
}

void DebugConsole::History(i32 dir) noexcept {
    if (m_histCount == 0U) return;
    if (dir > 0) {
        if (m_histNav < 0) m_histNav = static_cast<i8>(m_histCount - 1);
        else if (m_histNav > 0) --m_histNav;
    } else {
        if (m_histNav < 0) return;
        if (m_histNav + 1 < m_histCount) ++m_histNav;
        else {
            m_histNav = -1;
            m_input   = {};
            return;
        }
    }
    const u8 idx = static_cast<u8>(
        (m_histWrite + m_history.size() - m_histCount + static_cast<u8>(m_histNav)) % m_history.size());
    m_input = m_history[idx];
}

void DebugConsole::Append(std::string_view utf8) noexcept {
    if (utf8.empty()) return;
    (void)m_input.Append(utf8);
    m_autoCursor = 0U;
}

void DebugConsole::Backspace() noexcept {
    const auto cur = m_input.View();
    if (cur.empty()) return;
    m_input      = FixedString<96>{cur.substr(0U, cur.size() - 1U)};
    m_autoCursor = 0U;
}

void DebugConsole::Submit() noexcept {
    const auto line = TrimWhitespace(m_input.View());
    if (line.empty()) return;
    Print(LogLevel::Debug, line);
    m_history[m_histWrite] = FixedString<96>{line};
    m_histWrite            = static_cast<u8>((m_histWrite + 1U) % m_history.size());
    if (m_histCount < m_history.size()) ++m_histCount;
    m_histNav    = -1;
    Enqueue(line);
    m_input      = {};
    m_autoCursor = 0U;
}

void DebugConsole::Emit(Viewport vp, DebugDraw& hud) const noexcept {
    if (!m_open) return;
    const f32 h  = 180.0f;
    const f32 y0 = static_cast<f32>(vp.h) - h - 8.0f;
    hud.Rect({8.0f, y0, static_cast<f32>(vp.w) - 16.0f, h}, {0.05f, 0.08f, 0.10f, 0.85f});
    hud.Box({8.0f, y0, static_cast<f32>(vp.w) - 16.0f, h}, 1.0f, {0.3f, 0.7f, 0.5f, 0.9f});

    f32       ly = y0 + h - 36.0f;
    const u16 n  = std::min<u16>(m_logCount, 8U);
    for (u16 i = 0U; i < n; ++i) {
        const u16             idx = static_cast<u16>((m_logWrite + kMaxConsoleLogs - 1U - i) & (kMaxConsoleLogs - 1U));
        const ConsoleLogMeta& m   = m_logs[idx];
        Color col{0.8f, 0.85f, 0.9f, 1.0f};
        if (m.level == LogLevel::Warn) col = {1.0f, 0.8f, 0.3f, 1.0f};
        else if (m.level == LogLevel::Error) col = {1.0f, 0.35f, 0.3f, 1.0f};
        else if (m.level == LogLevel::Debug) col = {0.4f, 1.0f, 0.6f, 1.0f};
        hud.Text(16.0f, ly, {m_text + m.offset, m.length}, col, 0.95f);
        ly -= 16.0f;
    }

    char  prompt[128]{};
    char* p = DebugDraw::Append(prompt, prompt + 128, "> ");
    p       = DebugDraw::Append(p, prompt + 128, m_input.View());
    hud.Text(16.0f, y0 + h - 20.0f, {prompt, static_cast<usize>(p - prompt)},
             {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
}

} // namespace en

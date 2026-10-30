#pragma once

#include "debug/DebugBatcher.h"

namespace en {

struct alignas(32) ConsoleSymbol {
    enum class Kind : u8 { Empty = 0, Cmd, CVarF32, CVarI32, CVarBool };

    const char* name{nullptr};
    void*       target{nullptr};
    union {
        void (*fn)(std::string_view, void*, class DebugConsole&) noexcept;
        struct { f32 minV, maxV; } f32b;
        struct { i32 minV, maxV; } i32b;
    } spec{};
    u32  hash{0U};
    Kind kind{Kind::Empty};
    u8   nameLen{0U};

    [[nodiscard]] constexpr std::string_view Name() const noexcept {
        return name ? std::string_view{name, nameLen} : std::string_view{};
    }
};
static_assert(sizeof(ConsoleSymbol) == 32);

struct alignas(4) LogMeta {
    u16 offset{0U};
    u8  length{0U};
    LogLevel level{LogLevel::Info};
};
static_assert(sizeof(LogMeta) == 4);

class alignas(64) DebugProfiler {
public:
    constexpr void Init(std::span<FrameSample, kProfileFrames> storage) noexcept {
        m_samples = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        m_buckets.fill(0U);
        if (m_samples) {
            for (u32 i = 0U; i < kProfileFrames; ++i) m_samples[i] = FrameSample{};
        }
        m_bucketMask = 0ULL;
        m_idx = 0U;
        m_valid = 0U;
    }

    void BeginFrame() noexcept;
    void Record(ProfileScope scope, u32 us) noexcept;
    void RecordTelemetry(const Telemetry& t) noexcept;
    void EndFrame() noexcept;

    [[nodiscard]] u32 PercentileUs(u32 rank256) const noexcept;
    [[nodiscard]] const FrameSample& Latest() const noexcept;
    [[nodiscard]] constexpr u16 ValidFrames() const noexcept { return m_valid; }

    void EmitHud(Overlay flags, Viewport vp, DebugDraw& hud) const noexcept;

private:
    [[nodiscard]] static constexpr u8 ToBucket(u32 us) noexcept {
        const u32 b = us / 500U;
        return static_cast<u8>(b < (kProfileBuckets - 1U) ? b : (kProfileBuckets - 1U));
    }

    std::array<u16, kProfileBuckets> m_buckets{};
    FrameSample* m_samples{nullptr};
    u64 m_bucketMask{0ULL};
    u16 m_idx{0U};
    u16 m_valid{0U};
};
static_assert(sizeof(DebugProfiler) == 128);

template <typename ClockT>
class ScopeTimer {
public:
#if EN_ENABLE_DEBUG_TOOLS
    constexpr ScopeTimer(DebugProfiler& p, ProfileScope s) noexcept
        : m_p(&p), m_start(ClockT::NowNs()), m_scope(s) {}
    ~ScopeTimer() noexcept {
        if (m_p) m_p->Record(m_scope, static_cast<u32>((ClockT::NowNs() - m_start) / 1000ULL));
    }
#else
    constexpr ScopeTimer(DebugProfiler&, ProfileScope) noexcept {}
    ~ScopeTimer() noexcept = default;
#endif
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
private:
#if EN_ENABLE_DEBUG_TOOLS
    DebugProfiler* m_p{nullptr};
    u64 m_start{0ULL};
    ProfileScope m_scope{ProfileScope::Frame};
#endif
};

class alignas(64) DebugConsole {
public:
    void Init(
        std::span<ConsoleSymbol, kMaxSymbols> symbols,
        std::span<LogMeta, kMaxLogs> logs,
        std::span<char, kLogTextBytes> text) noexcept;

    void Clear() noexcept;
    constexpr void ClearLogs() noexcept {
        m_textCursor = 0U;
        m_logWrite = 0U;
        m_logCount = 0U;
    }

    [[nodiscard]] Result<void, DebugErrc> RegisterCmd(
        std::string_view name,
        void (*fn)(std::string_view, void*, DebugConsole&) noexcept,
        void* ctx = nullptr) noexcept;

    [[nodiscard]] Result<void, DebugErrc> RegisterF32(
        std::string_view name, f32* target, f32 minV = -1e9f, f32 maxV = 1e9f) noexcept;
    [[nodiscard]] Result<void, DebugErrc> RegisterI32(
        std::string_view name, i32* target, i32 minV = -2147483647, i32 maxV = 2147483647) noexcept;
    [[nodiscard]] Result<void, DebugErrc> RegisterBool(std::string_view name, bool* target) noexcept;

    void Print(LogLevel level, std::string_view msg) noexcept;
    void ListCVars() noexcept;

    void Enqueue(std::string_view line) noexcept;
    void Flush() noexcept;
    void Exec(std::string_view line) noexcept;

    bool Autocomplete() noexcept;
    void History(i32 dir) noexcept;
    void Append(std::string_view utf8) noexcept;
    void Backspace() noexcept;
    void Submit() noexcept;

    constexpr void SetOpen(bool open) noexcept { m_open = open ? 1U : 0U; }
    constexpr void Toggle() noexcept { m_open ^= 1U; }
    [[nodiscard]] constexpr bool IsOpen() const noexcept { return m_open != 0U; }
    [[nodiscard]] constexpr std::string_view Input() const noexcept { return m_input.View(); }
    [[nodiscard]] constexpr u16 SymbolCount() const noexcept { return m_symbolCount; }
    [[nodiscard]] constexpr u16 LogCount() const noexcept { return m_logCount; }

    void Emit(Viewport vp, DebugDraw& hud) const noexcept;

private:
    [[nodiscard]] ConsoleSymbol* FindSlot(std::string_view name, bool insert) noexcept;
    [[nodiscard]] Result<ConsoleSymbol*, DebugErrc> Alloc(
        std::string_view name, ConsoleSymbol::Kind kind, void* target) noexcept;

    ConsoleSymbol* m_symbols{nullptr};
    LogMeta*       m_logs{nullptr};
    char*          m_text{nullptr};
    u16 m_textCursor{0U};
    u16 m_logWrite{0U};
    u16 m_logCount{0U};
    u16 m_symbolCount{0U};
    u8  m_autoCursor{0U};
    u8  m_histWrite{0U};
    u8  m_histCount{0U};
    i8  m_histNav{-1};
    u8  m_pending{0U};
    u8  m_open{0U};

    FixedString<96> m_input{};
    std::array<FixedString<96>, 8> m_history{};
    std::array<FixedString<96>, 2> m_queue{};
};
static_assert(sizeof(DebugConsole) == 1216);

} // namespace en

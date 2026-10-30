#pragma once

#include "debug/DebugDraw.h"

namespace en {

// 32-byte registered console command or CVar binding slot.
struct ConsoleSymbol {
    enum class Kind : u8 { Empty = 0, Cmd, CVarF32, CVarI32, CVarBool };

    const char* name{nullptr};
    void*       target{nullptr};
    union {
        void (*fn)(std::string_view, void*, class DebugConsole&) noexcept;
        struct { f32 minV, maxV; } f32b;
        struct { i32 minV, maxV; } i32b;
    } spec{};
    u32  hash{0};
    Kind kind{Kind::Empty};
    u8   nameLen{0};
    u16  pad{0};

    [[nodiscard]] constexpr std::string_view Name() const noexcept {
        return name ? std::string_view{name, nameLen} : std::string_view{};
    }
};
static_assert(sizeof(ConsoleSymbol) == 32);

struct ConsoleLogMeta {
    u16      offset{0};
    u8       length{0};
    LogLevel level{LogLevel::Info};
};
static_assert(sizeof(ConsoleLogMeta) == 4);

// Rolling 128-frame microsecond profiler with O(1) histogram percentile calculation.
// Non-owning view over Arena sample storage; emits HUD sparklines and telemetry counters.
// Invariant: sizeof(DebugProfiler) == 88.
class DebugProfiler {
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
        m_idx        = 0U;
        m_valid      = 0U;
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
    FrameSample*                     m_samples{nullptr};
    u64                              m_bucketMask{0};
    u16                              m_idx{0};
    u16                              m_valid{0};
    u32                              m_pad{0};
};
static_assert(sizeof(DebugProfiler) == 88);

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
    u64            m_start{0};
    ProfileScope   m_scope{ProfileScope::Frame};
#endif
};

// Interactive developer console supporting command registration, typed CVars, autocomplete, and history.
// Non-owning view over Arena symbol and log buffers; zero heap allocations.
class DebugConsole {
public:
    void Init(
        std::span<ConsoleSymbol, kMaxSymbols> symbols,
        std::span<ConsoleLogMeta, kMaxConsoleLogs> logs,
        std::span<char, kConsoleTextBytes> text) noexcept;

    void Clear() noexcept;
    constexpr void ClearLogs() noexcept {
        m_textCursor = 0U;
        m_logWrite   = 0U;
        m_logCount   = 0U;
    }

    [[nodiscard]] Result<void, DebugErrc> RegisterCmd(
        std::string_view name,
        void (*fn)(std::string_view, void*, DebugConsole&) noexcept,
        void* ctx = nullptr) noexcept;
    [[nodiscard]] Result<void, DebugErrc> RegisterF32(
        std::string_view name, f32* target, f32 minV = -1e9f, f32 maxV = 1e9f) noexcept;
    [[nodiscard]] Result<void, DebugErrc> RegisterI32(
        std::string_view name, i32* target, i32 minV = -2147483647, i32 maxV = 2147483647) noexcept;
    [[nodiscard]] Result<void, DebugErrc> RegisterBool(
        std::string_view name, bool* target) noexcept;

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

    ConsoleSymbol*                  m_symbols{nullptr};
    ConsoleLogMeta*                 m_logs{nullptr};
    char*                           m_text{nullptr};
    u16                             m_textCursor{0};
    u16                             m_logWrite{0};
    u16                             m_logCount{0};
    u16                             m_symbolCount{0};
    u8                              m_autoCursor{0};
    u8                              m_histWrite{0};
    u8                              m_histCount{0};
    i8                              m_histNav{-1};
    u8                              m_pending{0};
    u8                              m_open{0};
    u16                             m_pad{0};
    FixedString<96>                 m_input{};
    std::array<FixedString<96>, 8>  m_history{};
    std::array<FixedString<96>, 2>  m_queue{};
};
static_assert(sizeof(DebugConsole) == 1096);

// 64-tick entity flight recorder and world/HUD overlay inspector for colliders, velocities, and combat boxes.
// Non-owning view over Arena history buffer; supports world/screen picking and timeline scrubbing.
// Invariant: sizeof(DebugInspector) == 16.
class DebugInspector {
public:
    constexpr void Init(std::span<ActorSample, kFlightTicks> storage) noexcept {
        m_hist = storage.data();
        Clear();
    }

    constexpr void Clear() noexcept {
        if (m_hist) {
            for (u32 i = 0U; i < kFlightTicks; ++i) m_hist[i] = ActorSample{};
        }
        m_pin   = kNullEntity;
        m_write = 0U;
        m_valid = 0U;
        m_scrub = 0U;
    }

    void Record(
        const World& world,
        u32 tick,
        const TickInput* input = nullptr,
        const ActionBuffer* buffer = nullptr) noexcept;

    [[nodiscard]] EntityId Pick(const World& world, Vec2 worldPos, f32 radius = 1.0f) noexcept;
    [[nodiscard]] EntityId PickScreen(
        const World& world, Vec2 screen, Viewport vp, const Camera2D& cam = {}) noexcept;

    constexpr void Pin(EntityId e) noexcept {
        if (m_pin != e) {
            m_pin   = e;
            m_write = 0U;
            m_valid = 0U;
            m_scrub = 0U;
        }
    }

    [[nodiscard]] constexpr EntityId Pinned() const noexcept { return m_pin; }

    constexpr void Scrub(i32 delta) noexcept {
        if (m_valid == 0U) {
            m_scrub = 0U;
            return;
        }
        const i32 maxOff = static_cast<i32>(m_valid) - 1;
        m_scrub = static_cast<u8>(std::clamp(static_cast<i32>(m_scrub) + delta, 0, maxOff));
    }

    constexpr void ResetScrub() noexcept { m_scrub = 0U; }
    [[nodiscard]] constexpr u8 ScrubOffset() const noexcept { return m_scrub; }
    [[nodiscard]] constexpr u8 Recorded() const noexcept { return m_valid; }
    [[nodiscard]] const ActorSample* Current() const noexcept;

    void Emit(Overlay flags, const FrameCtx& ctx, DebugDraw& worldDraw, DebugDraw& hud) const noexcept;

private:
    ActorSample* m_hist{nullptr};
    EntityId     m_pin{kNullEntity};
    u16          m_write{0};
    u8           m_valid{0};
    u8           m_scrub{0};
};
static_assert(sizeof(DebugInspector) == 16);

} // namespace en

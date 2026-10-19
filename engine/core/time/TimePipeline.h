#pragma once

#include "core/time/TimeModulator.h"

#include <algorithm>
#include <bit>

namespace en::time {

// Packed 24-byte configuration (u64 fields ordered first to eliminate 8-byte alignment holes).
struct TimePipelineConfig {
    u64  maxFrameDeltaNs   = 250'000'000ULL;
    u64  vsyncSnapMarginNs = 150'000ULL;
    u32  maxCatchupTicks   = 8;
    bool enableVsyncSnap   = true;
};
static_assert(sizeof(TimePipelineConfig) == 24);

struct TimeTelemetry {
    u64 totalSimTicks  = 0;
    u64 totalFrames    = 0;
    u64 droppedTicks   = 0;
    u64 accumulatorNs  = 0;
    u32 ticksLastFrame = 0;
};

// Coordinates deterministic 120Hz fixed-timestep simulation using an exact rational accumulator.
// Owns the clock instance and TimeModulator (placed at offset 0 for L1 cache alignment).
// Invariant: m_accUnits is scaled by (ns * kSimHz) where kNsPerSecond units == 1 exact simulation tick.
template <ClockSource ClockT = ManualClock>
class alignas(64) TimePipeline {
public:
    constexpr explicit TimePipeline(const TimePipelineConfig& cfg = {}, ClockT clock = {}) noexcept
        : m_config(SanitizeConfig(cfg))
        , m_clock(std::move(clock)) {}

    constexpr void Reset() noexcept {
        m_modulator.Reset();
        m_prevFrameNs = 0;
        m_accUnits    = 0;
        m_realDt      = 0.0f;
        m_rawAlpha    = 0.0f;
        m_telemetry   = {};
        m_paused      = false;
        m_inTick      = false;
    }

    constexpr void BeginFrame() noexcept {
        const u64 now = m_clock.NowNs();
        if (m_prevFrameNs == 0) [[unlikely]] {
            m_prevFrameNs = now;
            BeginFrameManual(0ULL);
            return;
        }
        const u64 rawDelta = (now >= m_prevFrameNs) ? (now - m_prevFrameNs) : 0ULL;
        m_prevFrameNs      = now;
        BeginFrameManual(rawDelta);
    }

    constexpr void BeginFrameManual(u64 rawDeltaNs) noexcept {
        CloseActiveTickIfAny();

        ++m_telemetry.totalFrames;
        m_telemetry.ticksLastFrame = 0;

        // Clamp abnormal OS spikes (window drag, debugger breakpoint) to prevent spiral-of-death.
        const u64 deltaNs = std::min(rawDeltaNs, m_config.maxFrameDeltaNs);
        m_realDt          = static_cast<f32>(static_cast<f64>(deltaNs) * kInvNsPerSecond);

        if (m_paused) {
            return;
        }

        // Convert deltaNs to exact rational accumulator units (1 tick == kNsPerSecond).
        // Exact multiples of kFixedDtNs map losslessly to N * kNsPerSecond to avoid integer truncation drift.
        u64 accDelta = (deltaNs > 0 && (deltaNs % kFixedDtNs == 0))
            ? ((deltaNs / kFixedDtNs) * kNsPerSecond)
            : (deltaNs * kSimHz);

        if (m_config.enableVsyncSnap && accDelta > 0) {
            accDelta = SnapToRefreshHarmonicUnits(accDelta);
        }

        m_accUnits += accDelta;
    }

    [[nodiscard]] constexpr SimStepView PollTick() noexcept {
        CloseActiveTickIfAny();

        if (m_accUnits < kNsPerSecond) {
            FinalizeFrame();
            return {};
        }

        if (m_telemetry.ticksLastFrame >= m_config.maxCatchupTicks) [[unlikely]] {
            m_telemetry.droppedTicks += m_accUnits / kNsPerSecond;
            m_accUnits %= kNsPerSecond;
            FinalizeFrame();
            return {};
        }

        m_inTick = true;
        return SimStepView{
            m_telemetry.totalSimTicks,
            m_telemetry.ticksLastFrame,
            m_modulator
        };
    }

    [[nodiscard]] constexpr RenderTimeView GetRenderView() const noexcept {
        return RenderTimeView{
            .channelAlpha = m_modulator.BakeRenderState(m_rawAlpha),
            .rawAlpha     = m_rawAlpha,
            .realDt       = m_realDt,
            .tickIndex    = m_telemetry.totalSimTicks,
            .frozenMask   = FrozenMask()
        };
    }

    constexpr void SetPaused(bool paused) noexcept {
        if (m_paused == paused) {
            return;
        }
        m_paused = paused;
        if (m_paused) {
            m_accUnits                = 0;
            m_rawAlpha                = 1.0f;
            m_telemetry.accumulatorNs = 0;
        } else {
            // Re-anchor clock baseline so elapsed pause duration does not trigger catchup ticks.
            m_prevFrameNs = m_clock.NowNs();
            FinalizeFrame();
        }
    }

    // Advances N simulation ticks (e.g. 1_ticks or 1_f60 == 2 ticks) while paused, clamped to maxCatchupTicks.
    constexpr void RequestStep(u32 ticks = 1) noexcept {
        if (m_paused && ticks > 0) {
            const u32 clamped = std::min(ticks, m_config.maxCatchupTicks);
            m_accUnits        = static_cast<u64>(clamped) * kNsPerSecond;
        }
    }

    constexpr void RequestSingleStep() noexcept {
        RequestStep(1);
    }

    // Multi-frame gameplay freeze across World..Camera (0x3F) while keeping Ui (bit 6) active.
    constexpr void RequestGameplayFreeze(u32 ticks, f32 intensity = 0.0f) noexcept {
        m_modulator.RequestMaskHitstop(kGameplayChannelsMask, ticks, intensity);
    }

    constexpr void OnWindowRefocus() noexcept {
        CloseActiveTickIfAny();
        m_prevFrameNs             = m_clock.NowNs();
        m_accUnits                = 0;
        m_rawAlpha                = m_paused ? 1.0f : 0.0f;
        m_telemetry.accumulatorNs = 0;
    }

    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }

    // Returns active hitstop freeze mask OR-ed with gameplay channels when paused; persists across 0-tick frames.
    [[nodiscard]] constexpr u8 FrozenMask() const noexcept {
        const u8 mask = m_modulator.FrozenMask();
        return m_paused ? static_cast<u8>(mask | kGameplayChannelsMask) : mask;
    }

    [[nodiscard]] constexpr std::span<const f32, kMaxTimeChannels> EffectiveScales() const noexcept {
        return m_modulator.EffectiveScales();
    }

    [[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] constexpr const TimePipelineConfig& Config() const noexcept { return m_config; }

    [[nodiscard]] constexpr TimeModulator& Modulator() noexcept { return m_modulator; }
    [[nodiscard]] constexpr const TimeModulator& Modulator() const noexcept { return m_modulator; }

    [[nodiscard]] constexpr ClockT& Clock() noexcept { return m_clock; }
    [[nodiscard]] constexpr const ClockT& Clock() const noexcept { return m_clock; }

    [[nodiscard]] constexpr u64 ComputeAuditHash() const noexcept {
        u64 h = m_modulator.ComputeAuditHash();
        auto mix = [&h](u64 val) constexpr noexcept {
            h ^= val + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        };
        mix(m_telemetry.totalSimTicks);
        mix(m_telemetry.droppedTicks);
        mix(m_accUnits);
        mix(static_cast<u64>(m_paused));
        return h;
    }

private:
    [[nodiscard]] static constexpr TimePipelineConfig SanitizeConfig(TimePipelineConfig cfg) noexcept {
        if (cfg.maxCatchupTicks == 0) {
            cfg.maxCatchupTicks = 1;
        }
        return cfg;
    }

    constexpr void CloseActiveTickIfAny() noexcept {
        if (!m_inTick) {
            return;
        }
        m_accUnits -= kNsPerSecond;
        ++m_telemetry.totalSimTicks;
        ++m_telemetry.ticksLastFrame;
        m_modulator.AdvanceTick();
        m_inTick = false;
    }

    constexpr void FinalizeFrame() noexcept {
        const u64 rem             = m_accUnits % kNsPerSecond;
        m_telemetry.accumulatorNs = rem / kSimHz;
        m_rawAlpha                = m_paused
            ? 1.0f
            : static_cast<f32>(static_cast<f64>(rem) * kInvNsPerSecond);
    }

    // O(1) branchless harmonic filter for standard VSync displays (480Hz, 240Hz, 120Hz, 60Hz, 30Hz).
    // All standard VSync intervals are power-of-two multiples {1, 2, 4, 8, 16} of a quarter-tick (250M units).
    [[nodiscard]] constexpr u64 SnapToRefreshHarmonicUnits(u64 accDelta) const noexcept {
        constexpr u64 kQuarterTick = kNsPerSecond / 4;
        const u64 marginUnits      = m_config.vsyncSnapMarginNs * kSimHz;
        const u64 nearestMult      = (accDelta + (kQuarterTick / 2)) / kQuarterTick;

        if (nearestMult >= 1 && nearestMult <= 16 && std::has_single_bit(nearestMult)) {
            const u64 target = nearestMult * kQuarterTick;
            const u64 diff   = (accDelta > target) ? (accDelta - target) : (target - accDelta);
            if (diff <= marginUnits) {
                return target;
            }
        }
        return accDelta;
    }

    // Placed at offset 0 (Cache Line 0) so BakedTickState aligns directly with the pipeline base address.
    TimeModulator                m_modulator{};
    TimePipelineConfig           m_config{};
    [[no_unique_address]] ClockT m_clock{};

    u64           m_prevFrameNs = 0;
    u64           m_accUnits    = 0;
    f32           m_realDt      = 0.0f;
    f32           m_rawAlpha    = 0.0f;
    TimeTelemetry m_telemetry{};
    bool          m_paused      = false;
    bool          m_inTick      = false;
};

} // namespace en::time

#pragma once

#include "core/time/TimeViews.h"

#include <algorithm>

namespace en::time {

struct TimePipelineConfig {
    u32  maxCatchupTicks   = 8;
    u64  maxFrameDeltaNs   = 250'000'000ULL;
    u64  vsyncSnapMarginNs = 150'000ULL;
    bool enableVsyncSnap   = true;
};

struct TimeTelemetry {
    u64 totalSimTicks  = 0;
    u64 totalFrames    = 0;
    u64 droppedTicks   = 0;
    u32 ticksLastFrame = 0;
    u64 accumulatorNs  = 0;
};

/**
 * TimePipeline: Coordinates deterministic 120Hz fixed-timestep simulation using a rational accumulator.
 * Owns the clock instance and ChannelModulator with single-threaded main-loop affinity.
 * Invariant: m_accUnits is scaled by (ns * kSimHz) for zero floating-point drift across ticks.
 */
template <ClockSource ClockT>
class TimePipeline {
public:
    constexpr explicit TimePipeline(const TimePipelineConfig& cfg = {}, ClockT clock = {}) noexcept
        : m_config(cfg)
        , m_clock(std::move(clock)) {}

    constexpr void Reset() noexcept {
        m_modulator.Reset();
        m_prevFrameNs   = 0;
        m_accUnits      = 0;
        m_realDt        = 0.0f;
        m_rawAlpha      = 0.0f;
        m_telemetry     = {};
        m_paused        = false;
        m_stepRequested = false;
        m_inTick        = false;
    }

    void BeginFrame() noexcept {
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
        u64 deltaNs = std::min(rawDeltaNs, m_config.maxFrameDeltaNs);
        m_realDt    = static_cast<f32>(static_cast<f64>(deltaNs) / static_cast<f64>(kNsPerSecond));

        if (m_paused) {
            if (m_stepRequested) {
                m_accUnits      = kNsPerSecond;
                m_stepRequested = false;
            }
            return;
        }

        if (m_config.enableVsyncSnap && deltaNs > 0) {
            deltaNs = SnapToRefreshHarmonic(deltaNs);
        }

        // Accumulate in exact rational units (ns * kSimHz) where 10^9 units == 1 simulation tick.
        // If deltaNs equals kFixedDtNs, accumulate exact 1 tick (kNsPerSecond) to eliminate integer division truncation.
        m_accUnits += (deltaNs == kFixedDtNs) ? kNsPerSecond : (deltaNs * kSimHz);
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

        m_modulator.BakeTickState();
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
            .tickIndex    = m_telemetry.totalSimTicks
        };
    }

    void SetPaused(bool paused) noexcept {
        if (m_paused == paused) {
            return;
        }
        m_paused        = paused;
        m_stepRequested = false;
        if (!m_paused) {
            // Re-anchor clock baseline so elapsed pause duration does not trigger catchup ticks.
            m_prevFrameNs = m_clock.NowNs();
        }
    }

    constexpr void RequestSingleStep() noexcept {
        if (m_paused) {
            m_stepRequested = true;
        }
    }

    void OnWindowRefocus() noexcept {
        CloseActiveTickIfAny();
        m_prevFrameNs = m_clock.NowNs();
        m_accUnits    = 0;
        m_rawAlpha    = 0.0f;
    }

    [[nodiscard]] constexpr bool IsPaused() const noexcept { return m_paused; }
    [[nodiscard]] constexpr const TimeTelemetry& Telemetry() const noexcept { return m_telemetry; }
    [[nodiscard]] constexpr const ChannelModulator& Modulator() const noexcept { return m_modulator; }
    [[nodiscard]] constexpr const ClockT& Clock() const noexcept { return m_clock; }

private:
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
            : static_cast<f32>(static_cast<f64>(rem) / static_cast<f64>(kNsPerSecond));
    }

    // Filter minor OS timer jitter on standard VSync displays (240Hz, 120Hz, 60Hz, 30Hz)
    // to prevent occasional double-tick or zero-tick stutters when running at native refresh.
    [[nodiscard]] constexpr u64 SnapToRefreshHarmonic(u64 deltaNs) const noexcept {
        constexpr std::array<u64, 4> kHarmonicsNs = {
            kFixedDtNs / 2,
            kFixedDtNs,
            kFixedDtNs * 2,
            kFixedDtNs * 4
        };
        const u64 margin = m_config.vsyncSnapMarginNs;
        for (const u64 target : kHarmonicsNs) {
            const u64 diff = (deltaNs > target) ? (deltaNs - target) : (target - deltaNs);
            if (diff <= margin) {
                return target;
            }
        }
        return deltaNs;
    }

    TimePipelineConfig           m_config{};
    [[no_unique_address]] ClockT m_clock{};
    ChannelModulator             m_modulator{};

    u64           m_prevFrameNs   = 0;
    u64           m_accUnits      = 0;
    f32           m_realDt        = 0.0f;
    f32           m_rawAlpha      = 0.0f;
    TimeTelemetry m_telemetry{};
    bool          m_paused        = false;
    bool          m_stepRequested = false;
    bool          m_inTick        = false;
};

} // namespace en::time
#include "time/TimePipeline.h"
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace en {

void SimStepView::RequestHitstop(TimeChannel ch, u32 ticks, f32 intensity) const noexcept {
    if (pipeline) pipeline->RequestHitstop(ch, ticks, intensity);
}

void SimStepView::RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity) const noexcept {
    if (pipeline) pipeline->RequestMaskHitstop(channelMask, ticks, intensity);
}

void SimStepView::RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) const noexcept {
    if (pipeline) pipeline->RequestTimeRamp(ch, targetScale, holdTicks, blendTicks);
}

u64 TimePipeline::NowWallNs() noexcept {
#if defined(_WIN32)
    static LARGE_INTEGER s_freq{};
    if (s_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const u64 ticks = static_cast<u64>(counter.QuadPart);
    const u64 freq = static_cast<u64>(s_freq.QuadPart);
    return (ticks / freq) * kNsPerSecond + ((ticks % freq) * kNsPerSecond) / freq;
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

void TimePipeline::BakeChannel(u8 idx) noexcept {
    idx &= 7U;
    const ChannelModSlot& s = m_slots[idx];
    const u8 bit = static_cast<u8>(1U << idx);

    u8 cur = idx;
    f32 hierarchyScale = m_masterScale;
    for (u8 depth = 0; depth < kMaxTimeChannels && cur < kMaxTimeChannels; ++depth) {
        if (m_slots[cur].hitstopTicks > 0) {
            m_frozenMask |= bit;
            m_bakedTick.effectiveScale[idx] = 0.0f;
            m_bakedTick.scaledDt[idx] = 0.0f;
            return;
        }
        if (cur != idx) {
            hierarchyScale *= m_baseScales[cur];
        }
        cur = m_parentChannel[cur];
    }

    m_frozenMask &= static_cast<u8>(~bit);
    f32 rampScale = 1.0f;
    if ((m_activeRampMask & bit) != 0) {
        if (s.rampElapsedTicks < s.rampHoldTicks) {
            rampScale = s.rampTargetScale;
        } else if (s.rampBlendTicks > 0) {
            const u32 blendElapsed = s.rampElapsedTicks - s.rampHoldTicks;
            const f32 t = std::clamp(static_cast<f32>(blendElapsed) / static_cast<f32>(s.rampBlendTicks), 0.0f, 1.0f);
            rampScale = s.rampTargetScale + (1.0f - s.rampTargetScale) * t;
        }
    }
    const f32 eff = std::clamp(hierarchyScale * m_baseScales[idx] * rampScale, 0.0f, 10.0f);
    m_bakedTick.effectiveScale[idx] = eff;
    m_bakedTick.scaledDt[idx] = eff * kFixedDtSec;
    if (eff <= 1e-6f) {
        m_frozenMask |= bit;
    }
}

void TimePipeline::RebakeAllChannels() noexcept {
    for (u8 i = 0; i < kMaxTimeChannels; ++i) {
        BakeChannel(i);
    }
}

void TimePipeline::SetMasterScale(f32 scale) noexcept {
    m_masterScale = std::clamp(scale, 0.0f, 10.0f);
    RebakeAllChannels();
}

void TimePipeline::SetParentChannel(TimeChannel child, u8 parentChannelIdx) noexcept {
    const u8 cIdx = static_cast<u8>(child) & 7U;
    m_parentChannel[cIdx] = (parentChannelIdx < kMaxTimeChannels && parentChannelIdx != cIdx) ? parentChannelIdx : 0xFFU;
    RebakeAllChannels();
}

void TimePipeline::AdvanceChannelModulationTick() noexcept {
    u8 mask = static_cast<u8>(m_activeHitstopMask | m_activeRampMask);
    while (mask != 0) {
        const u8 idx = static_cast<u8>(std::countr_zero(mask));
        mask &= static_cast<u8>(mask - 1U);
        const u8 bit = static_cast<u8>(1U << idx);
        ChannelModSlot& s = m_slots[idx];
        if (s.hitstopTicks > 0) {
            --s.hitstopTicks;
            if (s.hitstopTicks == 0) {
                s.hitstopIntensity = 0.0f;
                m_activeHitstopMask &= static_cast<u8>(~bit);
            }
        } else if ((m_activeRampMask & bit) != 0) {
            ++s.rampElapsedTicks;
            const u32 totalRamp = static_cast<u32>(s.rampHoldTicks) + static_cast<u32>(s.rampBlendTicks);
            if (s.rampElapsedTicks >= totalRamp) {
                s.rampTargetScale = 1.0f;
                s.rampHoldTicks = 0;
                s.rampBlendTicks = 0;
                s.rampElapsedTicks = 0;
                m_activeRampMask &= static_cast<u8>(~bit);
            }
        }
        BakeChannel(idx);
    }
}

void TimePipeline::BeginFrame() noexcept {
    const u64 now = NowWallNs();
    if (m_lastWallNs == 0 || now <= m_lastWallNs) {
        m_lastWallNs = now;
        BeginFrameManual(kFixedDtNs);
        return;
    }
    const u64 deltaNs = now - m_lastWallNs;
    m_lastWallNs = now;
    BeginFrameManual(deltaNs);
}

void TimePipeline::BeginFrameManual(u64 rawDeltaNs) noexcept {
    constexpr u64 kMaxFrameDeltaNs = 250'000'000ULL;
    const u64 clampedNs = std::min(rawDeltaNs, kMaxFrameDeltaNs);
    m_realDtSec = static_cast<f32>(clampedNs) * 1e-9f;
    ++m_telemetry.totalFrames;
    m_telemetry.ticksLastFrame = 0;
    m_catchupCursor = 0;

    u64 units = clampedNs * static_cast<u64>(kSimHz);
    constexpr u64 kQuarterTickUnits = kNsPerSecond / 4ULL;
    constexpr u64 kMarginUnits = 150'000ULL * static_cast<u64>(kSimHz);
    for (u32 mult : {1U, 2U, 4U, 8U, 16U}) {
        const u64 target = kQuarterTickUnits * mult;
        const u64 diff = (units > target) ? (units - target) : (target - units);
        if (diff <= kMarginUnits) {
            units = target;
            break;
        }
    }

    m_accUnits += units;
    const u64 maxAccUnits = static_cast<u64>(m_telemetry.maxCatchupTicks) * kNsPerSecond;
    if (m_accUnits > maxAccUnits) {
        const u64 dropped = (m_accUnits - maxAccUnits) / kNsPerSecond;
        m_telemetry.droppedTicks += dropped;
        m_accUnits = maxAccUnits;
    }
    m_telemetry.accumulatorNs = m_accUnits / kSimHz;
}

SimStepView TimePipeline::PollTick() noexcept {
    if (m_accUnits < kNsPerSecond || m_catchupCursor >= m_telemetry.maxCatchupTicks) {
        return SimStepView{};
    }
    m_accUnits -= kNsPerSecond;
    m_telemetry.accumulatorNs = m_accUnits / kSimHz;
    ++m_telemetry.totalSimTicks;
    ++m_telemetry.ticksLastFrame;

    const u8 effFrozen = FrozenMask();
    if (m_stepAllowanceTicks > 0) --m_stepAllowanceTicks;

    SimStepView view{};
    view.baked = &m_bakedTick;
    view.pipeline = this;
    view.tickIndex = m_telemetry.totalSimTicks;
    view.catchupIndex = m_catchupCursor++;
    view.substepTotal = static_cast<u16>(m_telemetry.ticksLastFrame);
    view.frozenMask = effFrozen;
    view.valid = true;

    AdvanceChannelModulationTick();
    return view;
}

RenderTimeView TimePipeline::GetRenderView() const noexcept {
    RenderTimeView rv{};
    rv.rawAlpha = std::clamp(static_cast<f32>(m_accUnits) / static_cast<f32>(kNsPerSecond), 0.0f, 1.0f);
    rv.realDt = m_realDtSec;
    rv.tickIndex = m_telemetry.totalSimTicks;
    const u8 frozen = FrozenMask();
    for (u8 i = 0; i < kMaxTimeChannels; ++i) {
        rv.channelAlpha[i] = ((frozen & (1U << i)) != 0) ? 1.0f : rv.rawAlpha;
    }
    return rv;
}

void TimePipeline::SetBaseScale(TimeChannel ch, f32 scale) noexcept {
    const u8 idx = static_cast<u8>(ch) & 7U;
    m_baseScales[idx] = std::clamp(scale, 0.0f, 10.0f);
    RebakeAllChannels();
}

void FramePacer::EndFrameAndPace() noexcept {
    m_lastSleepNs = 0;
    m_lastSpinNs = 0;
    if (m_targetFrameNs == 0) return;
    if (m_frameStartNs == 0) {
        m_frameStartNs = TimePipeline::NowWallNs();
        return;
    }
    const u64 targetEndNs = m_frameStartNs + m_targetFrameNs;
    u64 now = TimePipeline::NowWallNs();
    if (now < targetEndNs) {
        const u64 remainingNs = targetEndNs - now;
        if (remainingNs > kSpinMarginNs) {
            const u64 sleepDurationNs = remainingNs - kSpinMarginNs;
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleepDurationNs));
            const u64 afterSleep = TimePipeline::NowWallNs();
            m_lastSleepNs = (afterSleep > now) ? (afterSleep - now) : sleepDurationNs;
            now = afterSleep;
        }
        const u64 spinStart = now;
        while (now < targetEndNs) {
            std::this_thread::yield();
            now = TimePipeline::NowWallNs();
        }
        m_lastSpinNs = (now > spinStart) ? (now - spinStart) : 0ULL;
    }
    m_frameStartNs = now;
}

void TimePipeline::RequestHitstop(TimeChannel ch, u32 ticks, f32 intensity) noexcept {
    if (ticks == 0) return;
    const u8 idx = static_cast<u8>(ch) & 7U;
    ChannelModSlot& s = m_slots[idx];
    const u16 clamped = static_cast<u16>(std::min<u32>(ticks, 0xFFFFU));
    if (clamped >= s.hitstopTicks) {
        s.hitstopTicks = clamped;
        s.hitstopIntensity = std::max(s.hitstopIntensity, intensity);
        m_activeHitstopMask |= static_cast<u8>(1U << idx);
        BakeChannel(idx);
    }
}

void TimePipeline::RequestMaskHitstop(u8 channelMask, u32 ticks, f32 intensity) noexcept {
    u8 m = channelMask;
    while (m != 0) {
        const u8 idx = static_cast<u8>(std::countr_zero(m));
        m &= static_cast<u8>(m - 1U);
        RequestHitstop(static_cast<TimeChannel>(idx), ticks, intensity);
    }
}

void TimePipeline::RequestTimeRamp(TimeChannel ch, f32 targetScale, u32 holdTicks, u32 blendTicks) noexcept {
    const u8 idx = static_cast<u8>(ch) & 7U;
    ChannelModSlot& s = m_slots[idx];
    s.rampTargetScale = std::clamp(targetScale, 0.0f, 10.0f);
    s.rampHoldTicks = static_cast<u16>(std::min<u32>(holdTicks, 0xFFFFU));
    s.rampBlendTicks = static_cast<u16>(std::min<u32>(blendTicks, 0xFFFFU));
    s.rampElapsedTicks = 0;
    m_activeRampMask |= static_cast<u8>(1U << idx);
    BakeChannel(idx);
}

void TimePipeline::RequestStep(u32 ticks) noexcept {
    const u32 clamped = std::min(ticks, m_telemetry.maxCatchupTicks);
    m_stepAllowanceTicks += clamped;
    m_accUnits += static_cast<u64>(clamped) * kNsPerSecond;
}

u64 TimePipeline::ComputeAuditHash() const noexcept {
    u64 h = Mix64(m_telemetry.totalSimTicks ^ m_accUnits);
    h = HashCombine64(h, static_cast<u64>(FrozenMask()) | (static_cast<u64>(m_activeHitstopMask) << 8U));
    for (u8 i = 0; i < kMaxTimeChannels; ++i) {
        h = HashCombine64(h, std::bit_cast<u32>(m_bakedTick.scaledDt[i]));
    }
    return h;
}

} // namespace en

#include "debug/DebugTools.h"

namespace en {

void DebugProfiler::BeginFrame() noexcept {
    if (!m_samples) return;
    const u16 slot = static_cast<u16>(m_idx & (kProfileFrames - 1U));
    if (m_valid >= kProfileFrames) {
        const u16 old = m_samples[slot].us[static_cast<usize>(ProfileScope::Frame)];
        const u8  b   = ToBucket(old);
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
    s.entities     = t.entities;
    s.quads        = t.quads;
    s.batches      = t.batches;
    s.contacts     = t.contacts;
    s.voices       = t.voices;
    if (t.scales) {
        s.worldScaleQ6  = static_cast<u8>(std::clamp(t.scales[0] * 64.0f + 0.5f, 0.0f, 255.0f));
        s.playerScaleQ6 = static_cast<u8>(std::clamp(t.scales[1] * 64.0f + 0.5f, 0.0f, 255.0f));
    }
}

void DebugProfiler::EndFrame() noexcept {
    if (!m_samples) return;
    FrameSample& s     = m_samples[m_idx & (kProfileFrames - 1U)];
    u32          total = s.us[static_cast<usize>(ProfileScope::Frame)];
    if (total == 0U) {
        for (usize i = 1U; i < static_cast<usize>(ProfileScope::Count); ++i) total += s.us[i];
        s.us[static_cast<usize>(ProfileScope::Frame)] =
            static_cast<u16>(std::min<u32>(65535U, total));
    }
    const u8 b = ToBucket(s.us[static_cast<usize>(ProfileScope::Frame)]);
    ++m_buckets[b];
    m_bucketMask |= (1ULL << b);
    m_idx         = static_cast<u16>((m_idx + 1U) & (kProfileFrames - 1U));
    if (m_valid < kProfileFrames) ++m_valid;
}

u32 DebugProfiler::PercentileUs(u32 rank256) const noexcept {
    if (m_valid == 0U) return 0U;
    const u32 target = std::max<u32>(1U, (rank256 * m_valid) / kProfileFrames);
    u32       acc    = 0U;
    u64       mask   = m_bucketMask;
    while (mask != 0ULL) {
        const u32 b = static_cast<u32>(std::countr_zero(mask));
        mask       &= mask - 1ULL;
        acc        += m_buckets[b];
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
    const f32          x = static_cast<f32>(vp.w) - 220.0f;
    hud.Box({x, 8.0f, 208.0f, 100.0f}, 1.0f, {0.2f, 0.8f, 0.5f, 0.85f});

    char  buf[64]{};
    char* end = buf + sizeof(buf);
    char* p   = DebugDraw::Append(buf, end, "frame ");
    p         = DebugDraw::FmtF2(p, end, static_cast<f32>(L.us[0]) * 0.001f);
    p         = DebugDraw::Append(p, end, " ms");
    hud.Text(x + 8.0f, 16.0f, {buf, static_cast<usize>(p - buf)}, {0.3f, 1.0f, 0.6f, 1.0f}, 1.0f);

    p = DebugDraw::Append(buf, end, "e ");
    p = DebugDraw::FmtI64(p, end, L.entities);
    p = DebugDraw::Append(p, end, " q ");
    p = DebugDraw::FmtI64(p, end, L.quads);
    p = DebugDraw::Append(p, end, " c ");
    p = DebugDraw::FmtI64(p, end, L.contacts);
    hud.Text(x + 8.0f, 34.0f, {buf, static_cast<usize>(p - buf)}, {0.85f, 0.95f, 1.0f, 1.0f}, 1.0f);

    const u32 p99 = PercentileUs(250U);
    p = DebugDraw::Append(buf, end, "p99 ");
    p = DebugDraw::FmtF2(p, end, static_cast<f32>(p99) * 0.001f);
    p = DebugDraw::Append(p, end, " ms");
    hud.Text(x + 8.0f, 52.0f, {buf, static_cast<usize>(p - buf)}, {0.9f, 0.85f, 0.4f, 1.0f}, 1.0f);

    const u32 n = std::min<u32>(m_valid, 64U);
    for (u32 i = 0U; i < n; ++i) {
        const u16 idx = static_cast<u16>((m_idx + kProfileFrames - n + i) & (kProfileFrames - 1U));
        const f32 h   = std::clamp(static_cast<f32>(m_samples[idx].us[0]) * 0.0025f, 1.0f, 40.0f);
        hud.Rect({x + 8.0f + static_cast<f32>(i) * 3.0f, 96.0f - h, 2.0f, h},
                 {0.3f, 0.9f, 0.5f, 0.8f});
    }
}

} // namespace en

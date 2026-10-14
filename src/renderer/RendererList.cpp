#include "renderer/RendererList.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace en::renderer {

namespace {

consteval std::array<u16, RenderList::kChunkQuads * 6> BuildStaticQuadIndices() noexcept {
    std::array<u16, RenderList::kChunkQuads * 6> indices{};
    for (u32 i = 0; i < RenderList::kChunkQuads; ++i) {
        const u16 base = static_cast<u16>(i * 4);
        const u32 idx  = i * 6;
        indices[idx + 0] = base + 0;
        indices[idx + 1] = base + 1;
        indices[idx + 2] = base + 2;
        indices[idx + 3] = base + 2;
        indices[idx + 4] = base + 3;
        indices[idx + 5] = base + 0;
    }
    return indices;
}

alignas(64) constexpr auto kStaticQuadIndices = BuildStaticQuadIndices();

constexpr void HashBytesFNV1a(u64& hash, const void* data, std::size_t len) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= 1099511628211ULL;
    }
}

} // namespace

void RenderList::Reset(platform::Extent2i defaultViewport) noexcept {
    m_quadCount   = 0;
    m_batchCount  = 0;
    m_prevKey     = 0;
    m_isMonotonic = true;
    m_compiled    = false;
    m_stats       = {};

    std::memset(m_histograms.data(), 0, sizeof(m_histograms));

    const FRect fullVp{
        0.0f,
        0.0f,
        static_cast<f32>(defaultViewport.w),
        static_cast<f32>(defaultViewport.h)
    };

    for (u32 i = 0; i < kMaxViews; ++i) {
        m_views[i] = Camera2D{
            .worldCenterX = fullVp.w * 0.5f,
            .worldCenterY = fullVp.h * 0.5f,
            .zoom         = 1.0f,
            .viewport     = fullVp,
            .scissor      = {},
            .screenSpace  = true
        };
    }

    for (u32 i = 0; i < kMaxPasses; ++i) {
        m_passes[i] = RenderPassDesc{
            .clearColor = Colors::Black,
            .clear      = (i == 0),
            .enabled    = (i == 0)
        };
    }
}

void RenderList::SetPass(u8 passId, const RenderPassDesc& desc) noexcept {
    m_passes[passId & 0x0Fu] = desc;
}

void RenderList::SetView(u8 viewSlot, const Camera2D& cam) noexcept {
    m_views[viewSlot & 0x0Fu] = cam;
}

void RenderList::PushQuad(const QuadCmd& cmd) noexcept {
    const u8 passIdx = cmd.passId & 0x0Fu;
    if (!m_passes[passIdx].enabled || cmd.w <= 0.0f || cmd.h <= 0.0f) {
        return;
    }

    const Camera2D& cam = m_views[cmd.viewSlot & 0x0Fu];
    const f32 zoom = cam.screenSpace ? 1.0f : cam.zoom;

    const f32 cx = cam.screenSpace
        ? cmd.x
        : (cmd.x - cam.worldCenterX) * zoom + (cam.viewport.w * 0.5f);
    const f32 cy = cam.screenSpace
        ? cmd.y
        : (cmd.y - cam.worldCenterY) * zoom + (cam.viewport.h * 0.5f);

    const f32 halfW = (cmd.w * 0.5f) * zoom;
    const f32 halfH = (cmd.h * 0.5f) * zoom;

    const f32 hxX = halfW * cmd.cosRot;
    const f32 hxY = halfW * cmd.sinRot;
    const f32 hyX = -halfH * cmd.sinRot;
    const f32 hyY = halfH * cmd.cosRot;

    // Conservative L1 bounding radius in view-space for fast branchless AABB frustum culling.
    const f32 boundRx = std::abs(hxX) + std::abs(hyX);
    const f32 boundRy = std::abs(hxY) + std::abs(hyY);

    if (cam.viewport.w > 0.0f && cam.viewport.h > 0.0f) {
        if (cx + boundRx < 0.0f || cx - boundRx > cam.viewport.w ||
            cy + boundRy < 0.0f || cy - boundRy > cam.viewport.h) {
            ++m_stats.culledQuads;
            return;
        }
    }

    if (m_quadCount >= kMaxQuads) {
        ++m_stats.droppedQuads;
        return;
    }

    const u16 quadIdx = static_cast<u16>(m_quadCount);
    const u64 key = SortKey::Pack(cmd, quadIdx);

    if (m_quadCount > 0 && key < m_prevKey) {
        m_isMonotonic = false;
    }
    m_prevKey = key;

    // Accumulate histograms for bytes 2..7 during insertion to avoid pre-sort memory scans.
    for (u32 b = 0; b < 6; ++b) {
        const u8 byteVal = static_cast<u8>((key >> ((b + 2) * 8)) & 0xFFu);
        ++m_histograms[b][byteVal];
    }

    f32 u0 = cmd.uv.x;
    f32 v0 = cmd.uv.y;
    f32 u1 = cmd.uv.x + cmd.uv.w;
    f32 v1 = cmd.uv.y + cmd.uv.h;
    if (cmd.flipX) { std::swap(u0, u1); }
    if (cmd.flipY) { std::swap(v0, v1); }

    m_quads[quadIdx] = PreparedQuad{
        .cx         = cx,
        .cy         = cy,
        .hxX        = hxX,
        .hxY        = hxY,
        .hyX        = hyX,
        .hyY        = hyY,
        .u0         = u0,
        .v0         = v0,
        .u1         = u1,
        .v1         = v1,
        .color      = cmd.color,
        .textureGen = cmd.texture.generation
    };

    m_keys[quadIdx] = key;
    ++m_quadCount;
    ++m_stats.submittedQuads;
}

void RenderList::PushRect(FRect rect, Color color, u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    PushQuad(QuadCmd{
        .x        = rect.x + rect.w * 0.5f,
        .y        = rect.y + rect.h * 0.5f,
        .w        = rect.w,
        .h        = rect.h,
        .color    = color,
        .texture  = kWhiteTexture,
        .depth    = depth,
        .passId   = passId,
        .layer    = layer,
        .viewSlot = viewSlot
    });
}

void RenderList::PushLine(
    f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, Color color,
    u8 passId, u8 layer, u16 depth, u8 viewSlot) noexcept {
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-8f || thickness <= 0.0f) {
        return;
    }

    const f32 len = std::sqrt(lenSq);
    const f32 invLen = 1.0f / len;

    PushQuad(QuadCmd{
        .x        = (x0 + x1) * 0.5f,
        .y        = (y0 + y1) * 0.5f,
        .w        = len,
        .h        = thickness,
        .cosRot   = dx * invLen,
        .sinRot   = dy * invLen,
        .color    = color,
        .texture  = kWhiteTexture,
        .depth    = depth,
        .passId   = passId,
        .layer    = layer,
        .viewSlot = viewSlot
    });
}

void RenderList::Compile() noexcept {
    if (m_compiled) {
        return;
    }
    m_compiled = true;
    m_batchCount = 0;

    if (m_quadCount == 0) {
        m_stats.monotonicFastPath = true;
        return;
    }

    m_stats.monotonicFastPath = m_isMonotonic;

    // Stage 1: 6-Byte LSD Radix Sort on bytes [2..7] (bits 16..63).
    // Bytes [0..1] store the monotonically increasing quadIndex and never need sorting.
    if (!m_isMonotonic && m_quadCount > 1) {
        u64* src = m_keys.data();
        u64* dst = m_scratchKeys.data();

        for (u32 b = 0; b < 6; ++b) {
            const u32 shift = (b + 2) * 8;
            const auto& hist = m_histograms[b];
            const u8 firstByte = static_cast<u8>((src[0] >> shift) & 0xFFu);

            // Skip radix pass if all active keys share the exact same byte value.
            if (hist[firstByte] == m_quadCount) {
                continue;
            }

            std::array<u32, 256> offsets{};
            u32 runningOffset = 0;
            for (u32 i = 0; i < 256; ++i) {
                offsets[i] = runningOffset;
                runningOffset += hist[i];
            }

            for (u32 i = 0; i < m_quadCount; ++i) {
                const u64 k = src[i];
                const u8 bucket = static_cast<u8>((k >> shift) & 0xFFu);
                dst[offsets[bucket]++] = k;
            }

            std::swap(src, dst);
            ++m_stats.radixPassesRun;
        }

        if (src != m_keys.data()) {
            std::memcpy(m_keys.data(), src, m_quadCount * sizeof(u64));
        }
    }

    // Stage 2: Single-pass XOR State Coalescing.
    u64 prevKey = m_keys[0];
    u16 prevGen = m_quads[SortKey::QuadIndex(prevKey)].textureGen;

    m_batches[0] = DrawBatch{
        .firstQuad = 0,
        .quadCount = 1,
        .texture   = TextureHandle{SortKey::TextureIndex(prevKey), prevGen},
        .passId    = SortKey::PassId(prevKey),
        .viewSlot  = SortKey::ViewSlot(prevKey),
        .blend     = SortKey::Blend(prevKey)
    };
    m_batchCount = 1;

    for (u32 i = 1; i < m_quadCount; ++i) {
        const u64 key = m_keys[i];
        const u16 gen = m_quads[SortKey::QuadIndex(key)].textureGen;
        const bool sameState = (((key ^ prevKey) & SortKey::kBatchStateMask) == 0) && (gen == prevGen);

        if (sameState) {
            ++m_batches[m_batchCount - 1].quadCount;
        } else {
            if (m_batchCount >= kMaxBatches) {
                break;
            }
            m_batches[m_batchCount++] = DrawBatch{
                .firstQuad = i,
                .quadCount = 1,
                .texture   = TextureHandle{SortKey::TextureIndex(key), gen},
                .passId    = SortKey::PassId(key),
                .viewSlot  = SortKey::ViewSlot(key),
                .blend     = SortKey::Blend(key)
            };
            prevKey = key;
            prevGen = gen;
        }
    }

    m_stats.drawBatches = m_batchCount;
}

void RenderList::ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept {
    for (u32 i = 0; i < quadCount; ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[firstQuad + i]);
        const PreparedQuad& q = m_quads[qIdx];
        Vertex2D* v = &outVertices[i * 4];

        v[0] = Vertex2D{q.cx - q.hxX - q.hyX, q.cy - q.hxY - q.hyY, q.color, q.u0, q.v0};
        v[1] = Vertex2D{q.cx + q.hxX - q.hyX, q.cy + q.hxY - q.hyY, q.color, q.u1, q.v0};
        v[2] = Vertex2D{q.cx + q.hxX + q.hyX, q.cy + q.hxY + q.hyY, q.color, q.u1, q.v1};
        v[3] = Vertex2D{q.cx - q.hxX + q.hyX, q.cy - q.hxY + q.hyY, q.color, q.u0, q.v1};
    }
}

u64 RenderList::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    HashBytesFNV1a(hash, m_batches.data(), m_batchCount * sizeof(DrawBatch));
    for (u32 i = 0; i < m_quadCount; ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[i]);
        const u64 stateBits = m_keys[i] & ~0xFFFFULL;
        HashBytesFNV1a(hash, &stateBits, sizeof(u64));
        HashBytesFNV1a(hash, &m_quads[qIdx], sizeof(PreparedQuad));
    }
    return hash;
}

std::span<const u16> RenderList::SharedQuadIndices(u32 quadCount) noexcept {
    const u32 clamped = std::min(quadCount, kChunkQuads);
    return {kStaticQuadIndices.data(), clamped * 6};
}

} // namespace en::renderer
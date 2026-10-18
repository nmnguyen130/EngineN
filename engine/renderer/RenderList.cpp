#include "renderer/RenderList.h"

#include <algorithm>
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

constexpr void HashWords64(u64& hash, const void* data, std::size_t wordCount) noexcept {
    const auto* words = static_cast<const u64*>(data);
    for (std::size_t i = 0; i < wordCount; ++i) {
        u64 k = words[i];
        k ^= k >> 30;
        k *= 0xBF58476D1CE4E5B9ULL;
        k ^= k >> 27;
        k *= 0x94D049BB133111EBULL;
        k ^= k >> 31;
        hash ^= k;
        hash *= 1099511628211ULL;
    }
}

} // namespace

void RenderList::Reset(ViewportExtent size) noexcept {
    m_prevKey         = 0;
    m_varyingBits     = 0;
    m_stats           = {};
    m_enabledPassMask = 0x0001u;
    m_clearPassMask   = 0x0001u;
    m_isMonotonic     = true;
    m_compiled        = false;

    const FRect fullVp{
        0.0f,
        0.0f,
        static_cast<f32>(size.w),
        static_cast<f32>(size.h)
    };
    m_views.fill(Camera2D{
        .worldCenterX = fullVp.w * 0.5f,
        .worldCenterY = fullVp.h * 0.5f,
        .zoom         = 1.0f,
        .viewport     = fullVp,
        .scissor      = {},
        .screenSpace  = true
    });

    m_passes.fill(RenderPassDesc{
        .clearColor = Colors::Black,
        .clear      = false,
        .enabled    = false
    });
    m_passes[0].clear   = true;
    m_passes[0].enabled = true;
}

void RenderList::PushQuad(const QuadCmd& cmd) noexcept {
    const u8 passIdx = cmd.passId & 0x0Fu;
    if (((m_enabledPassMask & (1u << passIdx)) == 0u) || cmd.w <= 0.0f || cmd.h <= 0.0f) {
        return;
    }

    const Camera2D& cam = m_views[cmd.viewSlot & 0x0Fu];
    const f32 zoom = cam.screenSpace ? 1.0f : cam.zoom;
    if (zoom <= 0.0f) {
        return;
    }

    const f32 cx = cam.screenSpace
        ? cmd.x
        : (cmd.x - cam.worldCenterX) * zoom + (cam.viewport.w * 0.5f);
    const f32 cy = cam.screenSpace
        ? cmd.y
        : (cmd.y - cam.worldCenterY) * zoom + (cam.viewport.h * 0.5f);

    const f32 halfScale = 0.5f * zoom;
    const f32 halfW     = cmd.w * halfScale;
    const f32 halfH     = cmd.h * halfScale;

    const f32 hxX = halfW * cmd.cosRot;
    const f32 hxY = halfW * cmd.sinRot;
    const f32 hyX = -halfH * cmd.sinRot;
    const f32 hyY = halfH * cmd.cosRot;

    // Conservative L1 bounding radius in view-space for branchless AABB frustum culling.
    const f32 boundRx = std::abs(hxX) + std::abs(hyX);
    const f32 boundRy = std::abs(hxY) + std::abs(hyY);

    if (cam.viewport.w > 0.0f && cam.viewport.h > 0.0f) {
        if (cx + boundRx < 0.0f || cx - boundRx > cam.viewport.w ||
            cy + boundRy < 0.0f || cy - boundRy > cam.viewport.h) {
            ++m_stats.culledQuads;
            return;
        }
    }

    const u32 quadCount = m_stats.submittedQuads;
    if (quadCount >= kMaxQuads) {
        ++m_stats.droppedQuads;
        return;
    }

    const u16 quadIdx = static_cast<u16>(quadCount);
    const u64 key     = SortKey::Pack(cmd, quadIdx);

    if (quadCount > 0) {
        m_varyingBits |= (key ^ m_prevKey);
        if (key < m_prevKey) {
            m_isMonotonic = false;
        }
    }
    m_prevKey = key;

    const f32 uBase = cmd.uv.x;
    const f32 vBase = cmd.uv.y;
    const f32 uFar  = uBase + cmd.uv.w;
    const f32 vFar  = vBase + cmd.uv.h;

    m_quads[quadIdx] = PreparedQuad{
        .cx         = cx,
        .cy         = cy,
        .d0X        = hxX + hyX,
        .d0Y        = hxY + hyY,
        .d1X        = hxX - hyX,
        .d1Y        = hxY - hyY,
        .u0         = cmd.flipX ? uFar  : uBase,
        .v0         = cmd.flipY ? vFar  : vBase,
        .u1         = cmd.flipX ? uBase : uFar,
        .v1         = cmd.flipY ? vBase : vFar,
        .color      = cmd.color,
        .textureGen = static_cast<u16>(cmd.texture.IsValid() ? cmd.texture.generation : 1u)
    };

    m_keys[quadIdx] = key;
    m_stats.submittedQuads = quadCount + 1;
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

    const f32 len    = std::sqrt(lenSq);
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

    const u32 quadCount = m_stats.submittedQuads;
    if (quadCount == 0) {
        m_stats.monotonicFastPath = true;
        return;
    }

    m_stats.monotonicFastPath = m_isMonotonic;

    // Stage 1: 6-Byte LSD Radix Sort on bytes [2..7] (bits 16..63).
    // Active varying bytes are identified via m_varyingBits and histogrammed in one linear scan.
    if (!m_isMonotonic && quadCount > 1) {
        u8 activeShifts[6]{};
        u32 numPasses = 0;
        for (u32 b = 0; b < 6; ++b) {
            const u8 shift = static_cast<u8>((b + 2) * 8);
            if (((m_varyingBits >> shift) & 0xFFULL) != 0) {
                activeShifts[numPasses++] = shift;
            }
        }

        if (numPasses > 0) {
            alignas(64) std::array<std::array<u32, 256>, 6> histograms{};
            for (u32 i = 0; i < quadCount; ++i) {
                const u64 k = m_keys[i];
                for (u32 p = 0; p < numPasses; ++p) {
                    ++histograms[p][static_cast<u8>((k >> activeShifts[p]) & 0xFFu)];
                }
            }

            u64* src = m_keys.data();
            u64* dst = m_scratchKeys.data();

            for (u32 p = 0; p < numPasses; ++p) {
                const u8 shift = activeShifts[p];
                auto& offsets  = histograms[p];

                u32 runningOffset = 0;
                for (u32 i = 0; i < 256; ++i) {
                    const u32 count = offsets[i];
                    offsets[i] = runningOffset;
                    runningOffset += count;
                }

                for (u32 i = 0; i < quadCount; ++i) {
                    const u64 k = src[i];
                    const u8 bucket = static_cast<u8>((k >> shift) & 0xFFu);
                    dst[offsets[bucket]++] = k;
                }

                std::swap(src, dst);
            }

            if (src != m_keys.data()) {
                std::memcpy(m_keys.data(), src, quadCount * sizeof(u64));
            }
            m_stats.radixPassesRun = numPasses;
        }
    }

    // Stage 2: Single-pass XOR State Coalescing.
    u64 prevKey = m_keys[0];
    u16 prevGen = m_quads[SortKey::QuadIndex(prevKey)].textureGen;

    DrawBatch* curBatch = &m_batches[0];
    *curBatch = DrawBatch{
        .firstQuad = 0,
        .quadCount = 1,
        .texture   = TextureHandle{SortKey::TextureIndex(prevKey), prevGen},
        .passId    = SortKey::PassId(prevKey),
        .viewSlot  = SortKey::ViewSlot(prevKey),
        .blend     = SortKey::Blend(prevKey)
    };
    u32 batchCount = 1;

    for (u32 i = 1; i < quadCount; ++i) {
        const u64 key = m_keys[i];
        const u16 gen = m_quads[SortKey::QuadIndex(key)].textureGen;
        if (((key ^ prevKey) & SortKey::kBatchStateMask) == 0 && gen == prevGen) {
            ++curBatch->quadCount;
        } else {
            if (batchCount >= kMaxBatches) {
                break;
            }
            curBatch = &m_batches[batchCount++];
            *curBatch = DrawBatch{
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

    m_stats.drawBatches = batchCount;
}

void RenderList::ExpandVertices(u32 firstQuad, u32 quadCount, std::span<Vertex2D> outVertices) const noexcept {
    for (u32 i = 0; i < quadCount; ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[firstQuad + i]);
        const PreparedQuad& q = m_quads[qIdx];
        Vertex2D* v = &outVertices[i * 4];

        v[0] = Vertex2D{q.cx - q.d0X, q.cy - q.d0Y, q.color, q.u0, q.v0};
        v[1] = Vertex2D{q.cx + q.d1X, q.cy + q.d1Y, q.color, q.u1, q.v0};
        v[2] = Vertex2D{q.cx + q.d0X, q.cy + q.d0Y, q.color, q.u1, q.v1};
        v[3] = Vertex2D{q.cx - q.d1X, q.cy - q.d1Y, q.color, q.u0, q.v1};
    }
}

u64 RenderList::ComputeAuditHash() const noexcept {
    u64 hash = 14695981039346656037ULL;
    HashWords64(hash, m_batches.data(), m_stats.drawBatches * (sizeof(DrawBatch) / sizeof(u64)));
    for (u32 i = 0; i < m_stats.submittedQuads; ++i) {
        const u16 qIdx = SortKey::QuadIndex(m_keys[i]);
        const u64 stateBits = m_keys[i] & ~0xFFFFULL;
        HashWords64(hash, &stateBits, 1);
        HashWords64(hash, &m_quads[qIdx], sizeof(PreparedQuad) / sizeof(u64));
    }
    return hash;
}

std::span<const u16> RenderList::SharedQuadIndices(u32 quadCount) noexcept {
    const u32 clamped = std::min(quadCount, kChunkQuads);
    return {kStaticQuadIndices.data(), clamped * 6};
}

} // namespace en::renderer

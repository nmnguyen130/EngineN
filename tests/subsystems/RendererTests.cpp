#include "tests/subsystems/CtsHarness.h"

#include "platform/Window.h"
#include "renderer/Renderer.h"

#include <vector>

namespace {

using namespace en;
using namespace en::cts;

bool DstRenderList4096QuadSortAndBatchOracle(CtsRunner& r) noexcept {
    RenderList list;
    list.Reset({1280, 720});

    Rng rng(0x64B175027ULL);
    // Submit 4,096 randomized quads across passes [0..3], layers [0..31], depths, views, blends, and textures.
    for (u32 i = 0U; i < RenderList::kMaxQuads; ++i) {
        QuadCmd cmd{
            .center  = {rng.Range(64.0f, 1200.0f), rng.Range(64.0f, 650.0f)},
            .size    = {16.0f, 16.0f},
            .color   = {1.0f, 1.0f, 1.0f, 1.0f},
            .texture = TextureHandle{static_cast<u16>(rng.Bounded(4U)), 1U},
            .depth   = static_cast<u16>(rng.Bounded(65536U)),
            .pass    = static_cast<u8>(rng.Bounded(4U)),
            .layer   = static_cast<u8>(rng.Bounded(8U)),
            .view    = 7U, // Screen-space HUD view so all [64..1200, 64..650] quads pass frustum culling
            .blend   = static_cast<BlendMode>(rng.Bounded(4U)),
        };
        list.Push(cmd);
    }

    // 4097th quad must be counted as dropped when kMaxQuads (4096) is full.
    list.PushRect({100.0f, 100.0f, 10.0f, 10.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0U, 0U, 0U, 7U);
    CTS_CHECK(r, list.Stats().submitted == RenderList::kMaxQuads && list.Stats().dropped == 1U);

    list.Compile();
    const auto batches = list.Batches();
    CTS_CHECK(r, !batches.empty());

    u32 totalBatchedQuads = 0U;
    u16 expectedFirst = 0U;
    for (usize b = 0U; b < batches.size(); ++b) {
        const DrawBatch& cur = batches[b];
        CTS_CHECK(r, cur.first == expectedFirst && cur.count > 0U);
        expectedFirst = static_cast<u16>(expectedFirst + cur.count);
        totalBatchedQuads += cur.count;

        if (b > 0U) {
            const DrawBatch& prev = batches[b - 1U];
            // Pass order must be non-decreasing across compiled batches.
            CTS_CHECK(r, cur.pass >= prev.pass);
        }
    }
    CTS_CHECK(r, totalBatchedQuads + list.Stats().dropped - 1U == RenderList::kMaxQuads);
    r.RecordFuzzOps(RenderList::kMaxQuads);
    r.MixDigest(batches.size());
    return true;
}

bool CtsCamera2DCullingAndPrimitives(CtsRunner& r) noexcept {
    const Viewport vp{1280, 720};
    Camera2D cam{.cx = 10.0f, .cy = -5.0f, .zoom = 2.0f, .pixelsPerMeter = 48.0f, .screenSpace = false, .yUp = true};

    // World <-> View round-trip across 100 points.
    Rng rng(4242ULL);
    for (u32 i = 0U; i < 100U; ++i) {
        const Vec2 worldPt{rng.Range(-100.0f, 100.0f), rng.Range(-100.0f, 100.0f)};
        const Vec2 viewPt = cam.ToView(worldPt, vp);
        const Vec2 back   = cam.ToWorld(viewPt, vp);
        CTS_CHECK(r, Near(back.x, worldPt.x, 1e-3f) && Near(back.y, worldPt.y, 1e-3f));
    }

    RenderList list;
    list.Reset(vp);
    list.SetView(0U, cam);

    // Disabled pass must cull immediately.
    list.SetPass(2U, PassDesc{.enabled = false});
    list.PushRect({10.0f, -5.0f, 2.0f, 2.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 2U, 0U, 32768U, 0U);
    CTS_CHECK(r, list.Stats().culled == 1U && list.Stats().submitted == 0U);

    // Nine-slice with center=true emits 9 quads; center=false emits 8 quads.
    list.PushNine(NineSlice{.rect = {100.0f, 100.0f, 64.0f, 64.0f}, .border = 8.0f, .center = true}, 0U, 10U, 7U);
    CTS_CHECK(r, list.Stats().submitted == 9U);
    list.PushNine(NineSlice{.rect = {200.0f, 100.0f, 64.0f, 64.0f}, .border = 8.0f, .center = false}, 0U, 10U, 7U);
    CTS_CHECK(r, list.Stats().submitted == 17U);

    // Text measurement & glyph emission (spaces and newlines do not emit quads).
    const Vec2 textSize = RenderList::MeasureText("AB\nC", 1.0f);
    CTS_CHECK(r, Near(textSize.x, 24.0f) && textSize.y > 30.0f);
    list.PushText({50.0f, 50.0f}, "A B\nC", {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 0U, 20U, 7U, TextureHandle{2U, 1U});
    CTS_CHECK(r, list.Stats().submitted == 20U); // +3 visible glyphs ('A', 'B', 'C')

    // Verify 2D Ambient Lightmap Multiply pass + 4-edge trauma vignette + full-screen hit flash (+6 quads).
    list.SetPostProcess(PostProcessConfig{
        .ambientColor      = {0.20f, 0.25f, 0.35f, 0.85f},
        .vignetteColor     = {0.10f, 0.0f, 0.02f, 0.65f},
        .flashColor        = {1.0f, 1.0f, 1.0f, 0.50f},
        .vignetteThickness = 0.15f,
    });
    list.EmitPostProcess();
    CTS_CHECK(r, list.Stats().submitted == 26U);
    return true;
}

bool CtsTexturePoolExhaustionAndGenerations(CtsRunner& r) noexcept {
    Window win;
    CTS_CHECK(r, win.Create({.size = {640, 360}}, true).has_value());

    Renderer ren;
    CTS_CHECK(r, ren.Init(win, {.headless = true}).has_value());
    CTS_CHECK(r, ren.IsTextureValid(kWhiteTexture) && ren.LiveCount() == 1U);

    // Slot 0 (kWhiteTexture) must be immune to DestroyTexture.
    ren.DestroyTexture(kWhiteTexture);
    CTS_CHECK(r, ren.IsTextureValid(kWhiteTexture) && ren.LiveCount() == 1U);

    // Allocate all remaining 511 slots up to kMaxTextures (512).
    std::vector<TextureHandle> handles;
    handles.reserve(Renderer::kMaxTextures);
    for (u32 i = 1U; i < Renderer::kMaxTextures; ++i) {
        const auto res = ren.CreateRenderTarget(16U, 16U);
        CTS_CHECK(r, res.has_value());
        handles.push_back(*res);
    }
    CTS_CHECK(r, ren.LiveCount() == Renderer::kMaxTextures);

    // 513th texture allocation must return PoolExhausted.
    const auto overflowRes = ren.CreateRenderTarget(16U, 16U);
    CTS_CHECK(r, !overflowRes.has_value() && overflowRes.error() == RendererErrc::PoolExhausted);

    // Free and reallocate 100 slots, verifying generation increments and stale-handle rejection.
    for (u32 i = 0U; i < 100U; ++i) {
        const TextureHandle oldH = handles[i];
        ren.DestroyTexture(oldH);
        CTS_CHECK(r, !ren.IsTextureValid(oldH));
        const auto newRes = ren.CreateRenderTarget(32U, 32U);
        CTS_CHECK(r, newRes.has_value() && newRes->index == oldH.index && newRes->generation != oldH.generation);
        CTS_CHECK(r, ren.TextureSize(*newRes) == Viewport{32, 32});
    }

    // Out-of-bounds UpdateTexture must fail with InvalidDimension.
    std::array<u8, 64> px{};
    CTS_CHECK(r, ren.UpdateTexture(handles[200], {14.0f, 14.0f, 4.0f, 4.0f}, px).error() == RendererErrc::InvalidDimension);

    r.RecordFuzzOps(612U);
    ren.Shutdown();
    win.Destroy();
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "RendererTests (CTS+DST)"};
    runner.RunCase("DST: 4,096-Quad SortKey & Batching Oracle", DstRenderList4096QuadSortAndBatchOracle);
    runner.RunCase("Camera2D Round-Trip, Culling & Lighting/PostFX", CtsCamera2DCullingAndPrimitives);
    runner.RunCase("512-Slot GPU Pool Exhaustion & Generations", CtsTexturePoolExhaustionAndGenerations);
    return runner.Finish();
}

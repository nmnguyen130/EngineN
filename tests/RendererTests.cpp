#include "renderer/Renderer.h"
#include "platform/Window.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace en;
using namespace en::renderer;

struct CustomExtent {
    i32 w{0};
    i32 h{0};
};

void TestLayer1TypesAndZeroPadding() noexcept {
    static_assert(sizeof(ViewportExtent) == 8 && alignof(ViewportExtent) == 8);
    static_assert(sizeof(TextureHandle) == 4 && alignof(TextureHandle) == 4);
    static_assert(sizeof(Color) == 16 && alignof(Color) == 4);
    static_assert(sizeof(FRect) == 16 && alignof(FRect) == 4);
    static_assert(sizeof(Vertex2D) == 32 && alignof(Vertex2D) == 16);
    static_assert(sizeof(DrawBatch) == 16 && alignof(DrawBatch) == 16);
    static_assert(sizeof(Camera2D) == 48 && alignof(Camera2D) == 16);
    static_assert(sizeof(RenderPassDesc) == 20 && alignof(RenderPassDesc) == 4);
    static_assert(sizeof(RenderStats) == 24 && alignof(RenderStats) == 4);
    static_assert(sizeof(RenderList::PreparedQuad) == 64 && alignof(RenderList::PreparedQuad) == 64);

    const CustomExtent ext{1920, 1080};
    const ViewportExtent vp = ext;
    assert(vp.w == 1920 && vp.h == 1080 && vp.IsValid());

    const Color c = Color::FromRGBA8(0xFF800080u);
    assert(std::abs(c.a - (128.0f / 255.0f)) < 1e-5f);
    assert(std::abs(c.r - c.a) < 1e-5f);
}

void TestMonotonicFastPathAndFrustumCulling() noexcept {
    auto listPtr = std::make_unique<RenderList>();
    RenderList& list = *listPtr;
    list.Reset({1280, 720});

    assert(list.ClearPassMask() == 0x0001u);

    list.PushRect({100.0f, 200.0f, 50.0f, 40.0f}, Colors::Red);
    list.PushRect({300.0f, 400.0f, 60.0f, 30.0f}, Colors::Green);

    // Off-screen quads must be culled before allocation.
    list.PushRect({-200.0f, 100.0f, 50.0f, 50.0f}, Colors::Blue);
    list.PushRect({1500.0f, 100.0f, 50.0f, 50.0f}, Colors::Blue);
    list.PushRect({100.0f, -200.0f, 50.0f, 50.0f}, Colors::Blue);
    list.PushRect({100.0f, 900.0f, 50.0f, 50.0f}, Colors::Blue);

    // Disabled pass quads must be ignored immediately via O(1) bitmask test.
    list.PushRect({100.0f, 100.0f, 20.0f, 20.0f}, Colors::White, 3);

    list.Compile();

    const RenderStats& stats = list.Stats();
    assert(stats.submittedQuads == 2);
    assert(stats.culledQuads == 4);
    assert(stats.droppedQuads == 0);
    assert(stats.drawBatches == 1);
    assert(stats.radixPassesRun == 0);
    assert(stats.monotonicFastPath);

    const auto batches = list.Batches();
    assert(batches.size() == 1);
    assert(batches[0].firstQuad == 0);
    assert(batches[0].quadCount == 2);
    assert(batches[0].texture == kWhiteTexture);
}

void TestRadixSortAndPassSkipping() noexcept {
    auto listPtr = std::make_unique<RenderList>();
    RenderList& list = *listPtr;
    list.Reset({1280, 720});

    list.SetPass(1, RenderPassDesc{.clearColor = Colors::Black, .clear = false, .enabled = true});
    assert(list.ClearPassMask() == 0x0001u);

    // Only TextureIdx low byte (byte 2) and Depth low nibble (byte 4) vary -> 2 of 6 radix passes execute.
    list.PushQuad(QuadCmd{
        .x = 100.0f, .y = 100.0f, .w = 32.0f, .h = 32.0f,
        .texture = {5, 1}, .depth = 14, .passId = 0, .layer = 2
    });
    list.PushQuad(QuadCmd{
        .x = 120.0f, .y = 100.0f, .w = 32.0f, .h = 32.0f,
        .texture = {2, 1}, .depth = 10, .passId = 0, .layer = 2
    });
    list.PushQuad(QuadCmd{
        .x = 140.0f, .y = 100.0f, .w = 32.0f, .h = 32.0f,
        .texture = {2, 1}, .depth = 10, .passId = 0, .layer = 2
    });

    list.Compile();

    const RenderStats& stats = list.Stats();
    assert(!stats.monotonicFastPath);
    assert(stats.submittedQuads == 3);
    assert(stats.radixPassesRun == 2);
    assert(stats.drawBatches == 2);

    const auto batches = list.Batches();
    assert(batches[0].texture == (TextureHandle{2, 1}));
    assert(batches[0].quadCount == 2);
    assert(batches[1].texture == (TextureHandle{5, 1}));
    assert(batches[1].quadCount == 1);
}

void TestMixedGenerationCoalescingAndVertexExpansion() noexcept {
    auto listPtr = std::make_unique<RenderList>();
    RenderList& list = *listPtr;
    list.Reset({800, 600});

    // Same texture slot (3) with different generations (1 vs 2) must not coalesce into one batch.
    list.PushQuad(QuadCmd{
        .x = 200.0f, .y = 150.0f, .w = 40.0f, .h = 20.0f,
        .texture = {3, 1}
    });
    list.PushQuad(QuadCmd{
        .x = 250.0f, .y = 150.0f, .w = 40.0f, .h = 20.0f,
        .texture = {3, 2}
    });

    list.Compile();
    assert(list.Batches().size() == 2);
    assert(list.Batches()[0].texture == (TextureHandle{3, 1}));
    assert(list.Batches()[1].texture == (TextureHandle{3, 2}));

    std::array<Vertex2D, 8> verts{};
    list.ExpandVertices(0, 2, verts);

    assert(std::abs(verts[0].x - 180.0f) < 1e-4f && std::abs(verts[0].y - 140.0f) < 1e-4f);
    assert(std::abs(verts[1].x - 220.0f) < 1e-4f && std::abs(verts[1].y - 140.0f) < 1e-4f);
    assert(std::abs(verts[2].x - 220.0f) < 1e-4f && std::abs(verts[2].y - 160.0f) < 1e-4f);
    assert(std::abs(verts[3].x - 180.0f) < 1e-4f && std::abs(verts[3].y - 160.0f) < 1e-4f);

    const u64 h1 = list.ComputeAuditHash();
    const u64 h2 = list.ComputeAuditHash();
    assert(h1 != 0 && h1 == h2);
}

void TestRendererLayer2LifecycleAndPool() noexcept {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_Init(SDL_INIT_VIDEO);

    platform::Window win{};
    assert(win.Create({.title = "TestWin", .size = {800, 600}}).has_value());

    auto renPtr = std::make_unique<Renderer>();
    Renderer& ren = *renPtr;

    auto initRes = ren.Init(win);
    assert(initRes.has_value());
    assert(ren.IsValid());
    assert(ren.IsTextureValid(kWhiteTexture));

    std::vector<u8> pixels(16 * 16 * 4, 255);
    auto t1Res = ren.CreateTexture(TextureDesc{
        .width = 16, .height = 16, .pixels = pixels
    });
    assert(t1Res.has_value());
    const TextureHandle t1 = *t1Res;
    assert(t1.index == 1 && t1.generation == 1);
    assert(ren.IsTextureValid(t1));

    // Reject undersized pixel span on update (bounds safety).
    std::vector<u8> shortPixels(8, 128);
    auto badUpdate = ren.UpdateTexture(t1, {0.0f, 0.0f, 8.0f, 8.0f}, shortPixels);
    assert(!badUpdate.has_value() && badUpdate.error() == RendererErrc::TextureUpdateFailed);

    // Reject out-of-bounds region on update.
    auto oobUpdate = ren.UpdateTexture(t1, {12.0f, 12.0f, 8.0f, 8.0f}, pixels);
    assert(!oobUpdate.has_value() && oobUpdate.error() == RendererErrc::InvalidDimension);

    // Valid sub-region update.
    auto okUpdate = ren.UpdateTexture(t1, {4.0f, 4.0f, 8.0f, 8.0f}, pixels);
    assert(okUpdate.has_value());

    // Destroy and verify lazy freelist slot reuse with bumped generation.
    ren.DestroyTexture(t1);
    assert(!ren.IsTextureValid(t1));

    auto t2Res = ren.CreateTexture(TextureDesc{.width = 8, .height = 8});
    assert(t2Res.has_value());
    const TextureHandle t2 = *t2Res;
    assert(t2.index == 1 && t2.generation > t1.generation);
    assert(ren.IsTextureValid(t2));

    ren.DestroyTexture(t2);
    assert(!ren.IsTextureValid(t2));

    RenderList& list = ren.BeginFrame(win.State().pixelSize);
    list.PushLine(10.0f, 10.0f, 110.0f, 10.0f, 4.0f, Colors::Red);
    ren.Submit();
    assert(ren.LastFrameStats().submittedQuads == 1);

    ren.Shutdown();
    assert(!ren.IsValid());

    win.Destroy();
    SDL_Quit();
}

} // namespace

int main() {
    TestLayer1TypesAndZeroPadding();
    TestMonotonicFastPathAndFrustumCulling();
    TestRadixSortAndPassSkipping();
    TestMixedGenerationCoalescingAndVertexExpansion();
    TestRendererLayer2LifecycleAndPool();
    std::printf("All Phase 4 Renderer tests passed (sizeof(RenderList) = %zu bytes).\n", sizeof(RenderList));
    return 0;
}

#include "renderer/Renderer.h"
#include "platform/Window.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace en;

bool TestRenderListSortingAndBatching() noexcept {
    RenderList list;
    list.Reset({1280, 720});

    // Submit out-of-order layers and verify SortKey sorts and merges identical state.
    list.PushRect({100.0f, 100.0f, 32.0f, 32.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, 0U, 20U, 32768U, 7U);
    list.PushRect({200.0f, 100.0f, 32.0f, 32.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, 0U, 10U, 32768U, 7U);
    list.PushRect({300.0f, 100.0f, 32.0f, 32.0f}, {0.0f, 0.0f, 1.0f, 1.0f}, 0U, 10U, 32768U, 7U);

    // Off-screen quad should be frustum-culled.
    list.PushRect({5000.0f, 5000.0f, 16.0f, 16.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U, 32768U, 7U);

    list.Compile();
    if (list.Stats().submitted != 3U || list.Stats().culled != 1U) return false;
    // All 3 visible quads share Pass 0, View 7, Blend Alpha, Texture kWhiteTexture -> 1 merged DrawBatch!
    if (list.Batches().size() != 1U || list.Batches()[0].count != 3U) return false;

    std::array<Vertex2D, 12> verts{};
    list.Expand(0U, 3U, verts);
    // Layer 10 quads must appear before Layer 20 quad after sorting.
    if (verts[0].g != 1.0f || verts[8].r != 1.0f) return false;

    return true;
}

bool TestRendererTexturePool() noexcept {
    Window win;
    if (!win.Create({.size = {640, 360}}, true)) return false;

    Renderer ren;
    if (!ren.Init(win, {.headless = true}) || !ren.IsValid()) return false;
    if (!ren.IsTextureValid(kWhiteTexture) || ren.LiveCount() != 1U) return false;

    std::array<u8, 16> px{};
    px.fill(255U);
    const auto texRes = ren.CreateTexture({.pixels = px, .width = 2U, .height = 2U});
    if (!texRes || !ren.IsTextureValid(*texRes) || ren.TextureSize(*texRes) != Viewport{2, 2}) {
        return false;
    }

    const TextureHandle oldHandle = *texRes;
    ren.DestroyTexture(oldHandle);
    if (ren.IsTextureValid(oldHandle)) return false;

    // Reallocating the slot increments generation, invalidating oldHandle.
    const auto texRes2 = ren.CreateTexture({.pixels = px, .width = 2U, .height = 2U});
    if (!texRes2 || texRes2->index != oldHandle.index || texRes2->generation == oldHandle.generation) {
        return false;
    }

    RenderList list;
    list.Reset({640, 360});
    list.PushOutline({10.0f, 10.0f, 50.0f, 50.0f}, 2.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U, 32768U, 7U);
    ren.Submit(list);
    if (ren.Stats().submitted != 4U) return false;

    ren.Shutdown();
    win.Destroy();
    return true;
}

} // namespace

int main() {
    if (!TestRenderListSortingAndBatching()) return 1;
    if (!TestRendererTexturePool()) return 2;
    std::printf("RendererTests: ALL PASSED\n");
    return 0;
}

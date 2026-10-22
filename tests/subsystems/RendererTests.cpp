#include "renderer/Renderer.h"
#include "platform/Window.h"
#include <cmath>
#include <cstdio>
#include <memory>

namespace en {

namespace {

[[nodiscard]] consteval bool VerifyConstexprRendererLayer1() noexcept {
    // 1. Struct byte-size & alignment invariants (0B hidden compiler padding, 0 aliases)
    static_assert(sizeof(ViewportExtent) == 8 && alignof(ViewportExtent) == 8);
    static_assert(sizeof(TextureHandle) == 4 && alignof(TextureHandle) == 4);
    static_assert(sizeof(TextureDesc) == 32 && alignof(TextureDesc) == 8);
    static_assert(sizeof(Vertex2D) == 32 && alignof(Vertex2D) == 16);
    static_assert(sizeof(QuadCmd) == 80 && alignof(QuadCmd) == 16);
    static_assert(sizeof(PreparedQuad) == 64 && alignof(PreparedQuad) == 64);
    static_assert(sizeof(DrawBatch) == 16 && alignof(DrawBatch) == 16);
    static_assert(sizeof(UnitScaleQ8) == 2 && alignof(UnitScaleQ8) == 2);
    static_assert(sizeof(Camera2D) == 48 && alignof(Camera2D) == 16);
    static_assert(sizeof(RenderPassDesc) == 20 && alignof(RenderPassDesc) == 4);
    static_assert(sizeof(RenderStats) == 20 && alignof(RenderStats) == 4);
    static_assert(sizeof(NineSliceDesc) == 64 && alignof(NineSliceDesc) == 64);
    static_assert(sizeof(ShadowOccluder2D) == 16 && alignof(ShadowOccluder2D) == 16);
    static_assert(sizeof(Renderer::TextureSlot) == 16 && alignof(Renderer::TextureSlot) == 16);
    static_assert(sizeof(RenderList) == 1328256 && alignof(RenderList) == 64);
    static_assert(sizeof(Renderer) == 1393856 && alignof(Renderer) == 64);

    // 2. ViewportExtent implicit structural conversion from Extent2i
    constexpr Extent2i platExt{1920, 1080};
    constexpr ViewportExtent vp = platExt;
    if (!vp.IsValid() || vp.w != 1920 || vp.h != 1080) return false;
    if (ViewportExtent{0, 720}.IsValid()) return false;

    // 3. SortKey bit-packing, depth bias clamping, and Y-sort depth quantization
    QuadCmd cmd{};
    cmd.passId = 3U;
    cmd.layer = 42U;
    cmd.depth = 1000U;
    cmd.depthBias = -100;
    cmd.blend = BlendMode::Additive;
    cmd.viewSlot = 5U;
    cmd.texture = TextureHandle{123U, 2U};
    cmd.SetHitFlash(0.5f);
    if (cmd.flash8 != 128U) return false;

    const u64 key = SortKey::Pack(cmd, 77U);
    if (SortKey::PassId(key) != 3U) return false;
    if (SortKey::Layer(key) != 42U) return false;
    if (SortKey::Depth(key) != 900U) return false;
    if (SortKey::Blend(key) != BlendMode::Additive) return false;
    if (SortKey::ViewSlot(key) != 5U) return false;
    if (SortKey::TextureIndex(key) != 123U) return false;
    if (SortKey::QuadIndex(key) != 77U) return false;

    const u16 yLow = SortKey::EncodeYDepth(-100.0f, -500.0f, 500.0f);
    const u16 yHigh = SortKey::EncodeYDepth(100.0f, -500.0f, 500.0f);
    if (yLow >= yHigh) return false;

    // 4. Camera2D WorldToViewport, ViewportToWorld & Trauma Shake
    Camera2D cam{};
    cam.worldCenterX = 10.0f;
    cam.worldCenterY = -5.0f;
    cam.zoom = 2.0f;
    cam.pixelsPerMeter = 48.0f;
    cam.yUp = true;

    const Vec2 worldPt{12.5f, -3.0f};
    const Vec2 vpPt = cam.WorldToViewport(worldPt, vp);
    const Vec2 roundTrip = cam.ViewportToWorld(vpPt, vp);
    if (std::abs(roundTrip.x - worldPt.x) > 1e-4f || std::abs(roundTrip.y - worldPt.y) > 1e-4f) {
        return false;
    }

    cam.ApplyTraumaShake(0.8f, 1.25f, 0.5f);
    if (cam.worldCenterX == 10.0f && cam.worldCenterY == -5.0f) return false;

    // 5. MeasureText constexpr layout calculation
    const Vec2 textSize = RenderList::MeasureText("AB\nCDE", 2.0f);
    if (std::abs(textSize.x - 72.0f) > 1e-4f || textSize.y <= 32.0f) return false;

    return true;
}

static_assert(VerifyConstexprRendererLayer1());

} // namespace

} // namespace en

int main() {
    using namespace en;

    u32 passed = 0U;
    u32 failed = 0U;
    auto check = [&](bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr, "[FAIL] Line %d: %s\n", line, expr);
        }
    };
#define EN_CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

    auto listA = std::make_unique<RenderList>();
    auto listB = std::make_unique<RenderList>();
    auto ren = std::make_unique<Renderer>();

    // 1. Monotonic Fast-Path (0 radix passes when submitted in sorted order)
    listA->Reset({1280, 720});
    Camera2D screenCam{};
    screenCam.screenSpace = true;
    listA->SetView(0U, screenCam);

    listA->PushRect({10.0f, 10.0f, 32.0f, 32.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 5U, 100U, 0U);
    listA->PushRect({50.0f, 10.0f, 32.0f, 32.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 5U, 200U, 0U);
    listA->PushRect({90.0f, 10.0f, 32.0f, 32.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U, 100U, 0U);
    listA->Compile();

    EN_CHECK(listA->Stats().submittedQuads == 3U);
    EN_CHECK(listA->Stats().monotonicFastPath);
    EN_CHECK(listA->Stats().radixPassesRun == 0U);
    EN_CHECK(listA->Stats().drawBatches == 1U);

    // 2. Single-Pass Active-Byte LSD Radix Sort & State-Identical Batch Coalescing
    listA->Reset({1280, 720});
    listA->SetView(0U, screenCam);

    QuadCmd q0{};
    q0.center = {100.0f, 100.0f};
    q0.size = {20.0f, 20.0f};
    q0.passId = 1U;
    q0.layer = 20U;
    q0.depth = 5000U;
    q0.texture = TextureHandle{5U, 1U};
    listA->PushQuad(q0);

    QuadCmd q1 = q0;
    q1.passId = 0U;
    q1.layer = 10U;
    q1.depth = 2000U;
    q1.texture = TextureHandle{2U, 1U};
    listA->PushQuad(q1);

    QuadCmd q2 = q1;
    q2.center = {140.0f, 100.0f};
    q2.depth = 1000U;
    listA->PushQuad(q2);

    listA->Compile();
    EN_CHECK(!listA->Stats().monotonicFastPath);
    EN_CHECK(listA->Stats().radixPassesRun > 0U && listA->Stats().radixPassesRun <= 6U);
    EN_CHECK(listA->Stats().drawBatches == 2U);

    const auto sortedKeys = listA->SortedKeys();
    EN_CHECK(sortedKeys.size() == 3U);
    EN_CHECK(sortedKeys[0] < sortedKeys[1] && sortedKeys[1] < sortedKeys[2]);
    EN_CHECK(SortKey::QuadIndex(sortedKeys[0]) == 2U);
    EN_CHECK(SortKey::QuadIndex(sortedKeys[1]) == 1U);
    EN_CHECK(SortKey::QuadIndex(sortedKeys[2]) == 0U);

    const auto batches = listA->Batches();
    EN_CHECK(batches.size() == 2U);
    EN_CHECK(batches[0].firstQuad == 0U && batches[0].quadCount == 2U && batches[0].texture.index == 2U);
    EN_CHECK(batches[1].firstQuad == 2U && batches[1].quadCount == 1U && batches[1].texture.index == 5U);

    // 3. Hierarchical Scissor Clip Stack (PushClipRect / PopClipRect) & Frustum Culling
    listA->Reset({1280, 720});
    const u8 outerView = listA->PushClipRect({100.0f, 100.0f, 400.0f, 400.0f});
    const u8 innerView = listA->PushClipRect({200.0f, 200.0f, 500.0f, 500.0f}); // Intersects to [200..500, 200..500]
    EN_CHECK(outerView == 8U && innerView == 9U);
    EN_CHECK(listA->ActiveClipViewSlot() == 9U);

    // Quad at (150, 150) is inside outerView [100..500] but OUTSIDE innerView [200..500] -> culled on innerView!
    listA->PushRect({140.0f, 140.0f, 20.0f, 20.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U, 32768U, innerView);
    EN_CHECK(listA->Stats().submittedQuads == 0U && listA->Stats().culledQuads == 1U);

    listA->PopClipRect();
    EN_CHECK(listA->ActiveClipViewSlot() == outerView);
    // Same quad at (150, 150) on outerView -> kept!
    listA->PushRect({140.0f, 140.0f, 20.0f, 20.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U, 32768U, listA->ActiveClipViewSlot());
    EN_CHECK(listA->Stats().submittedQuads == 1U);
    listA->PopClipRect();
    EN_CHECK(listA->ActiveClipViewSlot() == RenderList::kHudViewSlot);

    // 4. World-Space yUp Bottom-Center Feet Pivot {0.5f, 0.0f} & Pixel-Art Snapping
    listA->Reset({1280, 720});
    Camera2D worldCam{};
    worldCam.worldCenterX = 0.0f;
    worldCam.worldCenterY = 0.0f;
    worldCam.pixelsPerMeter = 50.0f;
    worldCam.yUp = true;
    listA->SetView(0U, worldCam);
    listA->SetPixelSnap(true);

    // Character standing at world origin (0, 0) with size (1m, 2m) = (50px, 100px) and feet pivot {0.5f, 0.0f}
    // Screen center is (640, 360); since yUp is true, feet are at y = 360 and head is at y = 260 (center at y = 310)!
    QuadCmd heroCmd{};
    heroCmd.center = {0.0f, 0.0f};
    heroCmd.size = {1.0f, 2.0f};
    heroCmd.pivot = {0.5f, 0.0f};
    listA->PushQuad(heroCmd);
    listA->Compile();

    std::array<Vertex2D, 4> verts{};
    listA->ExpandVertices(0U, 1U, verts);
    EN_CHECK(std::abs(verts[0].x - 615.0f) < 1e-4f && std::abs(verts[0].y - 260.0f) < 1e-4f);
    EN_CHECK(std::abs(verts[2].x - 665.0f) < 1e-4f && std::abs(verts[2].y - 360.0f) < 1e-4f);

    // 5. Gouraud 2-Color Gradient Rect, Tiled Rect, Hit-Flash & PointLight
    listA->Reset({1280, 720});
    listA->SetView(0U, screenCam);
    listA->PushGradientRect(
        {0.0f, 0.0f, 100.0f, 50.0f},
        Color{1.0f, 0.0f, 0.0f, 1.0f},
        Color{0.0f, 0.0f, 1.0f, 1.0f},
        true);
    listA->Compile();
    listA->ExpandVertices(0U, 1U, verts);
    EN_CHECK(verts[0].color.r > 0.9f && verts[0].color.b < 0.1f); // Top edge red
    EN_CHECK(verts[2].color.b > 0.9f && verts[2].color.r < 0.1f); // Bottom edge blue

    listA->Reset({1280, 720});
    listA->SetView(0U, screenCam);
    listA->PushTiledRect({0.0f, 0.0f, 100.0f, 64.0f}, kWhiteTexture, {32.0f, 32.0f});
    // 100x64 tiled by 32x32 -> 4 columns (32, 32, 32, 4) x 2 rows (32, 32) = 8 quads!
    EN_CHECK(listA->Stats().submittedQuads == 8U);

    listA->PushHitFlashQuad(q1, 0.75f);
    EN_CHECK(listA->Stats().submittedQuads == 10U); // Base sprite + additive hit-flash overlay

    listA->PushPointLight({300.0f, 300.0f}, 64.0f, Color{1.0f, 0.8f, 0.4f, 1.0f}, 1.0f);
    EN_CHECK(listA->Stats().submittedQuads == 12U); // Outer + inner light halo

    // 6. True 3x3 9-Slice Patch Generation & Proportional Border Clamping
    listA->Reset({1280, 720});
    listA->SetView(0U, screenCam);

    NineSliceDesc ns{};
    ns.outerRect = {50.0f, 50.0f, 200.0f, 100.0f};
    ns.drawCenter = true;
    listA->PushNineSlice(ns, 0U, 20U, 0U);
    EN_CHECK(listA->Stats().submittedQuads == 9U);

    ns.drawCenter = false;
    listA->PushNineSlice(ns, 0U, 20U, 0U);
    EN_CHECK(listA->Stats().submittedQuads == 17U);

    ns.outerRect = {50.0f, 50.0f, 10.0f, 10.0f};
    ns.drawCenter = true;
    listA->PushNineSlice(ns, 0U, 20U, 0U);
    EN_CHECK(listA->Stats().submittedQuads == 21U);

    // 7. Procedural Geometry, Text, Shadow Hull & Deterministic Audit Hash
    listA->Reset({1280, 720});
    listA->SetView(0U, screenCam);
    listA->PushRectOutline({20.0f, 20.0f, 100.0f, 60.0f}, 2.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    const std::array<Vec2, 3> polyPts{{{30.0f, 30.0f}, {80.0f, 30.0f}, {55.0f, 70.0f}}};
    listA->PushPolyline(polyPts, 2.0f, Color{1.0f, 0.5f, 0.2f, 1.0f}, true);
    listA->PushCircleOutline({200.0f, 200.0f}, 30.0f, 12U, 2.0f, Color{0.2f, 0.8f, 1.0f, 1.0f});
    listA->PushSlashArc({300.0f, 200.0f}, 40.0f, 0.0f, 1.5f, 4.0f, Color{1.0f, 0.9f, 0.3f, 1.0f});
    listA->PushBeam({100.0f, 300.0f}, {250.0f, 300.0f}, 4.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{0.2f, 0.6f, 1.0f, 0.5f});
    listA->PushProgressBar({50.0f, 400.0f, 160.0f, 16.0f}, 0.75f, Color{0.2f, 0.9f, 0.3f, 1.0f}, Color{0.1f, 0.1f, 0.1f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f});
    listA->PushText({50.0f, 440.0f}, "HP: 100/100", Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 0U, 30U, 0U);
    listA->Compile();

    listB->Reset({1280, 720});
    listB->SetView(0U, screenCam);
    listB->PushRectOutline({20.0f, 20.0f, 100.0f, 60.0f}, 2.0f, Color{1.0f, 1.0f, 1.0f, 1.0f});
    listB->PushPolyline(polyPts, 2.0f, Color{1.0f, 0.5f, 0.2f, 1.0f}, true);
    listB->PushCircleOutline({200.0f, 200.0f}, 30.0f, 12U, 2.0f, Color{0.2f, 0.8f, 1.0f, 1.0f});
    listB->PushSlashArc({300.0f, 200.0f}, 40.0f, 0.0f, 1.5f, 4.0f, Color{1.0f, 0.9f, 0.3f, 1.0f});
    listB->PushBeam({100.0f, 300.0f}, {250.0f, 300.0f}, 4.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{0.2f, 0.6f, 1.0f, 0.5f});
    listB->PushProgressBar({50.0f, 400.0f, 160.0f, 16.0f}, 0.75f, Color{0.2f, 0.9f, 0.3f, 1.0f}, Color{0.1f, 0.1f, 0.1f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f});
    listB->PushText({50.0f, 440.0f}, "HP: 100/100", Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 0U, 30U, 0U);
    listB->Compile();
    EN_CHECK(listA->ComputeAuditHash() == listB->ComputeAuditHash());

    const std::array<ShadowOccluder2D, 1> occluders{{{Vec2{5.0f, -5.0f}, Vec2{5.0f, 5.0f}}}};
    std::array<Vec2, 16> shadowPoly{};
    const u32 vCount = RenderList::ComputeShadowVisibilityPolygon({0.0f, 0.0f}, 10.0f, occluders, shadowPoly);
    EN_CHECK(vCount == 16U && std::abs(shadowPoly[0].x - 5.0f) < 1e-3f && std::abs(shadowPoly[8].x - (-10.0f)) < 1e-3f);

    // 8. Layer 2 Renderer: Offscreen RenderTarget FBOs, Intrusive Freelist, ABA Protection & Multi-Pass Submit
    Window win{};
    (void)win.Create(WindowConfig{}, true);

    RendererConfig rcfg{};
    rcfg.headless = false;
    EN_CHECK(ren->Init(win, rcfg).has_value());
    EN_CHECK(ren->IsValid());
    EN_CHECK(ren->IsTextureValid(kWhiteTexture));
    EN_CHECK(ren->IsTextureValid(kBuiltinFontTexture));
    EN_CHECK(ren->IsTextureValid(kBuiltinSoftLightTexture));
    EN_CHECK(ren->HighWaterTextureSlots() == 3U && ren->LiveTextureCount() == 3U);

    // Create offscreen lightmap FBO render target -> Slot 3
    const auto fboRes = ren->CreateRenderTarget(640U, 360U);
    EN_CHECK(fboRes.has_value());
    const TextureHandle lightmapFbo = fboRes.value();
    EN_CHECK(lightmapFbo.index == 3U && ren->LiveTextureCount() == 4U);

    // Create custom 4x4 texture -> Slot 4
    std::array<u8, 4U * 4U * 4U> rgba4x4{};
    rgba4x4.fill(200U);
    TextureDesc tDesc{};
    tDesc.width = 4U;
    tDesc.height = 4U;
    tDesc.pixels = rgba4x4;
    tDesc.filter = TextureFilter::PixelArt;

    const auto tex1Res = ren->CreateTexture(tDesc);
    EN_CHECK(tex1Res.has_value());
    const TextureHandle h1 = tex1Res.value();
    EN_CHECK(h1.index == 4U && h1.generation == 1U);
    EN_CHECK(ren->HighWaterTextureSlots() == 5U && ren->LiveTextureCount() == 5U);
    EN_CHECK(ren->UpdateTexture(h1, {0.0f, 0.0f, 2.0f, 2.0f}, rgba4x4).has_value());

    // Destroy h1 -> pushes Slot 4 onto intrusive freelist
    ren->DestroyTexture(h1);
    EN_CHECK(!ren->IsTextureValid(h1) && ren->LiveTextureCount() == 4U);

    // Create another texture -> reuses Slot 4 with generation 2!
    const auto tex2Res = ren->CreateTexture(tDesc);
    EN_CHECK(tex2Res.has_value());
    const TextureHandle h2 = tex2Res.value();
    EN_CHECK(h2.index == 4U && h2.generation == 2U);
    EN_CHECK(ren->HighWaterTextureSlots() == 5U && ren->LiveTextureCount() == 5U);
    EN_CHECK(!ren->IsTextureValid(h1) && ren->IsTextureValid(h2));

    // Multi-pass lightmap + multiply composite frame submission
    RenderList& frameList = ren->BeginFrame(win.State().pixelSize);
    RenderPassDesc lightPass{};
    lightPass.clearColor = Color{0.12f, 0.14f, 0.25f, 1.0f};
    lightPass.targetTexIndex = lightmapFbo.index;
    lightPass.clear = true;
    frameList.SetPass(1U, lightPass);

    RenderPassDesc compPass{};
    compPass.targetTexIndex = 0U;
    compPass.clear = false;
    frameList.SetPass(2U, compPass);

    frameList.PushRect({0.0f, 0.0f, 64.0f, 64.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0U, 10U);
    frameList.PushPointLight({0.0f, 0.0f}, 5.0f, Color{1.0f, 0.9f, 0.6f, 1.0f}, 1.0f, 1U, 40U);
    QuadCmd lightmapComposite{};
    lightmapComposite.center = {640.0f, 360.0f};
    lightmapComposite.size = {1280.0f, 720.0f};
    lightmapComposite.texture = lightmapFbo;
    lightmapComposite.passId = 2U;
    lightmapComposite.viewSlot = RenderList::kHudViewSlot;
    lightmapComposite.blend = BlendMode::Multiply;
    frameList.PushQuad(lightmapComposite);

    ren->Submit();
    EN_CHECK(ren->LastFrameStats().submittedQuads == 4U);
    EN_CHECK(ren->LastFrameStats().drawBatches == 3U);

    ren->Shutdown();
    win.Destroy();

    std::printf("RendererTests: %u passed, %u failed.\n", passed, failed);
    return failed == 0U ? 0 : 1;
}

#include "renderer/RendererTypes.h"
#include "renderer/Texture.h"
#include "renderer/RenderBackend.h"
#include "renderer/RenderQueue.h"
#include "renderer/PrimitiveBatch.h"
#include "renderer/RenderDevice.h"
#include "renderer/RenderContext.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>

using namespace EngineN;

// ============================================================================
// Allocation Tracking Hook for Zero-Allocation Verification
// ============================================================================

static bool s_trackAllocations = false;
static usize s_allocationCount = 0;

void* operator new(std::size_t size) {
    if (s_trackAllocations) {
        s_allocationCount++;
    }
    return std::malloc(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

// ============================================================================
// Unit & Adversarial Test Functions
// ============================================================================

void TestColorMath() {
    std::cout << "  -> Testing Color Math & Conversions..." << std::endl;

    // Default is White
    Color def{};
    assert(def.r == 1.0f && def.g == 1.0f && def.b == 1.0f && def.a == 1.0f);

    // FromRGBA8
    Color c1 = Color::FromRGBA8(255, 128, 0, 255);
    assert(std::abs(c1.r - 1.0f) < 1e-4f);
    assert(std::abs(c1.g - (128.0f / 255.0f)) < 1e-4f);
    assert(c1.b == 0.0f);
    assert(c1.a == 1.0f);

    // Hex conversions round-trip
    u32 hex = 0xFF8000FF;
    Color fromHex = Color::FromHex(hex);
    u32 toHex = fromHex.ToHex();
    assert(toHex == hex);

    // Memory-packed RGBA8 (little-endian byte 0 is R)
    u32 packed = fromHex.ToPackedRGBA8();
    assert((packed & 0xFF) == 255);
    assert(((packed >> 8) & 0xFF) == 128);
    assert(((packed >> 16) & 0xFF) == 0);
    assert(((packed >> 24) & 0xFF) == 255);

    // Premultiplied alpha
    Color trans{1.0f, 0.5f, 0.0f, 0.5f};
    Color pma = trans.Premultiplied();
    assert(std::abs(pma.r - 0.5f) < 1e-5f);
    assert(std::abs(pma.g - 0.25f) < 1e-5f);
    assert(pma.b == 0.0f);
    assert(pma.a == 0.5f);

    // Unpremultiplied
    Color unp = pma.Unpremultiplied();
    assert(std::abs(unp.r - trans.r) < 1e-5f);
    assert(std::abs(unp.g - trans.g) < 1e-5f);

    // Zero-alpha unpremultiply is safe (no NaN or divide-by-zero)
    Color zeroAlpha{1.0f, 1.0f, 1.0f, 0.0f};
    Color unpZero = zeroAlpha.Unpremultiplied();
    assert(unpZero.a == 0.0f && unpZero.r == 0.0f);

    // NaN / out-of-range clamping in packed conversions
    Color nanCol{std::numeric_limits<float>::quiet_NaN(), -10.0f, 100.0f, 1.0f};
    u32 nanPacked = nanCol.ToPackedRGBA8();
    assert((nanPacked & 0xFF) == 0);         // NaN clamped to 0
    assert(((nanPacked >> 8) & 0xFF) == 0);  // Negative clamped to 0
    assert(((nanPacked >> 16) & 0xFF) == 255); // >1 clamped to 255

    // Lerp
    Color red = Colors::Red;
    Color blue = Colors::Blue;
    Color mid = Lerp(red, blue, 0.5f);
    assert(std::abs(mid.r - 0.5f) < 1e-5f);
    assert(std::abs(mid.b - 0.5f) < 1e-5f);

    std::cout << "     [PASS] Color Math & Conversions" << std::endl;
}

void TestRectGeometry() {
    std::cout << "  -> Testing Rect & FRect Geometry..." << std::endl;

    FRect r1{10.0f, 20.0f, 30.0f, 40.0f};
    assert(!r1.IsEmpty());
    assert(r1.Contains({10.0f, 20.0f}));
    assert(r1.Contains({39.9f, 59.9f}));
    assert(!r1.Contains({40.0f, 60.0f})); // Half-open interval [min, max)

    // Center
    Vec2 center = r1.Center();
    assert(center.x == 25.0f && center.y == 40.0f);

    // Touching edges do NOT overlap
    FRect r2{40.0f, 20.0f, 10.0f, 10.0f};
    assert(!r1.Overlaps(r2));

    // Overlapping
    FRect r3{25.0f, 30.0f, 20.0f, 20.0f};
    assert(r1.Overlaps(r3));
    FRect inter = r1.Intersection(r3);
    assert(inter.x == 25.0f && inter.y == 30.0f && inter.w == 15.0f && inter.h == 20.0f);

    // Non-overlapping intersection returns empty
    FRect disjoint{100.0f, 100.0f, 10.0f, 10.0f};
    FRect noInter = r1.Intersection(disjoint);
    assert(noInter.IsEmpty());

    // Union
    FRect u = r1.Union(r3);
    assert(u.x == 10.0f && u.y == 20.0f && u.w == 35.0f && u.h == 40.0f);

    // Expansion & over-shrink collapse
    FRect exp = r1.Expanded(5.0f);
    assert(exp.x == 5.0f && exp.y == 15.0f && exp.w == 40.0f && exp.h == 50.0f);

    FRect collapsed = r1.Expanded(-100.0f);
    assert(collapsed.IsEmpty());
    assert(collapsed.w == 0.0f && collapsed.h == 0.0f);
    assert(collapsed.x == 25.0f && collapsed.y == 40.0f); // Collapses to center

    // NaN safety
    FRect nanRect{std::numeric_limits<float>::quiet_NaN(), 0.0f, 10.0f, 10.0f};
    assert(nanRect.IsEmpty() || !nanRect.Contains({5.0f, 5.0f}));

    std::cout << "     [PASS] Rect & FRect Geometry" << std::endl;
}

void TestMat2DMath() {
    std::cout << "  -> Testing Mat2D Affine Transforms..." << std::endl;

    Mat2D id = Mat2D::Identity();
    Vec2 p{10.0f, 20.0f};
    Vec2 pTrans = id.TransformPoint(p);
    assert(pTrans.x == p.x && pTrans.y == p.y);

    // Translation
    Mat2D t = Mat2D::Translation(5.0f, -3.0f);
    Vec2 pt = t.TransformPoint(p);
    assert(pt.x == 15.0f && pt.y == 17.0f);

    // Scale
    Mat2D s = Mat2D::Scale(2.0f, 0.5f);
    Vec2 ps = s.TransformPoint(p);
    assert(ps.x == 20.0f && ps.y == 10.0f);

    // Composition order: A * B applies B first, then A
    Mat2D ts = t * s; // First scale, then translate
    Vec2 pts = ts.TransformPoint(p);
    assert(pts.x == 25.0f && pts.y == 7.0f);

    // Rotation (90 degrees counter-clockwise)
    Mat2D r = Mat2D::Rotation(kHalfPi);
    Vec2 unitX{1.0f, 0.0f};
    Vec2 rotated = r.TransformPoint(unitX);
    assert(std::abs(rotated.x) < 1e-5f);
    assert(std::abs(rotated.y - 1.0f) < 1e-5f);

    // Invertibility round-trip
    Mat2D complexM = Mat2D::Translation(100.0f, 50.0f) * Mat2D::Rotation(0.75f) * Mat2D::Scale(1.5f, 2.0f);
    auto inv = complexM.TryInverse();
    assert(inv.has_value());

    Vec2 pRound = inv->TransformPoint(complexM.TransformPoint(p));
    assert(std::abs(pRound.x - p.x) < 1e-3f);
    assert(std::abs(pRound.y - p.y) < 1e-3f);

    // Singular matrix rejection
    Mat2D singular = Mat2D::Scale(0.0f, 1.0f);
    auto invSingular = singular.TryInverse();
    assert(!invSingular.has_value());

    std::cout << "     [PASS] Mat2D Affine Transforms" << std::endl;
}

void TestTransform2DAndLerp() {
    std::cout << "  -> Testing Transform2D & Shortest-Arc Lerp..." << std::endl;

    Transform2D t1{
        .position = {10.0f, 10.0f},
        .rotation = 0.0f,
        .scale = {1.0f, 1.0f},
        .pivot = {0.0f, 0.0f}
    };

    // Pivot invariance: rotating around pivot {5, 5}
    Transform2D tPivot{
        .position = {100.0f, 100.0f},
        .rotation = kHalfPi,
        .scale = {1.0f, 1.0f},
        .pivot = {5.0f, 5.0f}
    };
    Mat2D mPivot = tPivot.ToMatrix();
    // The pivot point in local space must map exactly to tPivot.position in world space
    Vec2 worldPivot = mPivot.TransformPoint(tPivot.pivot);
    assert(std::abs(worldPivot.x - tPivot.position.x) < 1e-4f);
    assert(std::abs(worldPivot.y - tPivot.position.y) < 1e-4f);

    // Shortest-arc rotation Lerp across +/- PI boundary
    Transform2D rotA{ .rotation = kPi - 0.1f };
    Transform2D rotB{ .rotation = -kPi + 0.1f };

    // Distance is 0.2 rad shortest path, midpoint must be near +/- PI
    Transform2D rotMid = Transform2D::Lerp(rotA, rotB, 0.5f);
    f32 diff = std::abs(std::remainder(rotMid.rotation - kPi, kTwoPi));
    assert(diff < 0.05f);

    std::cout << "     [PASS] Transform2D & Shortest-Arc Lerp" << std::endl;
}

void TestViewTransform2D() {
    std::cout << "  -> Testing ViewTransform2D..." << std::endl;

    ViewTransform2D view{
        .viewport = {0.0f, 0.0f, 800.0f, 600.0f},
        .center = {400.0f, 300.0f},
        .zoom = 2.0f,
        .rotation = 0.0f,
        .screenSpace = false
    };

    // Center of camera maps to center of viewport
    Vec2 screenCenter = view.WorldToScreen(view.center);
    assert(std::abs(screenCenter.x - 400.0f) < 1e-4f);
    assert(std::abs(screenCenter.y - 300.0f) < 1e-4f);

    // Unproject round-trip
    Vec2 worldPt{123.4f, 567.8f};
    Vec2 screenPt = view.WorldToScreen(worldPt);
    Vec2 unprojected = view.ScreenToWorld(screenPt);
    assert(std::abs(unprojected.x - worldPt.x) < 1e-3f);
    assert(std::abs(unprojected.y - worldPt.y) < 1e-3f);

    // Screen-space bypass
    view.screenSpace = true;
    Vec2 hudPt{50.0f, 50.0f};
    assert(view.WorldToScreen(hudPt).x == hudPt.x);
    assert(view.WorldToScreen(hudPt).y == hudPt.y);

    std::cout << "     [PASS] ViewTransform2D" << std::endl;
}

void TestSortKeyPackingAndRadixSort() {
    std::cout << "  -> Testing 64-Bit SortKey & Radix Sort Stability..." << std::endl;

    // Field isolation test
    u64 key1 = SortKey::Pack(RenderPass::World, 5, 0.5f, BlendMode::Premultiplied, 2, 10);
    assert(SortKey::UnpackPass(key1) == RenderPass::World);
    assert(SortKey::UnpackLayer(key1) == 5);
    assert(SortKey::UnpackBlend(key1) == BlendMode::Premultiplied);
    assert(SortKey::UnpackMaterial(key1) == 2);
    assert(SortKey::UnpackTextureSlot(key1) == 10);

    // Painter's order strict priority: Pass > Layer > Depth
    u64 uiKey = SortKey::Pack(RenderPass::Ui, 0, 0.0f, BlendMode::Opaque, 0, 0);
    u64 worldKey = SortKey::Pack(RenderPass::World, 10, 0.9f, BlendMode::Opaque, 0, 0);
    assert(uiKey > worldKey); // UI draws over World

    u64 layer1 = SortKey::Pack(RenderPass::World, 1, 0.5f, BlendMode::Opaque, 0, 0);
    u64 layer2 = SortKey::Pack(RenderPass::World, 2, 0.5f, BlendMode::Opaque, 0, 0);
    assert(layer2 > layer1);

    // Oracle Comparison: test Radix Sort against std::stable_sort
    RenderQueue queue;
    auto initRes = queue.Init({
        .maxVertices = 4000,
        .maxIndices = 4000,
        .maxItems = 1000,
        .maxRuns = 1000,
        .maxScissors = 16
    });
    assert(initRes);

    std::mt19937_64 rng(1337);
    std::vector<SortEntry> testEntries(500);
    for (usize i = 0; i < testEntries.size(); ++i) {
        testEntries[i].key = (rng() & ~0xFFULL); // lower 8 bits 0
        testEntries[i].itemIndex = static_cast<u32>(i);
    }

    std::vector<SortEntry> expected = testEntries;
    std::stable_sort(expected.begin(), expected.end(), [](const SortEntry& a, const SortEntry& b) {
        return a.key < b.key;
    });

    // Populate queue items and sort
    queue.BeginFrame();
    for (const auto& entry : testEntries) {
        auto res = queue.Reserve(4, 6);
        assert(res.isValid);
        res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
        res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;
        queue.Commit(res, entry.key, 0, 4, 6);
    }

    BatchView batch = queue.Finalize();
    assert(batch.runs.size() > 0);

    // Verify queue sorted entries match std::stable_sort oracle exactly
    auto sorted = queue.GetSortedEntries();
    assert(sorted.size() == expected.size());
    for (usize i = 0; i < expected.size(); ++i) {
        assert(sorted[i].key == expected[i].key);
        assert(sorted[i].itemIndex == expected[i].itemIndex);
    }

    std::cout << "     [PASS] 64-Bit SortKey & Radix Sort Stability" << std::endl;
}

void TestQueueRunMerging() {
    std::cout << "  -> Testing Queue Run Merging..." << std::endl;

    RenderQueue queue;
    auto initRes = queue.Init({
        .maxVertices = 1000,
        .maxIndices = 1000,
        .maxItems = 100,
        .maxRuns = 50,
        .maxScissors = 16
    });
    assert(initRes);

    queue.BeginFrame();

    // Emit 10 identical quads (same texture, blend, material, scissor)
    const u64 key = SortKey::Pack(RenderPass::World, 0, 0.0f, BlendMode::Premultiplied, 0, 1);
    for (int i = 0; i < 10; ++i) {
        auto res = queue.Reserve(4, 6);
        assert(res.isValid);
        res.indices[0] = 0; res.indices[1] = 1; res.indices[2] = 2;
        res.indices[3] = 0; res.indices[4] = 2; res.indices[5] = 3;
        queue.Commit(res, key, 0, 4, 6);
    }

    BatchView batch = queue.Finalize();
    // All 10 quads must merge into exactly 1 draw run!
    assert(batch.runs.size() == 1);
    assert(batch.runs[0].indexCount == 60);
    assert(batch.runs[0].vertexCount == 40);

    // Now emit quads with different textures: should break runs
    queue.BeginFrame();
    u64 keyTex1 = SortKey::Pack(RenderPass::World, 0, 0.0f, BlendMode::Premultiplied, 0, 1);
    u64 keyTex2 = SortKey::Pack(RenderPass::World, 0, 0.0f, BlendMode::Premultiplied, 0, 2);

    for (int i = 0; i < 5; ++i) {
        auto res = queue.Reserve(4, 6);
        queue.Commit(res, keyTex1, 0, 4, 6);
    }
    for (int i = 0; i < 5; ++i) {
        auto res = queue.Reserve(4, 6);
        queue.Commit(res, keyTex2, 0, 4, 6);
    }

    BatchView multiBatch = queue.Finalize();
    assert(multiBatch.runs.size() == 2);
    assert(multiBatch.runs[0].textureSlot == 1);
    assert(multiBatch.runs[1].textureSlot == 2);

    std::cout << "     [PASS] Queue Run Merging" << std::endl;
}

void TestTextureLifecycle() {
    std::cout << "  -> Testing Texture Handles & Stale-Handle Protection..." << std::endl;

    RenderDevice device;
    auto devRes = device.CreateHeadless({.maxTextures = 16});
    assert(devRes);

    // Slot 1 is built-in white texture
    assert(device.IsTextureValid(TextureHandle::White()));
    assert(!device.IsTextureValid(TextureHandle::Null()));

    // Create custom texture
    TextureConfig cfg{.width = 32, .height = 32};
    auto texRes = device.CreateTexture(cfg);
    assert(texRes);
    TextureHandle handle1 = texRes.Value();
    assert(device.IsTextureValid(handle1));

    // Destroy texture
    device.DestroyTexture(handle1);
    assert(!device.IsTextureValid(handle1)); // Handle is now stale!

    // Reallocate slot: generational index increments, old handle remains invalid
    auto texRes2 = device.CreateTexture(cfg);
    assert(texRes2);
    TextureHandle handle2 = texRes2.Value();
    assert(handle2.index == handle1.index); // Same slot reused
    assert(handle2.generation != handle1.generation); // Different generation!
    assert(!device.IsTextureValid(handle1)); // Stale handle still safely rejected
    assert(device.IsTextureValid(handle2));

    // TextureRegion UV calculations and flipping
    TextureRegion region = TextureRegion::FromPixels(handle2, {0, 0, 16, 16}, 32, 32);
    assert(region.uvRect.w == 0.5f && region.uvRect.h == 0.5f);

    TextureRegion flippedH = region.Flipped(SpriteFlip::FlipH);
    assert(flippedH.uvRect.x == 0.5f && flippedH.uvRect.w == -0.5f);

    std::cout << "     [PASS] Texture Handles & Stale-Handle Protection" << std::endl;
}

void TestPrimitiveBatching() {
    std::cout << "  -> Testing Vector Primitives..." << std::endl;

    RenderDevice device;
    assert(device.CreateHeadless());

    RenderContext ctx;
    assert(ctx.Init(device));

    assert(device.BeginFrame());
    ctx.BeginFrame();
    auto& prim = ctx.Primitives();

    // Solid & Stroke Rect
    prim.FillRect({10, 10, 100, 50}, Colors::Red);
    prim.StrokeRect({20, 20, 80, 40}, 2.0f, Colors::Green);

    // Lines & Triangles
    prim.Line({0, 0}, {100, 100}, 4.0f, Colors::Blue);
    prim.FillTriangle({0, 0}, {50, 100}, {100, 0}, Colors::Yellow);

    // Circles & Capsules
    prim.FillCircle({200, 200}, 30.0f, Colors::Cyan);
    prim.StrokeCircle({300, 300}, 25.0f, 3.0f, Colors::Magenta);
    prim.FillCapsule({50, 50}, {150, 50}, 16.0f, Colors::White);
    prim.StrokeCapsule({50, 100}, {150, 100}, 16.0f, 2.0f, Colors::Red);

    // Grids & Text
    prim.Grid({0, 0, 200, 200}, 50.0f, 1.0f, Colors::Gray);
    prim.Text({10, 10}, "Hello EngineN 120Hz", Colors::White);

    assert(ctx.Queue().GetStats().drawCount > 0);
    assert(ctx.Queue().GetStats().droppedDraws == 0);

    auto endRes = ctx.EndFrame();
    assert(endRes);
    assert(device.Present());

    std::cout << "     [PASS] Vector Primitives" << std::endl;
}

void TestZeroAllocationOnHotPath() {
    std::cout << "  -> Testing Zero-Allocation Per-Frame Hot Path..." << std::endl;

    RenderDevice device;
    assert(device.CreateHeadless());

    RenderContext ctx;
    assert(ctx.Init(device, {
        .maxVertices = 65'536,
        .maxIndices = 98'304,
        .maxItems = 16'384,
        .maxRuns = 1'024,
        .maxScissors = 64
    }));

    // Warm-up frame
    assert(device.BeginFrame());
    ctx.BeginFrame();
    ctx.Primitives().FillRect({10, 10, 100, 100}, Colors::White);
    assert(ctx.EndFrame());
    assert(device.Present());

    // Enable heap allocation interception
    s_allocationCount = 0;
    s_trackAllocations = true;

    // Run 50 frames with mixed draw commands
    for (int frame = 0; frame < 50; ++frame) {
        assert(device.BeginFrame());
        ctx.BeginFrame();

        // 100 mixed primitives
        for (int i = 0; i < 50; ++i) {
            ctx.Primitives().FillRect({static_cast<f32>(i * 5), 10.0f, 20.0f, 20.0f}, Colors::Red);
            ctx.Primitives().Line({0.0f, 0.0f}, {100.0f, static_cast<f32>(i * 2)}, 2.0f, Colors::Green);
            ctx.Primitives().FillCircle({150.0f, 150.0f}, 25.0f, Colors::Blue);
        }

        assert(ctx.EndFrame());
        assert(device.Present());
    }

    s_trackAllocations = false;

    std::cout << "     -> Total Hot-Path Heap Allocations across 50 frames: " << s_allocationCount << std::endl;
    assert(s_allocationCount == 0);

    std::cout << "     [PASS] Zero-Allocation Per-Frame Hot Path" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << " EngineN — Milestone 03: Renderer Verification Suite" << std::endl;
    std::cout << "==================================================" << std::endl;

    TestColorMath();
    TestRectGeometry();
    TestMat2DMath();
    TestTransform2DAndLerp();
    TestViewTransform2D();
    TestSortKeyPackingAndRadixSort();
    TestQueueRunMerging();
    TestTextureLifecycle();
    TestPrimitiveBatching();
    TestZeroAllocationOnHotPath();

    std::cout << "==================================================" << std::endl;
    std::cout << " ALL 10 TEST SUITES PASSED! (0 FAILURES)" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
}

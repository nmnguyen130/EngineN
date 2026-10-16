#include "platform/Platform.h"
#include "platform/Window.h"
#include "renderer/Renderer.h"
#include "renderer/RendererList.h"
#include "renderer/RendererTypes.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>

using namespace en;
using namespace en::renderer;

namespace {

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

void AssertTrue(bool condition, std::string_view expression, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line()
                  << " Assertion failed: " << expression << "\n";
        ++g_failedAssertions;
    } else {
        ++g_passedAssertions;
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond)

void TestCompileTimeConstantsAndLayout() {
    static_assert(sizeof(Color) == 16);
    static_assert(sizeof(Vertex2D) == 32);
    static_assert(sizeof(RenderList::PreparedQuad) == 64);
    static_assert(alignof(RenderList::PreparedQuad) == 16);
    static_assert(RenderList::kMaxQuads == 65536);
    static_assert(RenderList::kMaxBatches == 4096);
    static_assert(RenderList::kMaxViews == 16);
    static_assert(RenderList::kMaxPasses == 16);

    TEST_ASSERT(sizeof(Color) == 16);
    TEST_ASSERT(sizeof(Vertex2D) == 32);
    TEST_ASSERT(sizeof(RenderList::PreparedQuad) == 64);
    TEST_ASSERT(alignof(RenderList::PreparedQuad) == 16);
}

void TestRendererTypes() {
    // 1. TextureHandle
    constexpr TextureHandle invalidHandle{};
    TEST_ASSERT(!invalidHandle.IsValid());

    constexpr TextureHandle white = kWhiteTexture;
    TEST_ASSERT(white.IsValid());
    TEST_ASSERT(white.index == 0 && white.generation == 1);

    // 2. Color math and premultiplication
    constexpr Color opaqueWhite = Colors::White;
    TEST_ASSERT(opaqueWhite.r == 1.0f && opaqueWhite.g == 1.0f && opaqueWhite.b == 1.0f && opaqueWhite.a == 1.0f);

    constexpr Color halfRed = Color::Premultiplied(1.0f, 0.0f, 0.0f, 0.5f);
    TEST_ASSERT(halfRed.r == 0.5f && halfRed.g == 0.0f && halfRed.b == 0.0f && halfRed.a == 0.5f);

    constexpr Color fromRgba = Color::FromRGBA8(255, 128, 0, 128);
    TEST_ASSERT(std::abs(fromRgba.a - (128.0f / 255.0f)) < 0.01f);
    TEST_ASSERT(std::abs(fromRgba.r - (128.0f / 255.0f)) < 0.01f);

    // 3. FRect helpers
    constexpr FRect emptyRect{0.0f, 0.0f, 0.0f, 100.0f};
    TEST_ASSERT(emptyRect.IsEmpty());

    constexpr FRect validRect{10.0f, 20.0f, 100.0f, 50.0f};
    TEST_ASSERT(!validRect.IsEmpty());

    // 4. SortKey bit-packing invariant
    QuadCmd cmd{};
    cmd.passId = 3;
    cmd.layer = 12;
    cmd.depth = 1000;
    cmd.blend = BlendMode::Additive;
    cmd.viewSlot = 2;
    cmd.texture = TextureHandle{42, 1};

    const u64 key = SortKey::Pack(cmd, 7);
    TEST_ASSERT(SortKey::PassId(key) == 3);
    TEST_ASSERT(SortKey::Blend(key) == BlendMode::Additive);
    TEST_ASSERT(SortKey::ViewSlot(key) == 2);
    TEST_ASSERT(SortKey::TextureIndex(key) == 42);
    TEST_ASSERT(SortKey::QuadIndex(key) == 7);
}

void TestRenderListRecordingAndCulling() {
    auto list = std::make_unique<RenderList>();
    list->Reset({800, 600});

    TEST_ASSERT(list->Stats().submittedQuads == 0);
    TEST_ASSERT(list->Stats().culledQuads == 0);

    // Quad inside the 800x600 viewport
    list->PushRect(FRect{100.0f, 100.0f, 50.0f, 50.0f}, Colors::White);
    TEST_ASSERT(list->Stats().submittedQuads == 1);
    TEST_ASSERT(list->Stats().culledQuads == 0);

    // Quad completely outside the viewport (e.g. far off-screen)
    list->PushRect(FRect{2000.0f, 2000.0f, 50.0f, 50.0f}, Colors::Red);
    TEST_ASSERT(list->Stats().submittedQuads == 1);
    TEST_ASSERT(list->Stats().culledQuads == 1);

    // Line drawing inside viewport
    list->PushLine(0.0f, 0.0f, 100.0f, 100.0f, 2.0f, Colors::Green);
    TEST_ASSERT(list->Stats().submittedQuads == 2);
    TEST_ASSERT(list->Stats().culledQuads == 1);
}

void TestRenderListRadixSortAndBatching() {
    auto list = std::make_unique<RenderList>();
    list->Reset({800, 600});

    // Record quads with differing texture handles to force separate batches
    QuadCmd q1{};
    q1.x = 10.0f; q1.y = 10.0f; q1.w = 32.0f; q1.h = 32.0f;
    q1.texture = TextureHandle{1, 1};
    q1.layer = 1;
    list->PushQuad(q1);

    QuadCmd q2{};
    q2.x = 20.0f; q2.y = 20.0f; q2.w = 32.0f; q2.h = 32.0f;
    q2.texture = TextureHandle{1, 1};
    q2.layer = 1;
    list->PushQuad(q2);

    // Different texture on same layer
    QuadCmd q3{};
    q3.x = 30.0f; q3.y = 30.0f; q3.w = 32.0f; q3.h = 32.0f;
    q3.texture = TextureHandle{2, 1};
    q3.layer = 1;
    list->PushQuad(q3);

    list->Compile();

    const auto batches = list->Batches();
    TEST_ASSERT(batches.size() == 2);
    TEST_ASSERT(batches[0].quadCount == 2);
    TEST_ASSERT(batches[0].texture.index == 1);
    TEST_ASSERT(batches[1].quadCount == 1);
    TEST_ASSERT(batches[1].texture.index == 2);

    // Check sorted keys monotonicity
    const auto keys = list->SortedKeys();
    TEST_ASSERT(keys.size() == 3);
    for (usize i = 1; i < keys.size(); ++i) {
        TEST_ASSERT(keys[i - 1] <= keys[i]);
    }
}

void TestRenderListVertexExpansion() {
    auto list = std::make_unique<RenderList>();
    list->Reset({800, 600});

    list->PushRect(FRect{10.0f, 20.0f, 30.0f, 40.0f}, Colors::Red);
    list->Compile();

    std::array<Vertex2D, 4> vertices{};
    list->ExpandVertices(0, 1, vertices);

    // Verify 4-vertex expansion
    TEST_ASSERT(vertices[0].color == Colors::Red);
    TEST_ASSERT(vertices[1].color == Colors::Red);
    TEST_ASSERT(vertices[2].color == Colors::Red);
    TEST_ASSERT(vertices[3].color == Colors::Red);

    // Shared index buffer
    const auto indices = RenderList::SharedQuadIndices(1);
    TEST_ASSERT(indices.size() == 6);
    TEST_ASSERT(indices[0] == 0 && indices[1] == 1 && indices[2] == 2);
    TEST_ASSERT(indices[3] == 2 && indices[4] == 3 && indices[5] == 0);
}

void TestRenderListAuditHashDeterminism() {
    auto a = std::make_unique<RenderList>();
    a->Reset({800, 600});
    a->PushRect(FRect{10.0f, 20.0f, 30.0f, 40.0f}, Colors::Red);
    a->PushRect(FRect{50.0f, 60.0f, 70.0f, 80.0f}, Colors::Blue);
    a->Compile();

    auto b = std::make_unique<RenderList>();
    b->Reset({800, 600});
    b->PushRect(FRect{10.0f, 20.0f, 30.0f, 40.0f}, Colors::Red);
    b->PushRect(FRect{50.0f, 60.0f, 70.0f, 80.0f}, Colors::Blue);
    b->Compile();

    TEST_ASSERT(a->ComputeAuditHash() != 0);
    TEST_ASSERT(a->ComputeAuditHash() == b->ComputeAuditHash());
}

void TestRendererHardwareLifecycle() {
    platform::Platform platform;
    const auto pInit = platform.Init("RendererTest", "EngineN");
    TEST_ASSERT(pInit.has_value());

    platform::Window window;
    const auto wInit = window.Create(platform::WindowConfig{
        .title = "RendererTest",
        .size = {640, 480},
        .mode = platform::WindowMode::Windowed
    });
    TEST_ASSERT(wInit.has_value());

    auto renderer = std::make_unique<Renderer>();
    TEST_ASSERT(!renderer->IsValid());

    const auto rInit = renderer->Init(window);
    TEST_ASSERT(rInit.has_value());
    TEST_ASSERT(renderer->IsValid());

    // Slot 0 is permanently bound to 1x1 white texture
    TEST_ASSERT(renderer->IsTextureValid(kWhiteTexture));

    // Dynamic texture creation (RGBA8 2x2)
    const std::array<u8, 16> rawPixels{
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255
    };
    const auto texRes = renderer->CreateTexture(TextureDesc{
        .width = 2,
        .height = 2,
        .format = TextureFormat::RGBA8,
        .pixels = rawPixels,
        .pitch = 8
    });
    TEST_ASSERT(texRes.has_value());
    const TextureHandle h = *texRes;
    TEST_ASSERT(h.IsValid());
    TEST_ASSERT(renderer->IsTextureValid(h));

    // Update texture subregion
    const std::array<u8, 4> singlePixel{255, 255, 0, 255};
    const auto updateRes = renderer->UpdateTexture(h, FRect{0.0f, 0.0f, 1.0f, 1.0f}, singlePixel, 4);
    TEST_ASSERT(updateRes.has_value());

    // Frame submission test
    auto& list = renderer->BeginFrame();
    list.PushRect(FRect{10.0f, 10.0f, 100.0f, 100.0f}, Colors::Green);
    list.PushQuad(QuadCmd{
        .x = 150.0f, .y = 150.0f, .w = 64.0f, .h = 64.0f,
        .texture = h
    });
    renderer->Submit();

    const auto& stats = renderer->LastFrameStats();
    TEST_ASSERT(stats.submittedQuads == 2);
    TEST_ASSERT(stats.drawBatches >= 1);

    // Destroy texture and verify ABA generation invalidation
    renderer->DestroyTexture(h);
    TEST_ASSERT(!renderer->IsTextureValid(h));

    // Next allocated texture in the same slot gets incremented generation
    const auto tex2Res = renderer->CreateTexture(TextureDesc{
        .width = 1,
        .height = 1,
        .format = TextureFormat::RGBA8,
        .pixels = singlePixel,
        .pitch = 4
    });
    TEST_ASSERT(tex2Res.has_value());
    TEST_ASSERT(tex2Res->index == h.index);
    TEST_ASSERT(tex2Res->generation > h.generation);
    TEST_ASSERT(!renderer->IsTextureValid(h)); // Old handle remains invalid!

    renderer->Shutdown();
    TEST_ASSERT(!renderer->IsValid());

    window.Destroy();
    platform.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Renderer Subsystem Tests...\n";

    TestCompileTimeConstantsAndLayout();
    TestRendererTypes();
    TestRenderListRecordingAndCulling();
    TestRenderListRadixSortAndBatching();
    TestRenderListVertexExpansion();
    TestRenderListAuditHashDeterminism();
    TestRendererHardwareLifecycle();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? 0 : 1;
}

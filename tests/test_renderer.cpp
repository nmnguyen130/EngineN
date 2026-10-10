#include "core/Log.h"
#include "renderer/RendererTypes.h"
#include "renderer/QuadInstance.h"
#include "renderer/SortKey.h"
#include "renderer/FramePacket.h"
#include "renderer/FrameArena.h"
#include "renderer/Batcher.h"
#include "renderer/NullBackend.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

using namespace EngineN;

// -----------------------------------------------------------------------------
// Global Allocation Counter for 0-Alloc Contract Verification
// -----------------------------------------------------------------------------
static bool g_trackAllocations = false;
static usize g_allocationCount = 0;

void* operator new(std::size_t size) {
    if (g_trackAllocations) {
        g_allocationCount++;
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

// -----------------------------------------------------------------------------
// Unit Test Suites
// -----------------------------------------------------------------------------

void TestDataContractAndLayout() {
    std::cout << "  [1/7] Testing Canonical Struct Sizes & Alignments..." << std::endl;

    static_assert(sizeof(QuadInstance) == 48);
    static_assert(alignof(QuadInstance) == 16);
    static_assert(sizeof(SortEntry) == 16);
    static_assert(alignof(SortEntry) == 16);

    // Color premultiplication checks
    Color c1 = Color::FromRGBA8(255, 128, 64, 128); // 50% alpha
    u32 premul = c1.ToPremultipliedRGBA8();
    u8 pr = static_cast<u8>(premul & 0xFF);
    u8 pg = static_cast<u8>((premul >> 8) & 0xFF);
    u8 pb = static_cast<u8>((premul >> 16) & 0xFF);
    u8 pa = static_cast<u8>((premul >> 24) & 0xFF);

    assert(pa == 128);
    assert(pr >= 127 && pr <= 128); // 255 * (128/255)
    assert(pg >= 63 && pg <= 65);
    assert(pb >= 31 && pb <= 33);

    std::cout << "        -> PASS" << std::endl;
}

void TestSortKeyPacking() {
    std::cout << "  [2/7] Testing 64-bit SortKey Pack & Unpack Fidelity..." << std::endl;

    const u8 pass = 5;
    const u8 layer = 120;
    const u16 depth = 43210;
    const u8 blend = static_cast<u8>(BlendMode::Additive);
    const u16 pipeline = 2048;
    const u16 texture = 54321;
    const u8 spare = 29;

    const u64 packed = SortKey::Pack(pass, layer, depth, blend, pipeline, texture, spare);
    const SortKey key{packed};

    assert(key.GetPass() == pass);
    assert(key.GetLayer() == layer);
    assert(key.GetDepth() == depth);
    assert(key.GetBlend() == blend);
    assert(key.GetPipeline() == pipeline);
    assert(key.GetTexture() == texture);
    assert(key.GetSpare() == spare);

    // Boundary sorting hierarchy check: Pass > Layer > Depth > Blend > Pipeline > Texture
    const u64 keyPass0 = SortKey::Pack(0, 255, 65535, 7, 4095, 65535);
    const u64 keyPass1 = SortKey::Pack(1, 0, 0, 0, 0, 0);
    assert(keyPass0 < keyPass1); // Higher pass must sort after lower pass regardless of layer/depth

    std::cout << "        -> PASS" << std::endl;
}

void TestFrameArenaCapacityAndOverflow() {
    std::cout << "  [3/7] Testing FrameArena Pre-Allocation & Overflow Drops..." << std::endl;

    FrameArenaSlot slot;
    assert(slot.InstanceCount() == 0);
    assert(slot.DroppedInstances() == 0);

    // Fill capacity
    QuadInstance testQuad{};
    testQuad.pos[0] = 100.0f;
    testQuad.pos[1] = 200.0f;

    u32 pushIndex = slot.PushQuads(&testQuad, 1);
    assert(pushIndex == 0);
    assert(slot.InstanceCount() == 1);

    // Reset test
    slot.Reset();
    assert(slot.InstanceCount() == 0);
    assert(slot.DroppedInstances() == 0);

    // Test dropping when full
    std::vector<QuadInstance> batch(1024);
    for (usize i = 0; i < FrameArenaSlot::kMaxInstances / 1024; ++i) {
        (void)slot.PushQuads(batch.data(), 1024);
    }
    assert(slot.InstanceCount() == FrameArenaSlot::kMaxInstances);

    // Excess push must drop cleanly without reallocating
    u32 droppedIdx = slot.PushQuads(batch.data(), 10);
    assert(droppedIdx == ~0u);
    assert(slot.DroppedInstances() == 10);
    assert(slot.InstanceCount() == FrameArenaSlot::kMaxInstances);

    std::cout << "        -> PASS" << std::endl;
}

void TestBatcherMonotonicBypass() {
    std::cout << "  [4/7] Testing Batcher Monotonic Sort Bypass..." << std::endl;

    FrameArenaSlot slot;
    Batcher batcher;
    FramePacket packet;

    // Push 10 strictly increasing keys
    for (u32 i = 0; i < 10; ++i) {
        QuadInstance q{};
        u32 idx = slot.PushQuads(&q, 1);
        (void)slot.PushSortEntry(SortEntry{
            .key = SortKey::Pack(0, 0, static_cast<u16>(i * 10), 0, 0, 1),
            .instanceIndex = idx,
            .instanceCount = 1,
            .viewSlot = 0
        });
    }

    batcher.Finalize(slot, packet);
    assert(packet.stats.instanceCount == 10);
    assert(!packet.stats.wasSorted); // Must have bypassed radix sort!

    std::cout << "        -> PASS" << std::endl;
}

void TestBatcherRadixSortAndStability() {
    std::cout << "  [5/7] Testing 8-bit LSD Radix Sort Stability..." << std::endl;

    FrameArenaSlot slot;
    Batcher batcher;
    FramePacket packet;

    // Push keys in reverse and unordered sequence with known duplicates
    const u64 keys[] = { 500, 200, 700, 200, 100, 900, 200, 300 };
    for (u32 i = 0; i < 8; ++i) {
        QuadInstance q{};
        q.param[0] = static_cast<f32>(i); // Tag original submission index in param[0]
        u32 idx = slot.PushQuads(&q, 1);
        (void)slot.PushSortEntry(SortEntry{
            .key = keys[i],
            .instanceIndex = idx,
            .instanceCount = 1,
            .viewSlot = 0
        });
    }

    batcher.Finalize(slot, packet);
    assert(packet.stats.wasSorted); // Out of order: must trigger sort
    assert(packet.stats.instanceCount == 8);

    // Verify non-decreasing sorted order
    auto entries = slot.SortEntries();
    for (u32 i = 1; i < entries.size(); ++i) {
        assert(entries[i].key >= entries[i - 1].key);
    }

    // Verify LSD Radix Sort stability for key 200:
    // Original indices were 1, 3, 6
    std::vector<u32> orderOf200;
    for (u32 i = 0; i < entries.size(); ++i) {
        if (entries[i].key == 200) {
            u32 origIdx = static_cast<u32>(packet.instances[i].param[0]);
            orderOf200.push_back(origIdx);
        }
    }
    assert(orderOf200.size() == 3);
    assert(orderOf200[0] == 1);
    assert(orderOf200[1] == 3);
    assert(orderOf200[2] == 6);

    std::cout << "        -> PASS" << std::endl;
}

void TestBatchMergeCoalescence() {
    std::cout << "  [6/7] Testing Contiguous DrawBatch Coalescence..." << std::endl;

    FrameArenaSlot slot;
    Batcher batcher;
    FramePacket packet;

    // Push 6 items: 3 with texture 1, then 1 with texture 2, then 2 with texture 1
    const u16 textures[] = { 1, 1, 1, 2, 1, 1 };
    for (u32 i = 0; i < 6; ++i) {
        QuadInstance q{};
        u32 idx = slot.PushQuads(&q, 1);
        (void)slot.PushSortEntry(SortEntry{
            .key = SortKey::Pack(0, 0, static_cast<u16>(i), 0, 0, textures[i]),
            .instanceIndex = idx,
            .instanceCount = 1,
            .viewSlot = 0
        });
    }

    batcher.Finalize(slot, packet);
    // After sorting by key (depth i = 0..5, all texture states preserved in order):
    // Batches should be:
    // Batch 0: Texture 1, count = 3
    // Batch 1: Texture 2, count = 1
    // Batch 2: Texture 1, count = 2
    assert(packet.batches.size() == 3);
    assert(packet.batches[0].textureSlot == 1 && packet.batches[0].instanceCount == 3);
    assert(packet.batches[1].textureSlot == 2 && packet.batches[1].instanceCount == 1);
    assert(packet.batches[2].textureSlot == 1 && packet.batches[2].instanceCount == 2);

    std::cout << "        -> PASS" << std::endl;
}

void TestZeroAllocationPerFrameContract() {
    std::cout << "  [7/7] Testing Strict 0-Allocation Per-Frame Contract..." << std::endl;

    FrameArena arena;
    Batcher batcher;
    NullBackend nullBackend;

    // Warm up one frame so any one-time initial static structures settle
    {
        auto& slot = arena.CurrentSlot();
        slot.Reset();
        QuadInstance q{};
        u32 idx = slot.PushQuads(&q, 1);
        (void)slot.PushSortEntry(SortEntry{.key = 1, .instanceIndex = idx, .instanceCount = 1, .viewSlot = 0});
        FramePacket packet;
        batcher.Finalize(slot, packet);
        auto res = nullBackend.Execute(packet);
        assert(res);
        arena.AdvanceSlot();
    }

    // Begin strict zero-allocation audit across 100 simulation frames
    g_allocationCount = 0;
    g_trackAllocations = true;

    for (int frame = 0; frame < 100; ++frame) {
        auto& slot = arena.CurrentSlot();
        slot.Reset();

        // Simulate recording 500 quads with mixed keys
        for (u32 i = 0; i < 500; ++i) {
            QuadInstance q{};
            q.pos[0] = static_cast<f32>(i);
            u32 idx = slot.PushQuads(&q, 1);

            const u16 texture = static_cast<u16>((i % 4) + 1);
            const u16 depth = static_cast<u16>(500 - i); // Non-monotonic
            (void)slot.PushSortEntry(SortEntry{
                .key = SortKey::Pack(0, 0, depth, 0, 0, texture),
                .instanceIndex = idx,
                .instanceCount = 1,
                .viewSlot = 0
            });
        }

        FramePacket packet;
        batcher.Finalize(slot, packet);

        auto execRes = nullBackend.Execute(packet);
        assert(execRes);
        assert(nullBackend.LastInstanceCount() == 500);

        arena.AdvanceSlot();
    }

    g_trackAllocations = false;

    std::cout << "        Total heap allocations during 100 frames: " << g_allocationCount << std::endl;
    assert(g_allocationCount == 0); // Strict proof: ZERO heap allocations!

    std::cout << "        -> PASS" << std::endl;
}

int main() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << " Running EngineN 2D Presentation Subsystem Verification" << std::endl;
    std::cout << "========================================================" << std::endl;

    TestDataContractAndLayout();
    TestSortKeyPacking();
    TestFrameArenaCapacityAndOverflow();
    TestBatcherMonotonicBypass();
    TestBatcherRadixSortAndStability();
    TestBatchMergeCoalescence();
    TestZeroAllocationPerFrameContract();

    std::cout << "\n========================================================" << std::endl;
    std::cout << " ALL RENDERER SUB-SYSTEM TESTS PASSED (0-ALLOC VERIFIED)" << std::endl;
    std::cout << "========================================================\n" << std::endl;
    return 0;
}

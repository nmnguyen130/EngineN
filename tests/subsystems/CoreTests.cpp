#include "core/Containers.h"
#include "core/Diagnostics.h"
#include "core/Jobs.h"
#include "core/Math.h"
#include "core/Memory.h"
#include "core/Types.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace en::tests {

struct TestStats {
    u32 passed{0};
    u32 failed{0};
};

#define EN_TEST_CHECK(stats, ...)                                                                \
    do {                                                                                         \
        if (__VA_ARGS__) {                                                                       \
            ++(stats).passed;                                                                    \
        } else {                                                                                 \
            ++(stats).failed;                                                                    \
            std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);      \
        }                                                                                        \
    } while (false)

enum class TestFlags : u8 {
    None = 0,
    Alpha = 1U << 0,
    Beta = 1U << 1,
    Gamma = 1U << 2,
};
consteval bool enable_bitmask_operators(TestFlags) noexcept { return true; }

void TestTypesAndPrimitives(TestStats& stats) noexcept {
    using namespace en::literals;

    // Result and Err
    const Result<u32, u8> okRes{42U};
    const Result<u32, u8> errRes = Err<u8>(7U);
    EN_TEST_CHECK(stats, okRes.has_value() && *okRes == 42U);
    EN_TEST_CHECK(stats, !errRes.has_value() && errRes.error() == 7U);

    // BitmaskEnum
    TestFlags flags = TestFlags::Alpha | TestFlags::Beta;
    EN_TEST_CHECK(stats, HasFlag(flags, TestFlags::Alpha));
    EN_TEST_CHECK(stats, !HasFlag(flags, TestFlags::Gamma));
    flags ^= TestFlags::Alpha;
    EN_TEST_CHECK(stats, !HasFlag(flags, TestFlags::Alpha) && HasFlag(flags, TestFlags::Beta));
    flags &= ~TestFlags::Beta;
    EN_TEST_CHECK(stats, flags == TestFlags::None);

    // Power-of-two and alignment boundaries
    EN_TEST_CHECK(stats, NextPow2(0U) == 1U && NextPow2(1U) == 1U && NextPow2(2U) == 2U);
    EN_TEST_CHECK(stats, NextPow2(3U) == 4U && NextPow2(1023U) == 1024U);
    EN_TEST_CHECK(stats, NextPow2(0x80000000U) == 0x80000000U);
    EN_TEST_CHECK(stats, NextPow2(0x80000001U) == 0x80000000U);
    EN_TEST_CHECK(stats, NextPow2(0xFFFFFFFFU) == 0x80000000U);

    EN_TEST_CHECK(stats, AlignUp(0U, 64U) == 0U && AlignUp(1U, 64U) == 64U && AlignUp(64U, 64U) == 64U);
    EN_TEST_CHECK(stats, AlignUp(15U, 3U) == 15U);
    EN_TEST_CHECK(stats, AlignUp(~usize{0} - 10U, 64U) == (~usize{0} & ~usize{63}));

    // IEEE 802.3 CRC32 standard vector ("123456789" -> 0xCBF43926) and incremental chaining
    constexpr std::array<u8, 9> kVec = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    constexpr u32 kCompileCrc = ComputeCrc32(kVec);
    static_assert(kCompileCrc == 0xCBF43926U);
    EN_TEST_CHECK(stats, kCompileCrc == 0xCBF43926U);
    EN_TEST_CHECK(stats, ComputeCrc32({}) == 0U);
    const u32 crcPart1 = ComputeCrc32(std::span<const u8>(kVec.data(), 4));
    const u32 crcChained = ComputeCrc32(std::span<const u8>(kVec.data() + 4, 5), crcPart1);
    EN_TEST_CHECK(stats, crcChained == 0xCBF43926U);

    // StringId case and slash normalization
    constexpr StringId emptySid = ""_sid;
    constexpr StringId sidA = "Assets\\Textures/Hero_Sheet.TGA"_sid;
    constexpr StringId sidB = "assets/textures/hero_sheet.tga"_sid;
    static_assert(!emptySid.IsValid());
    static_assert(sidA == sidB);
    EN_TEST_CHECK(stats, !emptySid.IsValid() && sidA.IsValid() && sidA == sidB);

    // Generational Handle
    struct DummyTag {};
    using DummyHandle = Handle<DummyTag>;
    constexpr DummyHandle nullH{};
    constexpr DummyHandle validH{0x1234U, 0x5678U};
    EN_TEST_CHECK(stats, !nullH.IsValid() && validH.IsValid());
    EN_TEST_CHECK(stats, DummyHandle::FromPacked(validH.Packed()) == validH);
    const DummyHandle maxH{0xFFFFU, 0xFFFFU};
    EN_TEST_CHECK(stats, DummyHandle::FromPacked(maxH.Packed()) == maxH);

    // FixedString exact byte layout (2-byte aligned, zero struct padding hole) & path resolution
    struct EmbeddedTest {
        u8 tag{0};
        FixedString<16> str{};
    };
    static_assert(sizeof(FixedString<16>) == 16);
    static_assert(sizeof(EmbeddedTest) == 18);

    FixedString<16> smallStr{};
    EN_TEST_CHECK(stats, smallStr.Empty() && smallStr.Capacity() == 13U);
    EN_TEST_CHECK(stats, smallStr.Assign("1234567890"));
    EN_TEST_CHECK(stats, smallStr.Append("123") && smallStr.Size() == 13U);
    EN_TEST_CHECK(stats, !smallStr.Append("4") && smallStr.View() == "1234567890123");
    EN_TEST_CHECK(stats, !smallStr.Assign("12345678901234"));
    EN_TEST_CHECK(stats, smallStr.View() == "1234567890123");
    const char embeddedNull[] = {'a', '\0', 'b'};
    EN_TEST_CHECK(stats, !smallStr.Assign(std::string_view(embeddedNull, 3)));

    FixedString<256> baseDir{};
    EN_TEST_CHECK(stats, !baseDir.AssignNormalizedDir(""));
    EN_TEST_CHECK(stats, baseDir.AssignNormalizedDir("C:\\Game\\Assets"));
    EN_TEST_CHECK(stats, baseDir.View() == "C:/Game/Assets/");

    FixedString<256> outPath{};
    EN_TEST_CHECK(stats, baseDir.Resolve("sprites\\hero.tga", outPath));
    EN_TEST_CHECK(stats, outPath.View() == "C:/Game/Assets/sprites/hero.tga");
    EN_TEST_CHECK(stats, baseDir.Resolve("archive..v1.pak", outPath));
    EN_TEST_CHECK(stats, outPath.View() == "C:/Game/Assets/archive..v1.pak");

    // Reject path traversal, absolute paths, empty segments, colons, and control chars without mutating outPath
    const FixedString<256> preserved = outPath;
    EN_TEST_CHECK(stats, !baseDir.Resolve("", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("/etc/passwd", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("\\Windows\\System32", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("sprites/", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("..", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve(".", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("sprites/../secret.txt", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("sprites/./hero.tga", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("sprites//hero.tga", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("C:/other.tga", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("bad\nname.tga", outPath));
    EN_TEST_CHECK(stats, !baseDir.Resolve("bad\x7fname.tga", outPath));
    EN_TEST_CHECK(stats, outPath == preserved);

    // Rng64 determinism and extreme bounds
    Rng64 rngZero{0ULL};
    Rng64 rngDefault{Rng64::kDefaultSeed};
    EN_TEST_CHECK(stats, rngZero.State() == rngDefault.State());
    EN_TEST_CHECK(stats, rngZero.NextU64() == rngDefault.NextU64());
    EN_TEST_CHECK(stats, rngZero.NextBoundedU32(0U) == 0U && rngZero.NextBoundedU32(1U) == 0U);

    for (u32 i = 0; i < 128U; ++i) {
        const f32 rf = rngZero.NextF32();
        const u32 rb = rngZero.NextBoundedU32(17U);
        const i32 ri = rngZero.RangeI32(-5, 5);
        EN_TEST_CHECK(stats, rf >= 0.0f && rf < 1.0f);
        EN_TEST_CHECK(stats, rb < 17U);
        EN_TEST_CHECK(stats, ri >= -5 && ri <= 5);
    }
    EN_TEST_CHECK(stats, rngZero.RangeI32(10, -10) == 10);
    EN_TEST_CHECK(stats, rngZero.RangeI32(7, 7) == 7);
    const i32 fullSpanSample = rngZero.RangeI32(
        std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max());
    const i32 wideSpanSample = rngZero.RangeI32(-2000000000, 2000000000);
    EN_TEST_CHECK(stats, fullSpanSample >= std::numeric_limits<i32>::min());
    EN_TEST_CHECK(stats, wideSpanSample >= -2000000000 && wideSpanSample <= 2000000000);
}

void TestMath2D(TestStats& stats) noexcept {
    // FastSinCos accuracy across [-720, 720] degrees and exact cardinal zeroes
    f32 c90 = 1.0f, s90 = 0.0f;
    FastSinCos(kHalfPi, c90, s90);
    EN_TEST_CHECK(stats, std::abs(c90) < 1e-6f && std::abs(s90 - 1.0f) < 1e-6f);

    for (i32 deg = -720; deg <= 720; deg += 15) {
        const f32 rad = static_cast<f32>(deg) * kDegToRad;
        f32 fastC = 0.0f, fastS = 0.0f;
        FastSinCos(rad, fastC, fastS);
        EN_TEST_CHECK(stats, std::abs(fastC - std::cos(rad)) < 1e-5f);
        EN_TEST_CHECK(stats, std::abs(fastS - std::sin(rad)) < 1e-5f);
    }

    // Spring, SmoothStep, EaseOutBack, CubicBezier2D
    EN_TEST_CHECK(stats, SmoothStep(10.0f, 5.0f, 10.0f) == 1.0f);
    EN_TEST_CHECK(stats, SmoothStep(10.0f, 5.0f, 4.0f) == 0.0f);
    EN_TEST_CHECK(stats, std::abs(EaseOutBack(1.0f) - 1.0f) < 1e-5f);

    f32 pos = 0.0f, vel = 0.0f;
    StepSpring(pos, vel, 10.0f, 15.0f, 0.0f);
    EN_TEST_CHECK(stats, pos == 0.0f && vel == 0.0f);
    for (u32 step = 0; step < 120U; ++step) {
        StepSpring(pos, vel, 10.0f, 12.0f, 1.0f / 60.0f);
    }
    EN_TEST_CHECK(stats, std::abs(pos - 10.0f) < 1e-2f);

    // Vec2 & Mat3x2
    const Vec2 zeroVec{0.0f, 0.0f};
    EN_TEST_CHECK(stats, (Vec2{4.0f, -2.0f} / 0.0f) == zeroVec);
    EN_TEST_CHECK(stats, zeroVec.Normalized() == zeroVec);
    EN_TEST_CHECK(stats, std::abs(Vec2{3.0f, 4.0f}.Length() - 5.0f) < 1e-5f);

    const Mat3x2 m = Mat3x2::FromTRS({10.0f, -5.0f}, 0.6f, {2.0f, 3.0f});
    const Mat3x2 invM = m.Inverse();
    const Vec2 origPt{7.5f, -3.25f};
    const Vec2 roundTrip = invM.TransformPoint(m.TransformPoint(origPt));
    EN_TEST_CHECK(stats, std::abs(roundTrip.x - origPt.x) < 1e-3f && std::abs(roundTrip.y - origPt.y) < 1e-3f);

    const Mat3x2 singular{0.0f, 0.0f, 0.0f, 0.0f, 5.0f, 5.0f};
    EN_TEST_CHECK(stats, singular.Inverse() == Mat3x2::Identity());

    // Rect & Color
    const Rect r{10.0f, 20.0f, 30.0f, 40.0f};
    EN_TEST_CHECK(stats, !r.IsEmpty() && Rect{0.0f, 0.0f, -1.0f, 5.0f}.IsEmpty());
    EN_TEST_CHECK(stats, r.Contains({10.0f, 20.0f}) && r.Contains({40.0f, 60.0f}) && !r.Contains({9.9f, 20.0f}));
    EN_TEST_CHECK(stats, r.Overlaps({35.0f, 55.0f, 10.0f, 10.0f}));
    EN_TEST_CHECK(stats, r.Intersection({20.0f, 30.0f, 50.0f, 50.0f}) == Rect{20.0f, 30.0f, 20.0f, 30.0f});

    const Color hsv = Color::FromHSV(-60.0f, 1.0f, 1.0f, 1.0f);
    EN_TEST_CHECK(stats, std::abs(hsv.r - 1.0f) < 1e-4f && std::abs(hsv.b - 1.0f) < 1e-4f && std::abs(hsv.g) < 1e-4f);
    EN_TEST_CHECK(stats, Color::FromRGBA8(255, 128, 0, 255).ToRGBA8() == 0xFF8000FFU);
    const Vec2 bezMid = CubicBezier2D({0.0f, 0.0f}, {0.0f, 10.0f}, {10.0f, 10.0f}, {10.0f, 0.0f}, 0.5f);
    EN_TEST_CHECK(stats, std::abs(bezMid.x - 5.0f) < 1e-4f && std::abs(bezMid.y - 7.5f) < 1e-4f);
}

void TestMemoryAndStreams(TestStats& stats) noexcept {
    AlignedArena arena{};
    EN_TEST_CHECK(stats, !arena.Init(0U));
    EN_TEST_CHECK(stats, !arena.Init(AlignedArena::kMaxArenaBytes + 64U));
    EN_TEST_CHECK(stats, arena.Init(64U * 1024U));
    EN_TEST_CHECK(stats, arena.IsValid() && arena.Capacity() == 64U * 1024U);

    EN_TEST_CHECK(stats, arena.CarveRawBytes(0U) == nullptr);
    EN_TEST_CHECK(stats, arena.Carve<u64>(AlignedArena::kMaxArenaBytes).empty());

    auto spanA = arena.Carve<u32>(16U);
    EN_TEST_CHECK(stats, spanA.size() == 16U && (reinterpret_cast<usize>(spanA.data()) & 63U) == 0U);
    spanA[0] = 0xDEADBEEFU;
    const u32 usedAfterA = arena.UsedBytes();

    {
        ArenaTempScope scope{arena};
        auto tempSlice = arena.Carve<u64>(128U);
        EN_TEST_CHECK(stats, tempSlice.size() == 128U && arena.UsedBytes() > usedAfterA);
        tempSlice[0] = 0x123456789ABCDEF0ULL;
    }
    EN_TEST_CHECK(stats, arena.UsedBytes() == usedAfterA);

    // Re-carving after ArenaTempScope rollback returns zeroed memory
    auto reCarved = arena.Carve<u64>(128U);
    EN_TEST_CHECK(stats, reCarved.size() == 128U && reCarved[0] == 0ULL);

    // Invalid marker rollback (> UsedBytes) is safely ignored
    const u32 usedAfterRecarve = arena.UsedBytes();
    arena.RollbackToMarker(arena.Capacity() + 64U);
    EN_TEST_CHECK(stats, arena.UsedBytes() == usedAfterRecarve);

    // Move semantics
    AlignedArena movedArena = std::move(arena);
    EN_TEST_CHECK(stats, movedArena.IsValid() && !arena.IsValid());

    // BinaryWriter & BinaryReader boundary and corruption checks
    std::array<u8, 32> streamBuf{};
    BinaryWriter writer{streamBuf};
    EN_TEST_CHECK(stats, writer.Write<u32>(0x11223344U));
    EN_TEST_CHECK(stats, writer.Write<i64>(-9876543210LL));
    EN_TEST_CHECK(stats, writer.WriteString("EngineN"));
    EN_TEST_CHECK(stats, writer.Ok() && writer.Offset() == 21U);

    BinaryReader reader{writer.WrittenSpan()};
    u32 v32 = 0;
    i64 v64 = 0;
    std::string_view sv{};
    EN_TEST_CHECK(stats, reader.Read(v32) && v32 == 0x11223344U);
    EN_TEST_CHECK(stats, reader.Read(v64) && v64 == -9876543210LL);
    EN_TEST_CHECK(stats, reader.ReadStringView(sv) && sv == "EngineN");
    EN_TEST_CHECK(stats, reader.Remaining() == 0U && reader.Ok());

    // Read past end sets sticky error and does not clobber out-parameter
    u32 untouched = 0xCAFEBABEU;
    EN_TEST_CHECK(stats, !reader.Read(untouched) && untouched == 0xCAFEBABEU && !reader.Ok());

    // Corrupted string length header larger than buffer restores offset and fails
    std::array<u8, 4> badStrBuf{};
    BinaryWriter badWriter{badStrBuf};
    EN_TEST_CHECK(stats, badWriter.Write<u16>(500U));
    BinaryReader badReader{badWriter.WrittenSpan()};
    std::string_view badSv = "initial";
    EN_TEST_CHECK(stats, !badReader.ReadStringView(badSv) && badSv == "initial" && badReader.Offset() == 0U && !badReader.Ok());
}

void TestContainers(TestStats& stats) noexcept {
    AlignedArena arena{};
    EN_TEST_CHECK(stats, arena.Init(256U * 1024U));

    // 1. SlotMap (32B) dense-sparse integrity, capacity limits, Clear() invalidation, and 16-bit generation wrap
    SlotMap<u32> slotMap{};
    EN_TEST_CHECK(stats, !slotMap.Init(arena, 0U));
    EN_TEST_CHECK(stats, !slotMap.Init(arena, SlotMap<u32>::kNullSlot));
    EN_TEST_CHECK(stats, slotMap.Init(arena, 4U));

    const Handle<u32> h0 = slotMap.Allocate(100U);
    const Handle<u32> h1 = slotMap.Allocate(200U);
    const Handle<u32> h2 = slotMap.Allocate(300U);
    const Handle<u32> h3 = slotMap.Allocate(400U);
    const Handle<u32> hOverflow = slotMap.Allocate(500U);
    EN_TEST_CHECK(stats, h0.IsValid() && h1.IsValid() && h2.IsValid() && h3.IsValid());
    EN_TEST_CHECK(stats, !hOverflow.IsValid() && slotMap.Full() && slotMap.ActiveCount() == 4U);

    // Free middle item (h1): dense array swaps last item (400U) into dense index 1
    EN_TEST_CHECK(stats, slotMap.Free(h1));
    EN_TEST_CHECK(stats, !slotMap.Free(h1));
    EN_TEST_CHECK(stats, !slotMap.IsValid(h1) && slotMap.TryGet(h1) == nullptr);
    EN_TEST_CHECK(stats, slotMap.ActiveCount() == 3U);
    EN_TEST_CHECK(stats, *slotMap.TryGet(h0) == 100U);
    EN_TEST_CHECK(stats, *slotMap.TryGet(h2) == 300U);
    EN_TEST_CHECK(stats, *slotMap.TryGet(h3) == 400U);

    // Reallocate into freed sparse slot: generation increments to 2, old handle h1 stays invalid
    const Handle<u32> h1New = slotMap.Allocate(999U);
    EN_TEST_CHECK(stats, h1New.index == h1.index && h1New.generation == 2U);
    EN_TEST_CHECK(stats, !slotMap.IsValid(h1) && *slotMap.TryGet(h1New) == 999U);

    u32 denseSum = 0;
    for (const u32 val : slotMap.Values()) denseSum += val;
    EN_TEST_CHECK(stats, denseSum == (100U + 300U + 400U + 999U));

    // Clear() bumps only active slots and invalidates all live handles
    slotMap.Clear();
    EN_TEST_CHECK(stats, slotMap.Empty() && !slotMap.IsValid(h0) && !slotMap.IsValid(h1New));
    const Handle<u32> hAfterClear = slotMap.Allocate(42U);
    EN_TEST_CHECK(stats, hAfterClear.IsValid() && hAfterClear.generation == 2U && !slotMap.IsValid(h0));

    // Generation 16-bit wrap-around past 0xFFFF skips 0 and lands on 1
    SlotMap<u32> wrapPool{};
    EN_TEST_CHECK(stats, wrapPool.Init(arena, 1U));
    for (u32 cycle = 0; cycle < 65535U; ++cycle) {
        const Handle<u32> h = wrapPool.Allocate(cycle);
        EN_TEST_CHECK(stats, h.IsValid() && wrapPool.Free(h));
    }
    const Handle<u32> wrappedH = wrapPool.Allocate(77U);
    EN_TEST_CHECK(stats, wrappedH.IsValid() && wrappedH.generation == 1U);

    // 2. RobinHoodMap (16B) single-pass upsert, backward-shift deletion, and update-at-max-load
    RobinHoodMap<u32> rhMap{};
    EN_TEST_CHECK(stats, !rhMap.Init(arena, 0U));
    EN_TEST_CHECK(stats, rhMap.Init(arena, 16U));
    EN_TEST_CHECK(stats, rhMap.Capacity() == 16U && rhMap.MaxLoad() == 14U);
    EN_TEST_CHECK(stats, !rhMap.InsertOrAssign(0ULL, 123U));

    for (u32 i = 1U; i <= rhMap.MaxLoad(); ++i) {
        EN_TEST_CHECK(stats, rhMap.InsertOrAssign(static_cast<u64>(i) * 17ULL, i * 10U));
    }
    EN_TEST_CHECK(stats, rhMap.Size() == rhMap.MaxLoad());
    EN_TEST_CHECK(stats, !rhMap.InsertOrAssign(99999ULL, 1U));

    // Single-pass update of existing keys at MaxLoad() must succeed regardless of probe distance
    for (u32 i = 1U; i <= rhMap.MaxLoad(); ++i) {
        EN_TEST_CHECK(stats, rhMap.InsertOrAssign(static_cast<u64>(i) * 17ULL, i * 100U));
        EN_TEST_CHECK(stats, rhMap.Find(static_cast<u64>(i) * 17ULL) != nullptr &&
                             *rhMap.Find(static_cast<u64>(i) * 17ULL) == i * 100U);
    }

    // Erase half the keys and verify remaining keys stay intact via backward-shift
    for (u32 i = 1U; i <= rhMap.MaxLoad(); i += 2U) {
        EN_TEST_CHECK(stats, rhMap.Erase(static_cast<u64>(i) * 17ULL));
        EN_TEST_CHECK(stats, !rhMap.Contains(static_cast<u64>(i) * 17ULL));
    }
    for (u32 i = 2U; i <= rhMap.MaxLoad(); i += 2U) {
        const u32* found = rhMap.Find(static_cast<u64>(i) * 17ULL);
        EN_TEST_CHECK(stats, found != nullptr && *found == i * 100U);
    }

    // 3. SmallVector & RingBuffer
    SmallVector<u32, 4> svec{};
    EN_TEST_CHECK(stats, svec.PushBack(10U) && svec.PushBack(20U) && svec.PushBack(30U) && svec.PushBack(40U));
    EN_TEST_CHECK(stats, svec.Full() && !svec.PushBack(50U));
    EN_TEST_CHECK(stats, svec.EraseSwapBack(1U) && svec.Size() == 3U && svec[1] == 40U);

    RingBuffer<u32, 4> ring{};
    for (u32 cycle = 0; cycle < 10U; ++cycle) {
        EN_TEST_CHECK(stats, ring.Push(cycle * 2U) && ring.Push(cycle * 2U + 1U));
        EN_TEST_CHECK(stats, *ring.PeekFront() == cycle * 2U && *ring.PeekBack() == cycle * 2U + 1U);
        u32 outA = 0, outB = 0;
        EN_TEST_CHECK(stats, ring.Pop(outA) && ring.Pop(outB));
        EN_TEST_CHECK(stats, outA == cycle * 2U && outB == cycle * 2U + 1U && ring.Empty());
    }
}

void TestJobsAndConcurrency(TestStats& stats) noexcept {
    // 1. Chase-Lev WorkStealingDeque single-thread & multi-thread theft verification
    WorkStealingDeque<u32, 64> deque{};
    for (u32 i = 0; i < 64U; ++i) {
        EN_TEST_CHECK(stats, deque.PushBottom(i + 1U));
    }
    EN_TEST_CHECK(stats, !deque.PushBottom(999U));

    u32 stolen = 0;
    u32 popped = 0;
    EN_TEST_CHECK(stats, deque.StealTop(stolen) && stolen == 1U);
    EN_TEST_CHECK(stats, deque.PopBottom(popped) && popped == 64U);
    deque.Clear();
    EN_TEST_CHECK(stats, deque.Empty());

    // Concurrent owner push/pop + 3 thief threads stealing simultaneously: verify exact conservation
    constexpr u32 kTotalItems = 2048U;
    WorkStealingDeque<u32, 256> stressDeque{};
    std::atomic<bool> producerDone{false};
    std::atomic<u32> consumedCount{0};
    std::atomic<u64> consumedSum{0};

    std::array<std::thread, 3> thieves{};
    for (auto& t : thieves) {
        t = std::thread([&]() noexcept {
            for (;;) {
                u32 val = 0;
                if (stressDeque.StealTop(val)) {
                    consumedSum.fetch_add(val, std::memory_order_relaxed);
                    consumedCount.fetch_add(1U, std::memory_order_relaxed);
                } else if (producerDone.load(std::memory_order_acquire) && stressDeque.Empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (u32 i = 1U; i <= kTotalItems; ++i) {
        while (!stressDeque.PushBottom(i)) {
            u32 localVal = 0;
            if (stressDeque.PopBottom(localVal)) {
                consumedSum.fetch_add(localVal, std::memory_order_relaxed);
                consumedCount.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        if ((i & 7U) == 0U) {
            u32 localVal = 0;
            if (stressDeque.PopBottom(localVal)) {
                consumedSum.fetch_add(localVal, std::memory_order_relaxed);
                consumedCount.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }
    {
        u32 localVal = 0;
        while (stressDeque.PopBottom(localVal)) {
            consumedSum.fetch_add(localVal, std::memory_order_relaxed);
            consumedCount.fetch_add(1U, std::memory_order_relaxed);
        }
    }
    producerDone.store(true, std::memory_order_release);
    for (auto& t : thieves) t.join();

    const u64 expectedSum = (static_cast<u64>(kTotalItems) * (kTotalItems + 1ULL)) / 2ULL;
    EN_TEST_CHECK(stats, consumedCount.load() == kTotalItems && consumedSum.load() == expectedSum);

    // 2. JobSystem: ParallelFor & Nested ParallelFor (help-while-waiting deadlock freedom) + Saturation(>256 batches)
    JobSystem jobs{};
    EN_TEST_CHECK(stats, jobs.Init(4U));
    std::atomic<u32> nestedTotal{0};
    jobs.ParallelForRange(32U, 4U, [&](u32 outerStart, u32 outerEnd, u32) noexcept {
        for (u32 i = outerStart; i < outerEnd; ++i) {
            jobs.ParallelForRange(16U, 4U, [&](u32 innerStart, u32 innerEnd, u32) noexcept {
                nestedTotal.fetch_add(innerEnd - innerStart, std::memory_order_relaxed);
            });
        }
    });
    EN_TEST_CHECK(stats, nestedTotal.load() == 32U * 16U);

    // Saturate local deque with > 256 batches (512 batches of size 1)
    std::atomic<u32> saturatedTotal{0};
    jobs.ParallelForRange(512U, 1U, [&](u32 s, u32 e, u32) noexcept {
        saturatedTotal.fetch_add(e - s, std::memory_order_relaxed);
    });
    EN_TEST_CHECK(stats, saturatedTotal.load() == 512U);
    jobs.Shutdown();
}

void TestDiagnostics(TestStats& stats) noexcept {
    using namespace en::literals;

    Logger::Clear();
    Logger::SetMinLevel(LogLevel::Warn);
    Logger::Log(LogLevel::Info, "core/test"_sid, "Filtered info message");
    EN_TEST_CHECK(stats, Logger::TotalLoggedCount() == 0ULL);

    u32 sinkCalls = 0;
    Logger::SetSink([](const LogRecord& rec, void* ctx) noexcept {
        if (rec.level >= LogLevel::Warn && rec.file != nullptr && rec.function != nullptr) {
            ++(*static_cast<u32*>(ctx));
        }
    }, &sinkCalls);

    for (u32 i = 0; i < 80U; ++i) {
        Logger::Log(LogLevel::Warn, "core/test"_sid, "Warning entry");
    }
    EN_TEST_CHECK(stats, sinkCalls == 80U && Logger::TotalLoggedCount() == 80ULL);

    std::array<LogRecord, 64> history{};
    const u32 histCount = Logger::RecentRecords(history);
    EN_TEST_CHECK(stats, histCount == 64U && history.front().sequence == 17ULL && history.back().sequence == 80ULL);
    Logger::SetSink(nullptr, nullptr);
    Logger::SetMinLevel(LogLevel::Info);

    // Custom assertion handler interception
    u32 assertCaught = 0;
    SetAssertHandler([](const char* expr, std::string_view msg, std::source_location loc, void* ctx) noexcept -> bool {
        if (expr != nullptr && msg == "Boundary violation" && loc.line() > 0U) {
            ++(*static_cast<u32*>(ctx));
        }
        return true;
    }, &assertCaught);

    EN_ASSERT(2 + 2 == 4, "Should not fire");
    EN_VERIFY(2 + 2 == 5, "Boundary violation");
    SetAssertHandler(nullptr, nullptr);
    EN_TEST_CHECK(stats, assertCaught == 1U);

    // CrashHandler stack trace recursion overflow (> 32 frames) and small-buffer formatting
    CrashHandler::ResetThreadTrace();
    CrashHandler::Install("/tmp/enginen_core_test_crash.dmp.txt");
    {
        EN_TRACE_SCOPE();
        for (u32 d = 0; d < 40U; ++d) {
            CrashHandler::PushFrame();
        }
        EN_TEST_CHECK(stats, CrashHandler::CurrentDepth() == 41U);

        std::array<TraceFrameEntry, 64> captured{};
        const u32 capCount = CrashHandler::CaptureStackTrace(captured);
        EN_TEST_CHECK(stats, capCount == CrashHandler::kMaxTraceDepth);
        EN_TEST_CHECK(stats, captured[0].depth == 31U);

        std::array<char, 16> tinyReport{};
        const usize tinyLen = CrashHandler::FormatCrashReport("OverflowTest", tinyReport);
        EN_TEST_CHECK(stats, tinyLen == 15U && tinyReport[15] == '\0');

        std::array<char, 2048> fullReport{};
        const usize fullLen = CrashHandler::FormatCrashReport("SignalTest", fullReport);
        EN_TEST_CHECK(stats, fullLen > 100U && CrashHandler::WriteCrashDump("SignalTest"));

        for (u32 d = 0; d < 40U; ++d) {
            CrashHandler::PopFrame();
        }
    }
    EN_TEST_CHECK(stats, CrashHandler::CurrentDepth() == 0U);
}

} // namespace en::tests

int main() {
    using namespace en::tests;
    TestStats stats{};

    TestTypesAndPrimitives(stats);
    TestMath2D(stats);
    TestMemoryAndStreams(stats);
    TestContainers(stats);
    TestJobsAndConcurrency(stats);
    TestDiagnostics(stats);

    std::printf(
        "EngineN Core Foundation Verification Suite: %u passed, %u failed\n",
        stats.passed,
        stats.failed);
    return (stats.failed == 0U) ? 0 : 1;
}

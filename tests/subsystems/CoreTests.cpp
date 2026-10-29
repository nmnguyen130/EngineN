#include "core/Containers.h"
#include "core/Jobs.h"
#include "core/Log.h"
#include "core/Math.h"
#include "core/Memory.h"
#include "core/Serialization.h"
#include "core/Types.h"

#include <atomic>
#include <cmath>
#include <cstdio>

namespace {

using namespace en;
using namespace en::literals;

enum class TestFlags : u8 {
    None = 0,
    Read = 1U << 0,
    Write = 1U << 1,
    Exec = 1U << 2,
};
consteval bool enable_bitmask_operators(TestFlags) { return true; }

struct DummyTag {};
using DummyHandle = Handle<DummyTag>;

bool TestTypesAndHashing() noexcept {
    TestFlags f = TestFlags::Read | TestFlags::Write;
    if (!HasFlag(f, TestFlags::Read) || HasFlag(f, TestFlags::Exec)) return false;
    f ^= TestFlags::Write;
    if (HasFlag(f, TestFlags::Write)) return false;

    if (NextPow2(0U) != 1U || NextPow2(1U) != 1U || NextPow2(17U) != 32U || NextPow2(0x80000001U) != 0U) {
        return false;
    }
    if (AlignUp(13U, 8U) != 16U || AlignUp(16U, 8U) != 16U || AlignUp(10U, 3U) != 0U) {
        return false;
    }

    constexpr StringId sidA = "Assets\\Sprites/Hero.PNG"_sid;
    constexpr StringId sidB = "assets/sprites/hero.png"_sid;
    if (!sidA.IsValid() || sidA != sidB || StringId{}.IsValid()) return false;

    constexpr DummyHandle h{42U, 7U};
    if (!h.IsValid() || DummyHandle::FromPacked(h.Packed()) != h) return false;

    FixedString<16> str("hello");
    if (str.Size() != 5U || str != "hello" || !str.Append("_world") || str != "hello_world") {
        return false;
    }
    if (str.Append("_overflow_check") || str != "hello_world") return false;

    const u8 data[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (Crc32(data) != 0xCBF43926U) return false;

    Rng rngA(12345ULL);
    Rng rngB(12345ULL);
    for (int i = 0; i < 32; ++i) {
        if (rngA.NextU64() != rngB.NextU64()) return false;
        const u32 b = rngA.Bounded(10U);
        (void)rngB.Bounded(10U);
        if (b >= 10U) return false;
        const i32 r = rngA.Range(-5, 5);
        (void)rngB.Range(-5, 5);
        if (r < -5 || r > 5) return false;
    }
    return true;
}

bool TestMath() noexcept {
    if (Approach(0.0f, 10.0f, 3.0f) != 3.0f || Approach(9.0f, 10.0f, 3.0f) != 10.0f ||
        Approach(10.0f, 0.0f, 4.0f) != 6.0f) {
        return false;
    }

    f32 c = 0.0f, s = 0.0f;
    FastSinCos(kHalfPi, c, s);
    if (std::abs(c) > 1e-4f || std::abs(s - 1.0f) > 1e-4f) return false;

    const Vec2 a{3.0f, 4.0f};
    if (std::abs(a.Length() - 5.0f) > 1e-5f) return false;
    const Vec2 n = a.Normalized();
    if (std::abs(n.Length() - 1.0f) > 1e-5f) return false;

    const Rect r1{0.0f, 0.0f, 10.0f, 10.0f};
    const Rect r2{5.0f, 5.0f, 10.0f, 10.0f};
    if (!r1.Overlaps(r2) || r1.Intersection(r2) != Rect{5.0f, 5.0f, 5.0f, 5.0f}) return false;

    const Mat3x2 trs = Mat3x2::FromTRS({10.0f, 20.0f}, 0.5f, {2.0f, 3.0f});
    const Mat3x2 inv = trs.Inverse();
    const Vec2 pt{4.0f, -7.0f};
    const Vec2 roundTrip = inv.TransformPoint(trs.TransformPoint(pt));
    if (std::abs(roundTrip.x - pt.x) > 1e-3f || std::abs(roundTrip.y - pt.y) > 1e-3f) return false;

    f32 pos = 0.0f, vel = 0.0f;
    for (int i = 0; i < 120; ++i) StepSpring(pos, vel, 10.0f, 12.0f, 1.0f / 120.0f);
    if (std::abs(pos - 10.0f) > 0.05f) return false;

    return true;
}

bool TestMemoryAndSerialization() noexcept {
    Arena arena;
    if (!arena.Init(4096U) || !arena.IsValid()) return false;

    auto ints = arena.Alloc<u32>(16U);
    if (ints.size() != 16U || arena.Used() == 0U) return false;
    ints[0] = 0xDEADBEEFU;

    const u32 mark = arena.Mark();
    {
        ArenaScope temp(arena);
        auto scratch = arena.Alloc<u64>(32U);
        if (scratch.empty() || arena.Used() <= mark) return false;
        scratch[0] = 999ULL;
    }
    if (arena.Used() != mark || ints[0] != 0xDEADBEEFU) return false;

    std::array<u8, 64> buf{};
    BinaryWriter writer(buf);
    if (!writer.Write<u32>(0x12345678U) || !writer.WriteStr("metroid") || !writer.Ok()) {
        return false;
    }

    BinaryReader reader(writer.Written());
    u32 val = 0U;
    std::string_view sv{};
    if (!reader.Read(val) || val != 0x12345678U || !reader.ReadStr(sv) || sv != "metroid" ||
        reader.Remaining() != 0U) {
        return false;
    }
    return true;
}

bool TestContainers() noexcept {
    Arena arena;
    if (!arena.Init(65536U)) return false;

    HashMap<u32> map;
    if (!map.Init(arena, 64U)) return false;
    for (u32 i = 1U; i <= 40U; ++i) {
        if (!map.Insert(i * 100ULL, i)) return false;
    }
    if (map.Size() != 40U) return false;
    for (u32 i = 1U; i <= 40U; ++i) {
        const u32* found = map.Find(i * 100ULL);
        if (!found || *found != i) return false;
    }
    for (u32 i = 1U; i <= 20U; ++i) {
        if (!map.Erase(i * 100ULL) || map.Contains(i * 100ULL)) return false;
    }
    for (u32 i = 21U; i <= 40U; ++i) {
        if (!map.Contains(i * 100ULL)) return false;
    }

    RingBuffer<u32, 8> ring;
    for (u32 i = 0U; i < 8U; ++i) {
        if (!ring.Push(i)) return false;
    }
    if (!ring.Full() || ring.Push(99U)) return false;
    for (u32 i = 0U; i < 8U; ++i) {
        u32 out = 0U;
        if (!ring.Pop(out) || out != i) return false;
    }
    return ring.Empty();
}

bool TestLogAndJobs() noexcept {
    static u32 s_logCount = 0U;
    Log::SetLevel(LogLevel::Info);
    Log::SetSink([](const LogEntry& e, void*) noexcept {
        if (e.level == LogLevel::Warn) ++s_logCount;
    });
    Log::Info("test"_sid, "ignored by sink counter");
    Log::Warn("test"_sid, "counted warn");
    Log::SetSink(nullptr);
    if (s_logCount != 1U) return false;

    SetAbortOnAssert(false);
    EN_VERIFY(1 == 2, "expected test failure");

    JobSystem jobs;
    if (!jobs.Init(4U)) return false;
    std::atomic<u64> sum{0ULL};
    jobs.ParallelFor(1000U, 32U, [&](u32 start, u32 end, u32) noexcept {
        u64 local = 0ULL;
        for (u32 i = start; i < end; ++i) local += (i + 1U);
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    jobs.Shutdown();
    return sum.load() == (1000ULL * 1001ULL) / 2ULL;
}

} // namespace

int main() {
    if (!TestTypesAndHashing()) return 1;
    if (!TestMath()) return 2;
    if (!TestMemoryAndSerialization()) return 3;
    if (!TestContainers()) return 4;
    if (!TestLogAndJobs()) return 5;
    std::printf("CoreTests: ALL PASSED\n");
    return 0;
}

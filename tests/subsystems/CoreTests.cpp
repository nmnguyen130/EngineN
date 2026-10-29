#include "tests/subsystems/CtsHarness.h"

#include "core/Containers.h"
#include "core/Jobs.h"
#include "core/Log.h"
#include "core/Math.h"
#include "core/Memory.h"
#include "core/Serialization.h"
#include "core/Types.h"

#include <atomic>
#include <deque>
#include <unordered_map>
#include <vector>

namespace {

using namespace en;
using namespace en::cts;
using namespace en::literals;

enum class CtsPerms : u8 {
    None  = 0,
    Read  = 1U << 0,
    Write = 1U << 1,
    Exec  = 1U << 2,
};
consteval bool enable_bitmask_operators(CtsPerms) { return true; }

struct CtsTag {};
using CtsHandle = Handle<CtsTag>;

bool CtsPrimitivesHashingAndStrings(CtsRunner& r) noexcept {
    CtsPerms p = CtsPerms::Read | CtsPerms::Write;
    CTS_CHECK(r, HasFlag(p, CtsPerms::Read) && HasFlag(p, CtsPerms::Write) && !HasFlag(p, CtsPerms::Exec));
    p ^= CtsPerms::Write;
    p |= CtsPerms::Exec;
    p &= ~CtsPerms::Read;
    CTS_CHECK(r, p == CtsPerms::Exec);

    // Exhaustive power-of-two and alignment boundary checks.
    CTS_CHECK(r, NextPow2(0U) == 1U && NextPow2(1U) == 1U && NextPow2(2U) == 2U);
    CTS_CHECK(r, NextPow2(3U) == 4U && NextPow2(1023U) == 1024U && NextPow2(0x80000000U) == 0x80000000U);
    CTS_CHECK(r, NextPow2(0x80000001U) == 0U && NextPow2(0xFFFFFFFFU) == 0U);

    CTS_CHECK(r, AlignUp(0U, 64U) == 0U && AlignUp(1U, 64U) == 64U && AlignUp(64U, 64U) == 64U);
    CTS_CHECK(r, AlignUp(100U, 0U) == 0U && AlignUp(100U, 3U) == 0U && AlignUp(~usize{0}, 16U) == 0U);

    // Case-insensitive + path-separator equivalence classes for StringId.
    constexpr StringId s1 = "Data\\Sprites//Kaelen_Idle.PNG"_sid;
    constexpr StringId s2 = "data/sprites//kaelen_idle.png"_sid;
    CTS_CHECK(r, s1.IsValid() && s1 == s2 && !StringId{}.IsValid());

    // Generational Handle pack/unpack across boundary indices and generations.
    for (u32 idx : {0U, 1U, 255U, 32768U, 65535U}) {
        for (u32 gen : {0U, 1U, 127U, 65535U}) {
            const CtsHandle h{static_cast<u16>(idx), static_cast<u16>(gen)};
            CTS_CHECK(r, h.IsValid() == (gen != 0U));
            CTS_CHECK(r, CtsHandle::FromPacked(h.Packed()) == h);
        }
    }

    // FixedString<16> has kMaxLength = 13: "kaelen" (6) + "_123456" (7) = 13.
    FixedString<16> fs("kaelen");
    CTS_CHECK(r, fs.Size() == 6U && fs == "kaelen");
    CTS_CHECK(r, fs.Append("_123456") && fs.Size() == FixedString<16>::kMaxLength);
    CTS_CHECK(r, !fs.Append("x") && fs == "kaelen_123456");
    CTS_CHECK(r, !fs.Assign({"bad\0str", 7U}) && fs == "kaelen_123456");
    fs.Clear();
    CTS_CHECK(r, fs.Empty() && fs.Size() == 0U && fs.CStr()[0] == '\0');

    // IEEE 802.3 CRC32 single-pass vs chunked streaming equivalence.
    const u8 checkVec[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const u32 fullCrc = Crc32(checkVec);
    CTS_CHECK(r, fullCrc == 0xCBF43926U);
    const u32 part1 = Crc32({checkVec, 4U});
    const u32 part2 = Crc32({checkVec + 4U, 5U}, part1);
    CTS_CHECK(r, part2 == fullCrc);

    // Deterministic SplitMix64 PRNG lockstep and unbiased Lemire bounded distribution check.
    Rng rngA(0xCAFEBABE12345678ULL);
    Rng rngB(0xCAFEBABE12345678ULL);
    std::array<u32, 7> buckets{};
    for (u32 i = 0U; i < 7000U; ++i) {
        CTS_CHECK(r, rngA.NextU64() == rngB.NextU64());
        const u32 b = rngA.Bounded(7U);
        (void)rngB.Bounded(7U);
        CTS_CHECK(r, b < 7U);
        ++buckets[b];
        const f32 rf = rngA.Range(-10.0f, 10.0f);
        (void)rngB.Range(-10.0f, 10.0f);
        CTS_CHECK(r, rf >= -10.0f && rf <= 10.0f);
        const i32 ri = rngA.Range(-15, 15);
        (void)rngB.Range(-15, 15);
        CTS_CHECK(r, ri >= -15 && ri <= 15);
    }
    for (u32 count : buckets) {
        CTS_CHECK(r, count > 850U && count < 1150U);
    }
    r.RecordFuzzOps(7000U);
    r.MixDigest(rngA.State());
    return true;
}

bool CtsMathGeometryAndSweptCollision(CtsRunner& r) noexcept {
    // Sweep FastSinCos across 2,001 angles in [-20*pi, +20*pi] against std::cos/std::sin.
    for (i32 i = -1000; i <= 1000; ++i) {
        const f32 rad = static_cast<f32>(i) * 0.062831853f;
        f32 c = 0.0f, s = 0.0f;
        FastSinCos(rad, c, s);
        CTS_CHECK(r, Near(c, std::cos(rad), 2e-4f));
        CTS_CHECK(r, Near(s, std::sin(rad), 2e-4f));
        CTS_CHECK(r, Near(c * c + s * s, 1.0f, 3e-4f));
    }
    r.RecordFuzzOps(2001U);

    // Mat3x2 composition, determinant, and inverse round-trip across 200 random transforms.
    Rng rng(9001ULL);
    for (u32 i = 0U; i < 200U; ++i) {
        const Vec2 pos{rng.Range(-500.0f, 500.0f), rng.Range(-500.0f, 500.0f)};
        const f32  rot = rng.Range(-kPi, kPi);
        const Vec2 scl{rng.Range(0.5f, 4.0f), rng.Range(0.5f, 4.0f)};
        const Mat3x2 m   = Mat3x2::FromTRS(pos, rot, scl);
        const Mat3x2 inv = m.Inverse();
        const Vec2 p{rng.Range(-50.0f, 50.0f), rng.Range(-50.0f, 50.0f)};
        const Vec2 rt = inv.TransformPoint(m.TransformPoint(p));
        CTS_CHECK(r, Near(rt.x, p.x, 2e-3f) && Near(rt.y, p.y, 2e-3f));
    }

    // Critically damped spring monotonic convergence without overshoot from rest.
    f32 pos = 0.0f, vel = 0.0f;
    f32 prevPos = 0.0f;
    for (u32 tick = 0U; tick < 240U; ++tick) {
        StepSpring(pos, vel, 100.0f, 15.0f, 1.0f / 120.0f);
        CTS_CHECK(r, pos >= prevPos - 1e-4f && pos <= 100.001f);
        prevPos = pos;
    }
    CTS_CHECK(r, Near(pos, 100.0f, 1e-2f) && Near(vel, 0.0f, 1e-1f));

    // Swept AABB continuous collision across all 4 cardinal directions + grazing miss.
    const Rect wall{10.0f, 10.0f, 4.0f, 4.0f};
    const SweepHit hitLeft = SweepRect({4.0f, 11.0f, 2.0f, 2.0f}, {8.0f, 0.0f}, wall);
    CTS_CHECK(r, hitLeft && Near(hitLeft.time, 0.5f) && hitLeft.normal == Vec2{-1.0f, 0.0f});

    const SweepHit hitRight = SweepRect({18.0f, 11.0f, 2.0f, 2.0f}, {-8.0f, 0.0f}, wall);
    CTS_CHECK(r, hitRight && Near(hitRight.time, 0.5f) && hitRight.normal == Vec2{1.0f, 0.0f});

    const SweepHit hitTop = SweepRect({11.0f, 4.0f, 2.0f, 2.0f}, {0.0f, 8.0f}, wall);
    CTS_CHECK(r, hitTop && Near(hitTop.time, 0.5f) && hitTop.normal == Vec2{0.0f, -1.0f});

    const SweepHit hitBot = SweepRect({11.0f, 18.0f, 2.0f, 2.0f}, {0.0f, -8.0f}, wall);
    CTS_CHECK(r, hitBot && Near(hitBot.time, 0.5f) && hitBot.normal == Vec2{0.0f, 1.0f});

    // Exact tangent slide past wall top edge (y + h == wall.y) must NOT collide.
    const SweepHit graze = SweepRect({4.0f, 8.0f, 2.0f, 2.0f}, {16.0f, 0.0f}, wall);
    CTS_CHECK(r, !graze);
    return true;
}

bool CtsArenaAndBinarySerialization(CtsRunner& r) noexcept {
    Arena arena;
    CTS_CHECK(r, !arena.Init(0U) && !arena.Init(Arena::kMaxBytes + 64U));
    CTS_CHECK(r, arena.Init(65536U) && arena.IsValid());

    for (usize align : {8U, 16U, 32U, 64U}) {
        u8* p = arena.AllocBytes(13U, align);
        CTS_CHECK(r, p != nullptr && (reinterpret_cast<std::uintptr_t>(p) % align) == 0U);
    }

    // Verify ArenaScope rewinds and strictly zeroes rolled-back memory.
    const u32 mark = arena.Mark();
    u8* rawScratch = nullptr;
    {
        ArenaScope scope(arena);
        auto span = arena.Alloc<u64>(64U);
        CTS_CHECK(r, span.size() == 64U);
        rawScratch = reinterpret_cast<u8*>(span.data());
        for (u64& v : span) v = 0xFFFFFFFFFFFFFFFFULL;
    }
    CTS_CHECK(r, arena.Used() == mark);
    for (usize i = 0U; i < 64U * sizeof(u64); ++i) {
        CTS_CHECK(r, rawScratch[i] == 0U);
    }

    // Randomized BinaryWriter / BinaryReader round-trip fuzzing (200 * 23 = 4,600 bytes).
    std::array<u8, 8192> buf{};
    BinaryWriter w(buf);
    Rng rng(0x1337BEEFULL);
    for (u32 i = 0U; i < 200U; ++i) {
        CTS_CHECK(r, w.Write(rng.NextU64()));
        CTS_CHECK(r, w.Write(rng.NextF32()));
        CTS_CHECK(r, w.WriteStr("veil_echo"));
    }
    CTS_CHECK(r, w.Ok());

    BinaryReader rd(w.Written());
    Rng verifyRng(0x1337BEEFULL);
    for (u32 i = 0U; i < 200U; ++i) {
        u64 u = 0ULL;
        f32 f = 0.0f;
        std::string_view sv{};
        CTS_CHECK(r, rd.Read(u) && u == verifyRng.NextU64());
        CTS_CHECK(r, rd.Read(f) && f == verifyRng.NextF32());
        CTS_CHECK(r, rd.ReadStr(sv) && sv == "veil_echo");
    }
    CTS_CHECK(r, rd.Remaining() == 0U && rd.Ok());
    u8 extra = 0U;
    CTS_CHECK(r, !rd.Read(extra) && !rd.Ok());
    r.RecordFuzzOps(400U);
    return true;
}

bool DstContainersDifferentialFuzz(CtsRunner& r) noexcept {
    Arena arena;
    CTS_CHECK(r, arena.Init(256U * 1024U));

    HashMap<u32> map;
    CTS_CHECK(r, map.Init(arena, 256U));
    std::unordered_map<u64, u32> oracle;

    Rng rng(0xA5A5F0F011223344ULL);
    const u32 maxLoad = map.MaxLoad();

    // 15,000 randomized Insert / Update / Find / Erase operations against std::unordered_map oracle.
    for (u32 step = 0U; step < 15000U; ++step) {
        const u32 op  = rng.Bounded(100U);
        const u64 key = static_cast<u64>(rng.Bounded(320U)) + 1ULL;

        if (op < 50U) {
            const u32 val = rng.NextU32();
            const bool exists = oracle.contains(key);
            if (!exists && oracle.size() >= maxLoad) {
                CTS_CHECK(r, !map.Insert(key, val));
            } else {
                CTS_CHECK(r, map.Insert(key, val));
                oracle[key] = val;
            }
        } else if (op < 80U) {
            const bool erasedMap = map.Erase(key);
            const bool erasedOra = (oracle.erase(key) > 0U);
            CTS_CHECK(r, erasedMap == erasedOra);
        } else {
            const u32* found = map.Find(key);
            const auto it    = oracle.find(key);
            if (it == oracle.end()) {
                CTS_CHECK(r, found == nullptr);
            } else {
                CTS_CHECK(r, found != nullptr && *found == it->second);
            }
        }
        CTS_CHECK(r, map.Size() == oracle.size());
    }

    for (const auto& [k, v] : oracle) {
        const u32* found = map.Find(k);
        CTS_CHECK(r, found != nullptr && *found == v);
    }

    // Differential fuzzing for RingBuffer<u32, 64> against std::deque<u32> across 10,000 steps.
    RingBuffer<u32, 64> ring;
    std::deque<u32> dq;
    for (u32 step = 0U; step < 10000U; ++step) {
        if (rng.Bounded(2U) == 0U) {
            const u32 val = rng.NextU32();
            if (dq.size() == 64U) {
                CTS_CHECK(r, ring.Full() && !ring.Push(val));
            } else {
                CTS_CHECK(r, ring.Push(val));
                dq.push_back(val);
            }
        } else {
            u32 popped = 0U;
            if (dq.empty()) {
                CTS_CHECK(r, ring.Empty() && !ring.Pop(popped));
            } else {
                CTS_CHECK(r, ring.Pop(popped) && popped == dq.front());
                dq.pop_front();
            }
        }
        CTS_CHECK(r, ring.Size() == dq.size());
        if (!dq.empty()) {
            CTS_CHECK(r, *ring.Front() == dq.front() && *ring.Back() == dq.back());
        }
    }

    r.RecordFuzzOps(25000U);
    r.MixDigest(map.Size() ^ (static_cast<u64>(ring.Size()) << 32U));
    return true;
}

bool DstJobsParallelStress(CtsRunner& r) noexcept {
    static u32 s_warnCount = 0U;
    s_warnCount = 0U;
    Log::SetLevel(LogLevel::Warn);
    Log::SetSink([](const LogEntry& e, void*) noexcept {
        if (e.level >= LogLevel::Warn) ++s_warnCount;
    });
    Log::Info("cts/core"_sid, "filtered out");
    Log::Warn("cts/core"_sid, "captured warning");
    Log::SetSink(nullptr);
    CTS_CHECK(r, s_warnCount == 1U);

    SetAbortOnAssert(false);
    static u32 s_assertCount = 0U;
    s_assertCount = 0U;
    SetAssertHandler([](const char*, std::string_view, std::source_location, void*) noexcept {
        ++s_assertCount;
        return false;
    });
    EN_VERIFY(2 + 2 == 5, "intentional CTS verify check");
    SetAssertHandler(nullptr);
    CTS_CHECK(r, s_assertCount == 1U);

    // Test JobSystem determinism across worker counts {1, 2, 4, 8} and batch granularities {1, 7, 64}.
    constexpr u32 kItems = 4096U;
    std::vector<u64> baseline(kItems, 0ULL);
    for (u32 i = 0U; i < kItems; ++i) {
        baseline[i] = Mix64(static_cast<u64>(i) + 1ULL);
    }

    for (u32 workers : {1U, 2U, 4U, 8U}) {
        JobSystem jobs;
        CTS_CHECK(r, jobs.Init(workers));
        for (u32 batch : {1U, 7U, 64U}) {
            std::vector<u64> out(kItems, 0ULL);
            std::atomic<u32> nestedCalls{0U};
            jobs.ParallelFor(kItems, batch, [&](u32 start, u32 end, u32) noexcept {
                for (u32 i = start; i < end; ++i) {
                    out[i] = Mix64(static_cast<u64>(i) + 1ULL);
                }
                if ((start & 255U) == 0U) {
                    jobs.ParallelFor(8U, 2U, [&](u32 s, u32 e, u32) noexcept {
                        nestedCalls.fetch_add(e - s, std::memory_order_relaxed);
                    });
                }
            });
            CTS_CHECK(r, out == baseline && nestedCalls.load() > 0U);
            r.RecordFuzzOps(kItems);
        }
        jobs.Shutdown();
    }
    r.MixDigest(baseline.back());
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "CoreTests (CTS+DST)"};
    runner.RunCase("Primitives, Hashing, FixedString & PRNG", CtsPrimitivesHashingAndStrings);
    runner.RunCase("2D Math, Matrices, Springs & Swept AABB", CtsMathGeometryAndSweptCollision);
    runner.RunCase("Arena Scope Zeroing & Binary Serialization", CtsArenaAndBinarySerialization);
    runner.RunCase("DST: Robin Hood HashMap & RingBuffer Fuzz", DstContainersDifferentialFuzz);
    runner.RunCase("DST: Multi-Worker JobSystem Stress", DstJobsParallelStress);
    return runner.Finish();
}

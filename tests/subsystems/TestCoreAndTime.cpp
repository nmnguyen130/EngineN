#include "tests/TestRunner.h"
#include "core/Containers.h"
#include "core/CrashAndCoroutine.h"
#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Memory.h"
#include "core/Serialization.h"
#include "time/TimePipeline.h"

namespace en::tests {

void RunSuiteCoreAndTime(SuiteStats& stats) noexcept {
    using namespace en::literals;

    // 1. Core StringId, Handle, Math2D, Bezier, Hermite, PCG32
    constexpr StringId sid = "enginen_core_test"_sid;
    EN_CHECK(stats, sid.IsValid() && sid == "enginen_core_test"_sid);

    const Vec2 v{3.0f, 4.0f};
    EN_CHECK(stats, std::abs(v.Length() - 5.0f) < 1e-4f);
    const Mat3x2 trs = Mat3x2::FromTRS({10.0f, 20.0f}, 0.0f, {2.0f, 3.0f});
    const Vec2 pt = trs.TransformPoint({5.0f, 4.0f});
    EN_CHECK(stats, std::abs(pt.x - 20.0f) < 1e-4f && std::abs(pt.y - 32.0f) < 1e-4f);

    const Vec2 bez = CubicBezier2D({0.0f, 0.0f}, {0.0f, 10.0f}, {10.0f, 10.0f}, {10.0f, 0.0f}, 0.5f);
    EN_CHECK(stats, bez.x > 4.0f && bez.y > 5.0f);

    Rng64 rng{1337ULL};
    const f32 r01 = rng.NextF32();
    EN_CHECK(stats, r01 >= 0.0f && r01 < 1.0f);

    // 2. AlignedArena (-Wstringop-overflow safe bounds), ArenaTempScope, PoolAllocator, RingBuffer
    AlignedArena arena{};
    EN_CHECK(stats, !arena.Init(0));
    EN_CHECK(stats, !arena.Init(AlignedArena::kMaxArenaBytes + 64U));
    EN_CHECK(stats, arena.Init(128 * 1024));
    auto floats = arena.Carve<f32>(256);
    EN_CHECK(stats, floats.size() == 256U);
    EN_CHECK(stats, arena.Carve<f32>(AlignedArena::kMaxArenaBytes).empty());

    const usize savedUsed = arena.UsedBytes();
    {
        ArenaTempScope tempScope{arena};
        (void)arena.Carve<u64>(64);
        EN_CHECK(stats, arena.UsedBytes() > savedUsed);
    }
    EN_CHECK(stats, arena.UsedBytes() == savedUsed);

    PoolAllocator<Vec4> pool{};
    EN_CHECK(stats, pool.Init(arena, 8));
    const Handle<Vec4> hItem = pool.Allocate(Vec4{1.0f, 2.0f, 3.0f, 4.0f});
    EN_CHECK(stats, hItem.IsValid() && pool.ActiveCount() == 1U);
    EN_CHECK(stats, pool.Free(hItem) && pool.ActiveCount() == 0U);

    // 3. Containers: SmallVector, RobinHoodMap, Bitset256, RingBuffer, EventBus
    SmallVector<u32, 8> svec{};
    EN_CHECK(stats, svec.PushBack(11U) && svec.PushBack(22U) && svec.Size() == 2U);

    RobinHoodMap<u32> rhMap{};
    EN_CHECK(stats, rhMap.Init(arena, 32));
    EN_CHECK(stats, rhMap.InsertOrAssign(0xABCULL, 777U));
    EN_CHECK(stats, rhMap.Find(0xABCULL) != nullptr && *rhMap.Find(0xABCULL) == 777U);
    EN_CHECK(stats, rhMap.Erase(0xABCULL) && rhMap.Find(0xABCULL) == nullptr);

    Bitset256 bits{};
    bits.Set(199);
    EN_CHECK(stats, bits.Test(199) && bits.PopCount() == 1U);

    RingBuffer<u32, 16> ring{};
    EN_CHECK(stats, ring.Push(99U));
    u32 popped = 0;
    EN_CHECK(stats, ring.Pop(popped) && popped == 99U);

    // 4. Serialization: BinaryWriter, BinaryReader, CRC32, PropertyBag
    std::array<u8, 256> rawBuf{};
    BinaryWriter bw{rawBuf};
    EN_CHECK(stats, bw.Write<u32>(0xCAFEBABEU) && bw.Write<f32>(3.5f));
    BinaryReader br{bw.WrittenSpan()};
    u32 magic = 0;
    f32 fval = 0.0f;
    EN_CHECK(stats, br.Read(magic) && br.Read(fval) && magic == 0xCAFEBABEU && std::abs(fval - 3.5f) < 1e-4f);

    PropertyBag props{};
    EN_CHECK(stats, props.SetInt("hp"_sid, 120) && props.GetInt("hp"_sid) == 120);

    // 5. JobSystem ParallelFor
    JobSystem jobs{};
    EN_CHECK(stats, jobs.Init(2));
    std::atomic<u32> counter{0};
    jobs.ParallelFor(64, 8, [](u32 begin, u32 end, u32, void* ctx) noexcept {
        auto* c = static_cast<std::atomic<u32>*>(ctx);
        c->fetch_add(end - begin, std::memory_order_relaxed);
    }, &counter);
    EN_CHECK(stats, counter.load() == 64U);
    jobs.Shutdown();

    // 6. TimePipeline: 120Hz rational accumulator, Pause/RequestStep, Hitstop, TimeRamp, TimerWheel, TweenManager
    TimePipeline time{};
    time.BeginFrameManual(kFixedDtNs * 2ULL);
    SimStepView step1 = time.PollTick();
    SimStepView step2 = time.PollTick();
    EN_CHECK(stats, step1.valid && step2.valid && !time.PollTick().valid);
    EN_CHECK(stats, step2.tickIndex == 2ULL && std::abs(step2.Dt(TimeChannel::World) - kFixedDtSec) < 1e-5f);

    time.SetPaused(true);
    EN_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == kGameplayChannelsMask);
    time.RequestStep(1);
    EN_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == 0U);
    time.BeginFrameManual(kFixedDtNs);
    SimStepView pausedStep = time.PollTick();
    EN_CHECK(stats, pausedStep.valid && !pausedStep.IsFrozen(TimeChannel::Player));
    EN_CHECK(stats, (time.FrozenMask() & kGameplayChannelsMask) == kGameplayChannelsMask);
    time.SetPaused(false);

    TimerWheel wheel{};
    EN_CHECK(stats, wheel.Schedule("cooldown"_sid, 2, false));
    std::array<StringId, 4> fired{};
    EN_CHECK(stats, wheel.TickAndCollectFired(fired) == 0U);
    EN_CHECK(stats, wheel.TickAndCollectFired(fired) == 1U && fired[0] == "cooldown"_sid);

    TweenManager tweens{};
    f32 alpha = 0.0f;
    EN_CHECK(stats, tweens.StartTween(&alpha, 0.0f, 10.0f, 1.0f, TweenManager::Curve::SmoothStep));
    tweens.Step(0.5f);
    EN_CHECK(stats, std::abs(alpha - 5.0f) < 0.1f);
    tweens.Step(0.5f);
    EN_CHECK(stats, std::abs(alpha - 10.0f) < 1e-4f);

    // 7. Hierarchical TimeDilationGroup (World scale cascades to Player/Enemy/Projectile/Vfx/Camera, Ui stays 1.0f)
    time.SetBaseScale(TimeChannel::World, 0.25f);
    time.BeginFrameManual(kFixedDtNs);
    const SimStepView hStep = time.PollTick();
    EN_CHECK(stats, std::abs(hStep.EffectiveScale(TimeChannel::World) - 0.25f) < 1e-4f);
    EN_CHECK(stats, std::abs(hStep.EffectiveScale(TimeChannel::Player) - 0.25f) < 1e-4f);
    EN_CHECK(stats, std::abs(hStep.EffectiveScale(TimeChannel::Enemy) - 0.25f) < 1e-4f);
    EN_CHECK(stats, std::abs(hStep.EffectiveScale(TimeChannel::Ui) - 1.0f) < 1e-4f);
    time.SetBaseScale(TimeChannel::World, 1.0f);

    // 8. FramePacer hybrid sleep/spin-lock FPS limiter
    FramePacer pacer{240};
    EN_CHECK(stats, pacer.TargetFps() == 240U && pacer.TargetFrameNs() > 0ULL);
    pacer.BeginFrame();
    pacer.EndFrameAndPace();

    // 9. CrashHandler stack trace capture & CoroutineScheduler multi-frame execution
    CrashHandler::Install();
    {
        EN_TRACE_SCOPE();
        std::array<TraceFrameEntry, 8> trace{};
        const u32 depth = CrashHandler::CaptureStackTrace(trace);
        EN_CHECK(stats, depth >= 1U && trace[0].functionName != nullptr);
        std::array<char, 512> report{};
        EN_CHECK(stats, CrashHandler::FormatCrashReport("UnitTestCheck", report) > 20U);
    }

    CoroutineScheduler coros{};
    u32 coroCounter = 0;
    EN_CHECK(stats, coros.Spawn("test_coro"_sid, [](u32& stage, void* ctx) noexcept -> CoroutineWait {
        auto* c = static_cast<u32*>(ctx);
        if (stage == 0) {
            ++(*c);
            stage = 1;
            return CoroutineWait::Ticks(2);
        }
        ++(*c);
        return CoroutineWait::Done();
    }, &coroCounter));
    coros.Step(kFixedDtSec);
    EN_CHECK(stats, coroCounter == 1U && coros.ActiveCount() == 1U);
    coros.Step(kFixedDtSec);
    coros.Step(kFixedDtSec);
    EN_CHECK(stats, coroCounter == 2U && coros.ActiveCount() == 0U);
}

} // namespace en::tests

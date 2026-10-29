#pragma once

#include "core/Types.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace en {

// Chase-Lev single-producer multi-consumer circular work-stealing deque.
// Owner thread calls PushBottom and PopBottom; worker threads call StealTop concurrently.
// Invariant: CapacityPow2 is a power of two and m_top/m_bottom sit on separate cache lines.
template <typename T, usize CapacityPow2 = 256>
    requires (std::has_single_bit(CapacityPow2) && std::is_trivially_copyable_v<T>)
class WorkStealingDeque {
public:
    constexpr WorkStealingDeque() noexcept = default;
    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    [[nodiscard]] bool PushBottom(const T& item) noexcept {
        const i64 b = m_bottom.load(std::memory_order_relaxed);
        const i64 t = m_top.load(std::memory_order_acquire);
        if (b - t >= static_cast<i64>(CapacityPow2)) return false;
        m_buffer[static_cast<usize>(b) & kMask] = item;
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool PopBottom(T& outItem) noexcept {
        const i64 b = m_bottom.load(std::memory_order_relaxed) - 1;
        m_bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 t = m_top.load(std::memory_order_relaxed);

        if (t > b) {
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }
        outItem = m_buffer[static_cast<usize>(b) & kMask];
        if (t != b) return true;

        const bool won = m_top.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        m_bottom.store(b + 1, std::memory_order_relaxed);
        return won;
    }

    [[nodiscard]] bool StealTop(T& outItem) noexcept {
        i64 t = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const i64 b = m_bottom.load(std::memory_order_acquire);
        if (t >= b) return false;
        outItem = m_buffer[static_cast<usize>(t) & kMask];
        return m_top.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
    }

    void Clear() noexcept {
        m_top.store(0, std::memory_order_relaxed);
        m_bottom.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool Empty() const noexcept {
        return m_bottom.load(std::memory_order_acquire) <= m_top.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr usize Capacity() noexcept { return CapacityPow2; }

private:
    static constexpr usize kMask = CapacityPow2 - 1U;

    alignas(64) std::atomic<i64> m_top{0};
    alignas(64) std::atomic<i64> m_bottom{0};
    std::array<T, CapacityPow2> m_buffer{};
};

// Work-stealing parallel batch dispatcher with help-while-waiting and single-thread fallback.
// Non-copyable owner of worker threads; uses stack-resident completion counters for zero-allocation nesting.
// Invariant: m_workerCount <= kMaxWorkers; queue 0 belongs to the main thread and 1..N to workers.
class JobSystem {
public:
    static constexpr u32 kMaxWorkers = 16;
    static constexpr u32 kQueueCount = kMaxWorkers + 1U;
    static constexpr usize kDequeCapacity = 256;

    using TaskFn = void (*)(u32 startIdx, u32 endIdx, u32 threadIdx, void* userCtx) noexcept;

    struct alignas(32) JobItem {
        TaskFn            fn{nullptr};
        void*             userCtx{nullptr};
        std::atomic<u32>* counter{nullptr};
        u32               startIdx{0};
        u32               endIdx{0};
    };
    static_assert(sizeof(JobItem) == 32);

    JobSystem() noexcept = default;
    ~JobSystem() noexcept { Shutdown(); }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    [[nodiscard]] bool Init(u32 workerThreads = 0) noexcept;
    void Shutdown() noexcept;

    void ParallelFor(u32 itemCount, u32 minBatchSize, TaskFn fn, void* userCtx = nullptr) noexcept;

    template <typename Callable>
        requires (!std::convertible_to<Callable, TaskFn>)
    void ParallelFor(u32 itemCount, u32 minBatchSize, Callable&& callable) noexcept {
        using Fn = std::remove_reference_t<Callable>;
        struct Thunk {
            Fn* fn;
            static void Run(u32 s, u32 e, u32 t, void* ctx) noexcept {
                (*static_cast<Thunk*>(ctx)->fn)(s, e, t);
            }
        } thunk{std::addressof(callable)};
        ParallelFor(itemCount, minBatchSize, &Thunk::Run, &thunk);
    }

    [[nodiscard]] u32 WorkerCount() const noexcept { return m_workerCount; }

private:
    void WorkerLoop(u32 queueIdx) noexcept;
    [[nodiscard]] bool ExecuteOneJob(u32 callerQueueIdx) noexcept;
    void RunItem(const JobItem& item, u32 threadIdx) noexcept;
    [[nodiscard]] u32 CallerQueueIndex() const noexcept;

    std::array<WorkStealingDeque<JobItem, kDequeCapacity>, kQueueCount> m_deques{};
    std::array<std::thread, kMaxWorkers> m_workers{};
    std::mutex m_wakeMutex{};
    std::condition_variable m_wakeCv{};
    std::atomic<u32> m_inFlight{0};
    std::atomic<bool> m_stop{false};
    u32 m_workerCount{0};
};

} // namespace en

#pragma once

#include "core/Types.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace en {

// Atomic batch-stealing parallel-for and background async job dispatcher with zero-allocation barriers.
// Non-copyable owner of worker threads; nested ParallelFor calls execute inline on the calling worker.
// Invariant: m_workerCount <= kMaxWorkers; batch indices are claimed via lock-free atomic fetch_add.
class JobSystem {
public:
    static constexpr u32 kMaxWorkers = 16;

    using TaskFn  = void (*)(u32 startIdx, u32 endIdx, u32 threadIdx, void* userCtx) noexcept;
    using AsyncFn = void (*)(void* userCtx) noexcept;

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

    // Dispatches a single background job (e.g. room streaming validation) without blocking the caller.
    void DispatchAsync(AsyncFn fn, void* userCtx = nullptr) noexcept;
    void WaitAsync() noexcept;
    [[nodiscard]] bool IsAsyncDone() const noexcept {
        return m_asyncDone.load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 AsyncRunCount() const noexcept {
        return m_asyncRuns.load(std::memory_order_relaxed);
    }

    [[nodiscard]] u32 WorkerCount() const noexcept { return m_workerCount; }

private:
    struct Dispatch {
        TaskFn fn{nullptr};
        void*  userCtx{nullptr};
        u32    itemCount{0};
        u32    batchSize{1};
        u32    totalBatches{0};
    };

    void WorkerLoop(u32 workerIdx) noexcept;
    [[nodiscard]] bool ClaimAndExecuteBatch(u32 threadIdx) noexcept;
    [[nodiscard]] bool TryExecuteAsync() noexcept;

    std::array<std::thread, kMaxWorkers> m_workers{};
    std::mutex              m_mutex{};
    std::condition_variable m_wakeCv{};
    Dispatch                m_dispatch{};
    AsyncFn                 m_asyncFn{nullptr};
    void*                   m_asyncCtx{nullptr};
    alignas(64) std::atomic<u32> m_nextBatch{0};
    alignas(64) std::atomic<u32> m_remaining{0};
    std::atomic<u64>        m_asyncRuns{0};
    std::atomic<u32>        m_epoch{0};
    std::atomic<bool>       m_busy{false};
    std::atomic<bool>       m_asyncPending{false};
    std::atomic<bool>       m_asyncDone{true};
    std::atomic<bool>       m_stop{false};
    u32                     m_workerCount{0};
};

} // namespace en

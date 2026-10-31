#include "core/Jobs.h"

#include <algorithm>

namespace en {
namespace {
thread_local const JobSystem* g_tlsOwner = nullptr;
thread_local u32              g_tlsIdx   = 0U;
} // namespace

bool JobSystem::Init(u32 workerThreads) noexcept {
    Shutdown();
    m_nextBatch.store(0U, std::memory_order_relaxed);
    m_remaining.store(0U, std::memory_order_relaxed);
    m_asyncRuns.store(0U, std::memory_order_relaxed);
    m_epoch.store(0U, std::memory_order_relaxed);
    m_busy.store(false, std::memory_order_relaxed);
    m_asyncPending.store(false, std::memory_order_relaxed);
    m_asyncDone.store(true, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    g_tlsOwner = this;
    g_tlsIdx   = 0U;

    if (workerThreads == 0U) {
        const u32 hw = std::thread::hardware_concurrency();
        workerThreads = (hw > 1U) ? (hw - 1U) : 1U;
    }
    const u32 target = std::min(workerThreads, kMaxWorkers);
    m_workerCount = 0U;
    for (u32 i = 0U; i < target; ++i) {
        const u32 workerIdx = i + 1U;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
            m_workers[i] = std::thread([this, workerIdx]() noexcept { WorkerLoop(workerIdx); });
        } catch (...) {
            Shutdown();
            return false;
        }
#else
        m_workers[i] = std::thread([this, workerIdx]() noexcept { WorkerLoop(workerIdx); });
#endif
        m_workerCount = i + 1U;
    }
    return true;
}

void JobSystem::Shutdown() noexcept {
    WaitAsync();
    if (m_workerCount > 0U) {
        {
            std::lock_guard lock(m_mutex);
            m_stop.store(true, std::memory_order_release);
        }
        m_wakeCv.notify_all();
        for (u32 i = 0U; i < m_workerCount; ++i) {
            if (m_workers[i].joinable()) m_workers[i].join();
        }
        m_workerCount = 0U;
    }
    m_remaining.store(0U, std::memory_order_relaxed);
    m_busy.store(false, std::memory_order_relaxed);
    m_asyncPending.store(false, std::memory_order_relaxed);
    m_asyncDone.store(true, std::memory_order_relaxed);
}

bool JobSystem::TryExecuteAsync() noexcept {
    if (!m_asyncPending.exchange(false, std::memory_order_acq_rel)) return false;
    const AsyncFn fn  = m_asyncFn;
    void* const   ctx = m_asyncCtx;
    if (fn) fn(ctx);
    m_asyncRuns.fetch_add(1U, std::memory_order_relaxed);
    m_asyncDone.store(true, std::memory_order_release);
    return true;
}

void JobSystem::DispatchAsync(AsyncFn fn, void* userCtx) noexcept {
    if (!fn) return;
    WaitAsync();
    if (m_workerCount == 0U) {
        fn(userCtx);
        m_asyncRuns.fetch_add(1U, std::memory_order_relaxed);
        m_asyncDone.store(true, std::memory_order_release);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_asyncFn  = fn;
        m_asyncCtx = userCtx;
        m_asyncDone.store(false, std::memory_order_release);
        m_asyncPending.store(true, std::memory_order_release);
        m_epoch.fetch_add(1U, std::memory_order_release);
    }
    m_wakeCv.notify_one();
}

void JobSystem::WaitAsync() noexcept {
    if (m_asyncDone.load(std::memory_order_acquire)) return;
    if (TryExecuteAsync()) return;
    while (!m_asyncDone.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

bool JobSystem::ClaimAndExecuteBatch(u32 threadIdx) noexcept {
    const u32 total = m_dispatch.totalBatches;
    const u32 b = m_nextBatch.fetch_add(1U, std::memory_order_relaxed);
    if (b >= total) return false;

    const u32 start = b * m_dispatch.batchSize;
    const u32 end = (m_dispatch.itemCount - start > m_dispatch.batchSize)
                        ? (start + m_dispatch.batchSize)
                        : m_dispatch.itemCount;
    m_dispatch.fn(start, end, threadIdx, m_dispatch.userCtx);
    m_remaining.fetch_sub(1U, std::memory_order_acq_rel);
    return true;
}

void JobSystem::ParallelFor(u32 itemCount, u32 minBatchSize, TaskFn fn, void* userCtx) noexcept {
    if (itemCount == 0U || !fn) return;

    const u32 batchSize    = std::max(1U, minBatchSize);
    const u32 totalBatches = ((itemCount - 1U) / batchSize) + 1U;
    const u32 callerIdx    = (g_tlsOwner == this) ? g_tlsIdx : 0U;

    if (m_workerCount == 0U || totalBatches == 1U || callerIdx != 0U ||
        m_busy.exchange(true, std::memory_order_acq_rel)) {
        fn(0U, itemCount, callerIdx, userCtx);
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_dispatch = Dispatch{fn, userCtx, itemCount, batchSize, totalBatches};
        m_nextBatch.store(0U, std::memory_order_relaxed);
        m_remaining.store(totalBatches, std::memory_order_release);
        m_epoch.fetch_add(1U, std::memory_order_release);
    }
    m_wakeCv.notify_all();

    while (ClaimAndExecuteBatch(0U)) {}
    while (m_remaining.load(std::memory_order_acquire) > 0U) {
        std::this_thread::yield();
    }

    m_busy.store(false, std::memory_order_release);
}

void JobSystem::WorkerLoop(u32 workerIdx) noexcept {
    g_tlsOwner = this;
    g_tlsIdx   = workerIdx;
    u32 seenEpoch = 0U;

    for (;;) {
        (void)TryExecuteAsync();
        while (m_remaining.load(std::memory_order_acquire) > 0U) {
            if (!ClaimAndExecuteBatch(workerIdx)) break;
        }
        (void)TryExecuteAsync();

        std::unique_lock lock(m_mutex);
        m_wakeCv.wait(lock, [this, &seenEpoch]() noexcept {
            return m_stop.load(std::memory_order_acquire) ||
                   m_asyncPending.load(std::memory_order_acquire) ||
                   (m_epoch.load(std::memory_order_acquire) != seenEpoch &&
                    m_remaining.load(std::memory_order_acquire) > 0U);
        });
        if (m_stop.load(std::memory_order_acquire)) return;
        seenEpoch = m_epoch.load(std::memory_order_relaxed);
    }
}

} // namespace en

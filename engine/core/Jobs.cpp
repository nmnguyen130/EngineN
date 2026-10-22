#include "core/Jobs.h"

#include <algorithm>

namespace en {

namespace {
thread_local u32 g_tlsQueueIndex = 0U;
thread_local const JobSystem* g_tlsOwnerSystem = nullptr;
} // namespace

bool JobSystem::Init(u32 workerThreads) noexcept {
    Shutdown();
    for (auto& dq : m_deques) {
        dq.Clear();
    }
    m_inFlightJobs.store(0U, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    g_tlsOwnerSystem = this;
    g_tlsQueueIndex = 0U;

    const u32 clamped = std::min(workerThreads, kMaxWorkers);
    m_workerCount = clamped;
    for (u32 i = 0; i < clamped; ++i) {
        const u32 queueIdx = i + 1U;
        m_workers[i] = std::thread([this, queueIdx]() noexcept { WorkerLoop(queueIdx); });
    }
    return true;
}

void JobSystem::Shutdown() noexcept {
    if (m_workerCount > 0U) {
        {
            std::lock_guard lock(m_wakeMutex);
            m_stop.store(true, std::memory_order_release);
        }
        m_wakeCv.notify_all();
        for (u32 i = 0; i < m_workerCount; ++i) {
            if (m_workers[i].joinable()) {
                m_workers[i].join();
            }
        }
        m_workerCount = 0U;
    }
    for (auto& dq : m_deques) {
        dq.Clear();
    }
    m_inFlightJobs.store(0U, std::memory_order_relaxed);
}

u32 JobSystem::ResolveCallerQueueIndex() const noexcept {
    if (g_tlsOwnerSystem == this && g_tlsQueueIndex <= m_workerCount) {
        return g_tlsQueueIndex;
    }
    return 0U;
}

void JobSystem::ExecuteJobItem(const JobItem& item, u32 threadIdx) noexcept {
    if (item.fn) {
        item.fn(item.startIdx, item.endIdx, threadIdx, item.userCtx);
    }
    if (item.pending) {
        item.pending->fetch_sub(1U, std::memory_order_acq_rel);
    }
}

bool JobSystem::ExecuteOneJob(u32 callerQueueIdx) noexcept {
    JobItem item{};
    if (m_deques[callerQueueIdx].PopBottom(item)) {
        m_inFlightJobs.fetch_sub(1U, std::memory_order_acq_rel);
        ExecuteJobItem(item, callerQueueIdx);
        return true;
    }

    const u32 totalQueues = m_workerCount + 1U;
    for (u32 offset = 1U; offset < totalQueues; ++offset) {
        const u32 victim = (callerQueueIdx + offset) % totalQueues;
        if (m_deques[victim].StealTop(item)) {
            m_inFlightJobs.fetch_sub(1U, std::memory_order_acq_rel);
            ExecuteJobItem(item, callerQueueIdx);
            return true;
        }
    }
    return false;
}

void JobSystem::ParallelFor(u32 itemCount, u32 minBatchSize, ParallelTaskFn fn, void* userCtx) noexcept {
    if (itemCount == 0U || fn == nullptr) return;
    const u32 batchSize = std::max(1U, minBatchSize);
    const u32 totalBatches = (itemCount + batchSize - 1U) / batchSize;
    const u32 callerQueueIdx = ResolveCallerQueueIndex();

    if (m_workerCount == 0U || totalBatches == 1U) {
        fn(0U, itemCount, callerQueueIdx, userCtx);
        return;
    }

    alignas(64) std::atomic<u32> pending{totalBatches};
    u32 pushedCount = 0U;

    // Push batches 1..N-1 for workers to steal while reserving batch 0 for immediate execution.
    for (u32 b = 1U; b < totalBatches; ++b) {
        const u32 start = b * batchSize;
        const u32 end = std::min(start + batchSize, itemCount);
        const JobItem item{fn, userCtx, &pending, start, end};

        // Increment m_inFlightJobs before PushBottom so a stealing worker never underflows the counter.
        m_inFlightJobs.fetch_add(1U, std::memory_order_release);
        if (g_tlsOwnerSystem == this && m_deques[callerQueueIdx].PushBottom(item)) {
            ++pushedCount;
        } else {
            m_inFlightJobs.fetch_sub(1U, std::memory_order_relaxed);
            ExecuteJobItem(item, callerQueueIdx);
        }
    }

    if (pushedCount > 0U) {
        {
            std::lock_guard lock(m_wakeMutex);
        }
        m_wakeCv.notify_all();
    }

    // Execute batch 0 directly on the calling thread, then help drain remaining batches.
    const JobItem firstBatch{fn, userCtx, &pending, 0U, std::min(batchSize, itemCount)};
    ExecuteJobItem(firstBatch, callerQueueIdx);

    while (pending.load(std::memory_order_acquire) > 0U) {
        if (!ExecuteOneJob(callerQueueIdx)) {
            std::this_thread::yield();
        }
    }
}

void JobSystem::WorkerLoop(u32 queueIdx) noexcept {
    g_tlsOwnerSystem = this;
    g_tlsQueueIndex = queueIdx;

    for (;;) {
        while (ExecuteOneJob(queueIdx)) {
        }

        std::unique_lock lock(m_wakeMutex);
        m_wakeCv.wait(lock, [this]() noexcept {
            return m_stop.load(std::memory_order_acquire) ||
                   m_inFlightJobs.load(std::memory_order_acquire) > 0U;
        });
        if (m_stop.load(std::memory_order_acquire)) {
            return;
        }
    }
}

} // namespace en
